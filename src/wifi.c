#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
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
static char s_ap_ssid[33];
static char s_custom_ap_ssid[33];
static char s_ap_pass[65];
static char s_admin_pass[65];
static uint8_t s_sta_mac[6];
static bool s_use_static_ip;
static char s_static_ip[16];
static char s_netmask[16];
static char s_gateway[16];
static char s_dns[16];

bool wifi_parse_ipv4(const char *s, uint32_t *out)
{
    if (!s || !*s)
        return false;
    uint32_t parts[4] = {0};
    const char *p = s;
    for (int i = 0; i < 4; i++) {
        if (*p < '0' || *p > '9')
            return false;
        if (*p == '0' && p[1] >= '0' && p[1] <= '9')
            return false;
        uint32_t v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (uint32_t)(*p - '0');
            p++;
            if (++digits > 3 || v > 255)
                return false;
        }
        parts[i] = v;
        if (i < 3) {
            if (*p != '.')
                return false;
            p++;
        }
    }
    if (*p != 0)
        return false;
    if (out)
        *out = ESP_IP4TOADDR(parts[0], parts[1], parts[2], parts[3]);
    return true;
}

bool wifi_is_valid_hostname(const char *s)
{
    if (!s || !*s)
        return false;
    size_t len = strlen(s);
    if (len > 32)
        return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum && c != '-')
            return false;
        if ((i == 0 || i == len - 1) && c == '-')
            return false;
    }
    return true;
}

void wifi_sanitize_hostname(const char *src, char *dst, size_t dst_n)
{
    if (!dst || dst_n == 0)
        return;
    size_t max_out = dst_n - 1;
    if (max_out > 32)
        max_out = 32;
    size_t slen = src ? strlen(src) : 0;
    if (slen > 6) {
        static const char loc[6] = {'.', 'l', 'o', 'c', 'a', 'l'};
        bool has_local = true;
        for (size_t i = 0; i < 6; i++) {
            unsigned char c = (unsigned char)src[slen - 6 + i];
            if (c >= 'A' && c <= 'Z')
                c = (unsigned char)(c - 'A' + 'a');
            if ((char)c != loc[i]) {
                has_local = false;
                break;
            }
        }
        if (has_local)
            slen -= 6;
    }
    size_t p = 0;
    bool prev_dash = false;
    for (size_t i = 0; i < slen && p < max_out; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            dst[p++] = (char)c;
            prev_dash = false;
        } else if (p > 0 && !prev_dash) {
            dst[p++] = '-';
            prev_dash = true;
        }
    }
    while (p > 0 && dst[p - 1] == '-')
        p--;
    dst[p] = 0;
    if (p == 0) {
        const char *fb = HOSTNAME;
        if (!fb || !fb[0])
            fb = "airprint";
        size_t flen = strlen(fb);
        if (flen > max_out)
            flen = max_out;
        memcpy(dst, fb, flen);
        dst[flen] = 0;
    }
}

static void apply_static_dns(void)
{
    if (!s_sta_netif)
        return;
    uint32_t gw = 0, dns_addr = 0;
    if (!wifi_parse_ipv4(s_gateway, &gw) || !gw)
        return;
    if (!s_dns[0] || !wifi_parse_ipv4(s_dns, &dns_addr) || !dns_addr)
        dns_addr = gw;
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = dns_addr;
    esp_err_t err = esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "set static dns failed: %s", esp_err_to_name(err));
}

static void apply_static_ip(void)
{
    if (!s_sta_netif)
        return;
    uint32_t ip = 0, mask = 0, gw = 0;
    if (!wifi_parse_ipv4(s_static_ip, &ip) || !ip || !wifi_parse_ipv4(s_netmask, &mask) || !mask ||
        !wifi_parse_ipv4(s_gateway, &gw) || !gw) {
        ESP_LOGW(TAG, "invalid static IP config, falling back to DHCP");
        s_use_static_ip = false;
        esp_netif_dhcpc_start(s_sta_netif);
        return;
    }

    esp_err_t err = esp_netif_dhcpc_stop(s_sta_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "dhcpc_stop failed: %s", esp_err_to_name(err));
        return;
    }

    esp_netif_ip_info_t info = {0};
    info.ip.addr = ip;
    info.netmask.addr = mask;
    info.gw.addr = gw;
    err = esp_netif_set_ip_info(s_sta_netif, &info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set static ip failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "configured static IP %s mask %s gw %s dns %s", s_static_ip, s_netmask, s_gateway,
             s_dns[0] ? s_dns : s_gateway);
}

