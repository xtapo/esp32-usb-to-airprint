#include "wifi_portal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "print_job.h"
#include "printer_driver.h"
#include "wifi.h"

static const char *TAG = "wifi_portal";

static volatile bool s_dns_run;
static int s_dns_sock = -1;
static TaskHandle_t s_dns_task;
static esp_timer_handle_t s_reboot_timer;

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t pkt[512];
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(s_dns_sock, pkt, sizeof(pkt) - 16, 0, (struct sockaddr *)&from, &flen);
        if (n < 12) {
            if (!s_dns_run)
                break;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        // Standard DNS query with >= 1 question
        if ((pkt[2] & 0x80u) != 0)
            continue;
        int qend = 12;
        while (qend < n && pkt[qend] != 0)
            qend += 1 + pkt[qend];
        if (qend + 5 > n)
            continue;
        qend += 5; // null label + QTYPE (2) + QCLASS (2)

        uint8_t resp[512];
        if (qend + 16 > (int)sizeof(resp))
            continue;
        memcpy(resp, pkt, (size_t)qend);
        resp[2] = 0x81; // QR=1, RD=1
        resp[3] = 0x80; // RA=1, RCODE=0
        resp[4] = 0; resp[5] = 1; // QDCOUNT = 1
        resp[6] = 0; resp[7] = 1; // ANCOUNT = 1
        resp[8] = 0; resp[9] = 0;
        resp[10] = 0; resp[11] = 0;
        // Answer: pointer to QNAME at offset 12, TYPE A, CLASS IN, TTL 60, RDLEN 4, 192.168.4.1
        uint8_t ans[16] = {
            0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x04, 192, 168, 4, 1,
        };
        memcpy(resp + qend, ans, sizeof(ans));
        sendto(s_dns_sock, resp, (size_t)(qend + 16), 0, (struct sockaddr *)&from, flen);
    }
    vTaskDelete(NULL);
}

void wifi_portal_dns_start(void)
{
    if (s_dns_run)
        return;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
        return;
    struct timeval tv = {.tv_sec = 0, .tv_usec = 300000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = INADDR_ANY};
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return;
    }
    s_dns_sock = fd;
    s_dns_run = true;
    if (xTaskCreate(dns_task, "portal_dns", 3072, NULL, 3, &s_dns_task) != pdPASS) {
        s_dns_run = false;
        close(fd);
        s_dns_sock = -1;
    }
}

void wifi_portal_dns_stop(void)
{
    if (!s_dns_run)
        return;
    s_dns_run = false;
    if (s_dns_sock >= 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
    }
}

static void reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static void schedule_reboot(void)
{
    if (!s_reboot_timer) {
        const esp_timer_create_args_t args = {.callback = reboot_cb, .name = "wifi_reboot"};
        esp_timer_create(&args, &s_reboot_timer);
    }
    if (s_reboot_timer)
        esp_timer_start_once(s_reboot_timer, 1200000ULL);
}

// Extracts a string value for `"key":"value"` from small JSON payloads.
static bool json_get_str(const char *json, const char *key, char *out, size_t n)
{
    out[0] = 0;
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p)
        return false;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == '\t' || *p == ':'))
        p++;
    if (*p != '"')
        return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < n - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n')
                out[i++] = '\n';
            else if (*p == 'r')
                out[i++] = '\r';
            else if (*p == 't')
                out[i++] = '\t';
            else
                out[i++] = *p;
            p++;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = 0;
    return true;
}

static bool is_captive_probe(const char *path)
{
    static const char *const probes[] = {
        "/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
        "/ncsi.txt", "/connecttest.txt", "/redirect", "/canonical.html", "/fwlink",
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++)
        if (strcmp(path, probes[i]) == 0)
            return true;
    return false;
}

