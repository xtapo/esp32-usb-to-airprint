#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "local_config.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "wifi_portal.h"

static const char *TAG = "wifi_sta";
static const char *NVS_NS = "wifi_cfg";

static volatile bool s_connected;
static volatile bool s_ap_active;
static int s_retries;
static bool s_ever_connected;
static bool s_sta_configured;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_timer_handle_t s_fallback_timer;

static char s_ssid[33];
static char s_password[65];
static char s_hostname[33];
static char s_printer_name[65];
static char s_ap_ssid[24];

static void start_ap_internal(void)
{
    if (s_ap_active)
        return;
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", s_ap_ssid);
    ap.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    ESP_LOGW(TAG, "starting setup AP \"%s\" at 192.168.4.1", s_ap_ssid);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    s_ap_active = true;
    wifi_portal_dns_start();
}

static void stop_ap_internal(void)
{
    if (!s_ap_active)
        return;
    wifi_portal_dns_stop();
    s_ap_active = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "setup AP stopped");
}

static void fallback_timer_cb(void *arg)
{
    (void)arg;
    if (!s_connected && !s_ap_active)
        start_ap_internal();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_sta_configured)
            esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_connected = false;
        if (s_sta_configured) {
            if (s_retries++ % 10 == 0)
                ESP_LOGW(TAG, "disconnected (reason %d), retrying", d->reason);
            if (!s_ever_connected && s_retries >= 8 && !s_ap_active)
                start_ap_internal();
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        s_connected = true;
        s_ever_connected = true;
        if (s_fallback_timer)
            esp_timer_stop(s_fallback_timer);
        if (s_ap_active)
            stop_ap_internal();
    }
}

bool wifi_load_config(wifi_stored_cfg_t *out)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return false;
    size_t l = sizeof(out->ssid);
    bool ok = nvs_get_str(h, "ssid", out->ssid, &l) == ESP_OK && out->ssid[0] != 0;
    l = sizeof(out->password);
    if (nvs_get_str(h, "pass", out->password, &l) != ESP_OK)
        out->password[0] = 0;
    l = sizeof(out->hostname);
    if (nvs_get_str(h, "host", out->hostname, &l) != ESP_OK)
        out->hostname[0] = 0;
    l = sizeof(out->printer_name);
    if (nvs_get_str(h, "pname", out->printer_name, &l) != ESP_OK)
        out->printer_name[0] = 0;
    nvs_close(h);
    return ok;
}

esp_err_t wifi_save_config(const wifi_stored_cfg_t *cfg)
{
    if (!cfg || !cfg->ssid[0])
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs open");
    esp_err_t err = nvs_set_str(h, "ssid", cfg->ssid);
    if (err == ESP_OK)
        err = nvs_set_str(h, "pass", cfg->password);
    if (err == ESP_OK && cfg->hostname[0])
        err = nvs_set_str(h, "host", cfg->hostname);
    if (err == ESP_OK && cfg->printer_name[0])
        err = nvs_set_str(h, "pname", cfg->printer_name);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t wifi_clear_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        return ESP_OK;
    ESP_RETURN_ON_ERROR(err, TAG, "nvs open");
    err = nvs_erase_all(h);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t wifi_start(const char *default_hostname)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs");

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "AirPrint-Setup-%02X%02X", mac[4], mac[5]);

    wifi_stored_cfg_t stored;
    bool has_nvs = wifi_load_config(&stored);
    snprintf(s_ssid, sizeof(s_ssid), "%s", has_nvs ? stored.ssid : WIFI_SSID);
    snprintf(s_password, sizeof(s_password), "%s", has_nvs ? stored.password : WIFI_PASSWORD);
    snprintf(s_hostname, sizeof(s_hostname), "%s",
             (has_nvs && stored.hostname[0]) ? stored.hostname
                                             : (default_hostname && default_hostname[0] ? default_hostname : HOSTNAME));
    snprintf(s_printer_name, sizeof(s_printer_name), "%s",
             (has_nvs && stored.printer_name[0]) ? stored.printer_name : PRINTER_NAME);

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, s_hostname);

    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    s_sta_configured = s_ssid[0] != 0;
    if (s_sta_configured) {
        wifi_config_t cfg = {0};
        strncpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid));
        strncpy((char *)cfg.sta.password, s_password, sizeof(cfg.sta.password));
        cfg.sta.threshold.authmode = s_password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
        cfg.sta.pmf_cfg.capable = true;
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "config");
        esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");

        const esp_timer_create_args_t targs = {.callback = fallback_timer_cb, .name = "wifi_ap_fb"};
        if (esp_timer_create(&targs, &s_fallback_timer) == ESP_OK)
            esp_timer_start_once(s_fallback_timer, 15000000ULL);
        ESP_LOGI(TAG, "connecting as %s", s_hostname);
    } else {
        ESP_LOGW(TAG, "no WiFi SSID configured: starting captive portal");
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "mode");
        esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
        start_ap_internal();
    }

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
    return ESP_OK;
}