static void compute_ap_ssid(void)
{
    if (s_custom_ap_ssid[0])
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", s_custom_ap_ssid);
    else
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "AirPrint-Setup-%02X%02X", s_sta_mac[4], s_sta_mac[5]);
}

static void start_ap_internal(void)
{
    if (s_ap_active)
        return;
    wifi_config_t ap = {0};
    size_t slen = strlen(s_ap_ssid);
    if (slen > sizeof(ap.ap.ssid))
        slen = sizeof(ap.ap.ssid);
    memcpy(ap.ap.ssid, s_ap_ssid, slen);
    ap.ap.ssid_len = (uint8_t)slen;
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    size_t plen = strlen(s_ap_pass);
    if (plen >= 8 && plen < sizeof(ap.ap.password)) {
        memcpy(ap.ap.password, s_ap_pass, plen);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_LOGW(TAG, "starting setup AP \"%s\" at 192.168.4.1 (%s)", s_ap_ssid,
             ap.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2");
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
        if (s_use_static_ip)
            apply_static_dns();
        ESP_LOGI(TAG, "connected, IP " IPSTR " (%s)", IP2STR(&e->ip_info.ip), s_use_static_ip ? "static" : "DHCP");
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
    uint8_t st = 0;
    if (nvs_get_u8(h, "static", &st) == ESP_OK)
        out->use_static_ip = st != 0;
    l = sizeof(out->static_ip);
    if (nvs_get_str(h, "ip", out->static_ip, &l) != ESP_OK)
        out->static_ip[0] = 0;
    l = sizeof(out->netmask);
    if (nvs_get_str(h, "mask", out->netmask, &l) != ESP_OK)
        out->netmask[0] = 0;
    l = sizeof(out->gateway);
    if (nvs_get_str(h, "gw", out->gateway, &l) != ESP_OK)
        out->gateway[0] = 0;
    l = sizeof(out->dns);
    if (nvs_get_str(h, "dns", out->dns, &l) != ESP_OK)
        out->dns[0] = 0;
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
    if (err == ESP_OK && cfg->hostname[0]) {
        char clean_host[33];
        wifi_sanitize_hostname(cfg->hostname, clean_host, sizeof(clean_host));
        err = nvs_set_str(h, "host", clean_host);
    }
    if (err == ESP_OK && cfg->printer_name[0])
        err = nvs_set_str(h, "pname", cfg->printer_name);
    if (err == ESP_OK)
        err = nvs_set_u8(h, "static", cfg->use_static_ip ? 1 : 0);
    if (err == ESP_OK)
        err = nvs_set_str(h, "ip", cfg->static_ip);
    if (err == ESP_OK)
        err = nvs_set_str(h, "mask", cfg->netmask);
    if (err == ESP_OK)
        err = nvs_set_str(h, "gw", cfg->gateway);
    if (err == ESP_OK)
        err = nvs_set_str(h, "dns", cfg->dns);
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

bool wifi_load_sec_config(wifi_sec_cfg_t *out)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return false;
    size_t l = sizeof(out->ap_ssid);
    if (nvs_get_str(h, "ap_ssid", out->ap_ssid, &l) != ESP_OK)
        out->ap_ssid[0] = 0;
    l = sizeof(out->ap_password);
    if (nvs_get_str(h, "ap_pass", out->ap_password, &l) != ESP_OK)
        out->ap_password[0] = 0;
    l = sizeof(out->admin_password);
    if (nvs_get_str(h, "adm_pass", out->admin_password, &l) != ESP_OK)
        out->admin_password[0] = 0;
    nvs_close(h);
    return out->ap_ssid[0] || out->ap_password[0] || out->admin_password[0];
}

esp_err_t wifi_save_sec_config(const wifi_sec_cfg_t *cfg)
{
    if (!cfg)
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs open");
    esp_err_t err = nvs_set_str(h, "ap_ssid", cfg->ap_ssid);
    if (err == ESP_OK)
        err = nvs_set_str(h, "ap_pass", cfg->ap_password);
    if (err == ESP_OK)
        err = nvs_set_str(h, "adm_pass", cfg->admin_password);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        snprintf(s_custom_ap_ssid, sizeof(s_custom_ap_ssid), "%s", cfg->ap_ssid);
        snprintf(s_ap_pass, sizeof(s_ap_pass), "%s", cfg->ap_password);
        snprintf(s_admin_pass, sizeof(s_admin_pass), "%s", cfg->admin_password);
        compute_ap_ssid();
    }
    return err;
}

bool wifi_admin_protected(void)
{
    return s_admin_pass[0] != 0;
}

bool wifi_check_admin(const char *candidate)
{
    if (!s_admin_pass[0])
        return true;
    if (!candidate)
        return false;
    size_t la = strlen(s_admin_pass);
    size_t lb = strlen(candidate);
    uint8_t diff = la != lb ? 1 : 0;
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++)
        diff |= (uint8_t)(s_admin_pass[i] ^ candidate[i]);
    return diff == 0;
}