bool wifi_portal_http(airprint_http_t *h, const char *method, const char *path)
{
    bool get = strcmp(method, "GET") == 0;
    bool post = strcmp(method, "POST") == 0;

    if (get && wifi_ap_active() && is_captive_probe(path)) {
        char ip[24];
        wifi_get_ap_ip(ip, sizeof(ip));
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "Location: http://%s/#/wifi\r\nCache-Control: no-store\r\n", ip);
        airprint_http_send_ex(h, "302 Found", "text/plain", hdr, "", 0);
        return true;
    }

    if (get && strcmp(path, "/api/wifi") == 0) {
        char *buf = malloc(2048);
        if (!buf) {
            airprint_http_send(h, "500 Internal Server Error", "text/plain", "no memory", 9);
            return true;
        }
        char ssid[33], ip[24], ap_ssid[24], ap_ip[24];
        wifi_get_sta_ssid(ssid, sizeof(ssid));
        wifi_get_ip(ip, sizeof(ip));
        wifi_get_ap_ssid(ap_ssid, sizeof(ap_ssid));
        wifi_get_ap_ip(ap_ip, sizeof(ap_ip));
        const printer_driver_t *drv = print_job_driver();
        size_t p = 0;
        p += (size_t)snprintf(buf + p, 2048 - p,
                              "{\"connected\":%s,\"ap_active\":%s,\"rssi\":%d,\"ssid\":",
                              wifi_connected() ? "true" : "false", wifi_ap_active() ? "true" : "false", wifi_rssi());
        p = events_json_str(buf, p, 2048, ssid);
        p += (size_t)snprintf(buf + p, 2048 - p, ",\"ip\":");
        p = events_json_str(buf, p, 2048, ip);
        p += (size_t)snprintf(buf + p, 2048 - p, ",\"ap_ssid\":");
        p = events_json_str(buf, p, 2048, ap_ssid);
        p += (size_t)snprintf(buf + p, 2048 - p, ",\"ap_ip\":");
        p = events_json_str(buf, p, 2048, ap_ip);
        p += (size_t)snprintf(buf + p, 2048 - p, ",\"hostname\":");
        p = events_json_str(buf, p, 2048, wifi_effective_hostname());
        p += (size_t)snprintf(buf + p, 2048 - p, ",\"printer_name\":");
        p = events_json_str(buf, p, 2048, wifi_effective_printer_name());
        p += (size_t)snprintf(buf + p, 2048 - p, ",\"driver\":");
        p = events_json_str(buf, p, 2048, drv ? drv->id : "");
        p += (size_t)snprintf(buf + p, 2048 - p, ",\"model\":");
        p = events_json_str(buf, p, 2048, drv ? drv->make_model : "");
        int n_drv = 0;
        while (printer_drivers[n_drv])
            n_drv++;
        p += (size_t)snprintf(buf + p, 2048 - p, ",\"driver_count\":%d}", n_drv);
        airprint_http_send_ex(h, "200 OK", "application/json", "Cache-Control: no-store\r\n", buf, p);
        free(buf);
        return true;
    }

    if (get && strcmp(path, "/api/wifi/scan") == 0) {
        char *buf = malloc(2048);
        if (!buf) {
            airprint_http_send(h, "500 Internal Server Error", "text/plain", "no memory", 9);
            return true;
        }
        int n = wifi_scan_json(buf, 2048);
        airprint_http_send_ex(h, "200 OK", "application/json", "Cache-Control: no-store\r\n", buf, (size_t)n);
        free(buf);
        return true;
    }

    if (post && strcmp(path, "/api/wifi") == 0) {
        char body[512];
        int n = airprint_http_read_body(h, body, sizeof(body) - 1);
        if (n <= 0) {
            airprint_http_send(h, "400 Bad Request", "application/json", "{\"error\":\"empty body\"}", 22);
            return true;
        }
        body[n] = 0;
        wifi_stored_cfg_t cfg = {0};
        if (!json_get_str(body, "ssid", cfg.ssid, sizeof(cfg.ssid)) || !cfg.ssid[0]) {
            airprint_http_send(h, "400 Bad Request", "application/json", "{\"error\":\"ssid required\"}", 25);
            return true;
        }
        json_get_str(body, "password", cfg.password, sizeof(cfg.password));
        json_get_str(body, "hostname", cfg.hostname, sizeof(cfg.hostname));
        json_get_str(body, "printer_name", cfg.printer_name, sizeof(cfg.printer_name));
        if (wifi_save_config(&cfg) != ESP_OK) {
            airprint_http_send(h, "500 Internal Server Error", "application/json", "{\"error\":\"save failed\"}", 23);
            return true;
        }
        ESP_LOGI(TAG, "saved WiFi config for SSID \"%s\", rebooting", cfg.ssid);
        airprint_http_send(h, "200 OK", "application/json", "{\"ok\":true,\"rebooting\":true}", 28);
        schedule_reboot();
        return true;
    }

    if (post && strcmp(path, "/api/wifi/reset") == 0) {
        wifi_clear_config();
        ESP_LOGW(TAG, "cleared WiFi config, rebooting into setup AP");
        airprint_http_send(h, "200 OK", "application/json", "{\"ok\":true,\"rebooting\":true}", 28);
        schedule_reboot();
        return true;
    }

    return false;
}