esp_err_t wifi_start_portal(void)
{
    start_ap_internal();
    return ESP_OK;
}

bool wifi_connected(void)
{
    return s_connected;
}

bool wifi_ap_active(void)
{
    return s_ap_active;
}

void wifi_get_ap_ssid(char *buf, size_t n)
{
    snprintf(buf, n, "%s", s_ap_ssid);
}

void wifi_get_ap_ip(char *buf, size_t n)
{
    esp_netif_ip_info_t ip;
    if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK)
        snprintf(buf, n, IPSTR, IP2STR(&ip.ip));
    else
        snprintf(buf, n, "192.168.4.1");
}

void wifi_get_sta_ssid(char *buf, size_t n)
{
    snprintf(buf, n, "%s", s_ssid);
}

bool wifi_get_ip(char *buf, size_t n)
{
    esp_netif_ip_info_t ip;
    if (!s_connected || !s_sta_netif || esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK) {
        if (n)
            buf[0] = 0;
        return false;
    }
    snprintf(buf, n, IPSTR, IP2STR(&ip.ip));
    return true;
}

int wifi_rssi(void)
{
    int rssi = 0;
    if (!s_connected || esp_wifi_sta_get_rssi(&rssi) != ESP_OK)
        return 0;
    return rssi;
}

const char *wifi_effective_hostname(void)
{
    return s_hostname[0] ? s_hostname : HOSTNAME;
}

const char *wifi_effective_printer_name(void)
{
    return s_printer_name[0] ? s_printer_name : PRINTER_NAME;
}

int wifi_scan_json(char *buf, size_t n)
{
    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK)
        return snprintf(buf, n, "{\"networks\":[]}");
    uint16_t count = 20;
    wifi_ap_record_t recs[20];
    if (esp_wifi_scan_get_ap_records(&count, recs) != ESP_OK)
        count = 0;

    size_t p = (size_t)snprintf(buf, n, "{\"networks\":[");
    int emitted = 0;
    for (uint16_t i = 0; i < count && p + 96 < n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0])
            continue;
        bool dup = false;
        for (uint16_t j = 0; j < i; j++) {
            if (strcmp((const char *)recs[j].ssid, ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        p += (size_t)snprintf(buf + p, n - p, "%s{\"ssid\":\"", emitted++ ? "," : "");
        for (const char *s = ssid; *s && p + 32 < n; s++) {
            if (*s == '"' || *s == '\\')
                buf[p++] = '\\';
            if ((unsigned char)*s >= 0x20)
                buf[p++] = *s;
        }
        p += (size_t)snprintf(buf + p, n - p, "\",\"rssi\":%d,\"secure\":%s}", recs[i].rssi,
                              recs[i].authmode != WIFI_AUTH_OPEN ? "true" : "false");
    }
    p += (size_t)snprintf(buf + p, n - p, "]}");
    return (int)p;
}