bool wifi_ap_has_password(void)
{
    return strlen(s_ap_pass) >= 8;
}

void wifi_get_custom_ap_ssid(char *buf, size_t n)
{
    if (buf && n)
        snprintf(buf, n, "%s", s_custom_ap_ssid);
}

esp_err_t wifi_start(const char *default_hostname)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs");

    esp_read_mac(s_sta_mac, ESP_MAC_WIFI_STA);
    wifi_sec_cfg_t sec;
    wifi_load_sec_config(&sec);
    snprintf(s_custom_ap_ssid, sizeof(s_custom_ap_ssid), "%s", sec.ap_ssid);
    snprintf(s_ap_pass, sizeof(s_ap_pass), "%s", sec.ap_password);
    snprintf(s_admin_pass, sizeof(s_admin_pass), "%s", sec.admin_password);
    compute_ap_ssid();

    wifi_stored_cfg_t stored;
    bool has_nvs = wifi_load_config(&stored);
    snprintf(s_ssid, sizeof(s_ssid), "%s", has_nvs ? stored.ssid : WIFI_SSID);
    snprintf(s_password, sizeof(s_password), "%s", has_nvs ? stored.password : WIFI_PASSWORD);
    const char *raw_host =
        (has_nvs && stored.hostname[0]) ? stored.hostname
                                        : (default_hostname && default_hostname[0] ? default_hostname : HOSTNAME);
    wifi_sanitize_hostname(raw_host, s_hostname, sizeof(s_hostname));
    if (strcmp(raw_host, s_hostname) != 0)
        ESP_LOGW(TAG, "sanitized invalid hostname \"%s\" -> \"%s\" for mDNS/AirPrint", raw_host, s_hostname);
    snprintf(s_printer_name, sizeof(s_printer_name), "%s",
             (has_nvs && stored.printer_name[0]) ? stored.printer_name : PRINTER_NAME);
    if (has_nvs) {
        s_use_static_ip = stored.use_static_ip;
        snprintf(s_static_ip, sizeof(s_static_ip), "%s", stored.static_ip);
        snprintf(s_netmask, sizeof(s_netmask), "%s", stored.netmask);
        snprintf(s_gateway, sizeof(s_gateway), "%s", stored.gateway);
        snprintf(s_dns, sizeof(s_dns), "%s", stored.dns);
    }

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
        if (s_use_static_ip)
            apply_static_ip();
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

void wifi_get_static_cfg(bool *use_static, char *ip, size_t ip_n, char *mask, size_t mask_n, char *gw, size_t gw_n,
                         char *dns, size_t dns_n)
{
    if (use_static)
        *use_static = s_use_static_ip;
    if (ip && ip_n)
        snprintf(ip, ip_n, "%s", s_static_ip);
    if (mask && mask_n)
        snprintf(mask, mask_n, "%s", s_netmask);
    if (gw && gw_n)
        snprintf(gw, gw_n, "%s", s_gateway);
    if (dns && dns_n)
        snprintf(dns, dns_n, "%s", s_dns);
}

bool wifi_sta_has_password(void)
{
    return s_password[0] != 0;
}

void wifi_preserve_sta_password(const char *ssid, char *pass_out, size_t pass_n)
{
    if (ssid && pass_out && pass_n && !pass_out[0] && s_password[0] && strcmp(s_ssid, ssid) == 0)
        snprintf(pass_out, pass_n, "%s", s_password);
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
