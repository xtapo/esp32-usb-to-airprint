#include "wifi_portal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "print_job.h"
#include "printer_driver.h"
#include "usb_printer.h"
#include "wifi.h"

static const char *TAG = "wifi_portal";
static volatile bool s_ota_running;

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

static bool json_get_bool(const char *json, const char *key, bool *out)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p)
        return false;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == '\t' || *p == ':'))
        p++;
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
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

static bool require_admin(airprint_http_t *h)
{
    if (wifi_check_admin(airprint_http_admin_password(h)))
        return true;
    static const char msg[] = "{\"error\":\"admin password required\",\"auth_required\":true}";
    airprint_http_send_ex(h, "401 Unauthorized", "application/json", "Cache-Control: no-store\r\n", msg,
                          sizeof(msg) - 1);
    return false;
}

static const char *ota_state_str(const esp_partition_t *p)
{
    esp_ota_img_states_t st;
    if (!p || esp_ota_get_state_partition(p, &st) != ESP_OK)
        return "flashed";
    switch (st) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "unknown";
    }
}

static const char *check_ota_image(const uint8_t *p, size_t n)
{
    const size_t desc_ofs = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (n < desc_ofs + sizeof(esp_app_desc_t))
        return "image too small";
    const esp_image_header_t *ih = (const esp_image_header_t *)p;
    if (ih->magic != ESP_IMAGE_HEADER_MAGIC)
        return "not an ESP app image (upload firmware.bin, not factory.bin / universal-airprint.bin)";
    if (ih->chip_id != ESP_CHIP_ID_ESP32S3)
        return "image is not for ESP32-S3";
    const esp_app_desc_t *d = (const esp_app_desc_t *)(p + desc_ofs);
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD)
        return "missing ESP-IDF app description";
    if (strncmp(d->project_name, esp_app_get_description()->project_name, sizeof(d->project_name)) != 0)
        return "image belongs to another project";
    return NULL;
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
        char ssid[33], ip[24], ap_ssid[33], ap_ip[24], custom_ap[33];
        char st_ip[16], st_mask[16], st_gw[16], st_dns[16];
        bool use_static = false;
        wifi_get_sta_ssid(ssid, sizeof(ssid));
        wifi_get_ip(ip, sizeof(ip));
        wifi_get_ap_ssid(ap_ssid, sizeof(ap_ssid));
        wifi_get_ap_ip(ap_ip, sizeof(ap_ip));
        wifi_get_custom_ap_ssid(custom_ap, sizeof(custom_ap));
        wifi_get_static_cfg(&use_static, st_ip, sizeof(st_ip), st_mask, sizeof(st_mask), st_gw, sizeof(st_gw), st_dns,
                            sizeof(st_dns));
        const printer_driver_t *drv = print_job_driver();
        const esp_app_desc_t *app = esp_app_get_description();
        const esp_partition_t *run = esp_ota_get_running_partition();
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
        p += (size_t)snprintf(buf + p, 2048 - p, ",\"use_static_ip\":%s,\"static_ip\":", use_static ? "true" : "false");
        p = events_json_str(buf, p, 2048, st_ip);
        p += (size_t)snprintf(buf + p, 2048 - p, ",\"netmask\":");
        p = events_json_str(buf, p, 2048, st_mask);
        p += (size_t)snprintf(buf + p, 2048 - p, ",\"gateway\":");
        p = events_json_str(buf, p, 2048, st_gw);
        p += (size_t)snprintf(buf + p, 2048 - p, ",\"dns\":");
        p = events_json_str(buf, p, 2048, st_dns);
        p += (size_t)snprintf(buf + p, 2048 - p,
                              ",\"has_password\":%s,\"admin_protected\":%s,\"ap_has_password\":%s,\"ap_custom_ssid\":",
                              wifi_sta_has_password() ? "true" : "false",
                              wifi_admin_protected() ? "true" : "false", wifi_ap_has_password() ? "true" : "false");
        p = events_json_str(buf, p, 2048, custom_ap);
        p += (size_t)snprintf(buf + p, 2048 - p, ",\"fw_version\":");
        p = events_json_str(buf, p, 2048, app->version);
        p += (size_t)snprintf(buf + p, 2048 - p, ",\"fw_date\":");
        p = events_json_str(buf, p, 2048, app->date);
        p += (size_t)snprintf(buf + p, 2048 - p, ",\"fw_partition\":");
        p = events_json_str(buf, p, 2048, run ? run->label : "");
        p += (size_t)snprintf(buf + p, 2048 - p, ",\"ota_state\":");
        p = events_json_str(buf, p, 2048, ota_state_str(run));
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
        if (!require_admin(h))
            return true;
        char body[768];
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
        bool clear_password = false;
        json_get_bool(body, "clear_password", &clear_password);
        if (!cfg.password[0] && !clear_password)
            wifi_preserve_sta_password(cfg.ssid, cfg.password, sizeof(cfg.password));
        json_get_str(body, "hostname", cfg.hostname, sizeof(cfg.hostname));
        json_get_str(body, "printer_name", cfg.printer_name, sizeof(cfg.printer_name));
        json_get_bool(body, "use_static_ip", &cfg.use_static_ip);
        json_get_str(body, "static_ip", cfg.static_ip, sizeof(cfg.static_ip));
        json_get_str(body, "netmask", cfg.netmask, sizeof(cfg.netmask));
        json_get_str(body, "gateway", cfg.gateway, sizeof(cfg.gateway));
        json_get_str(body, "dns", cfg.dns, sizeof(cfg.dns));
        if (cfg.use_static_ip) {
            uint32_t ip = 0, mask = 0, gw = 0, dns = 0;
            if (!wifi_parse_ipv4(cfg.static_ip, &ip) || !ip) {
                airprint_http_send(h, "400 Bad Request", "application/json", "{\"error\":\"invalid static IP\"}", 29);
                return true;
            }
            if (!wifi_parse_ipv4(cfg.netmask, &mask) || !mask) {
                airprint_http_send(h, "400 Bad Request", "application/json", "{\"error\":\"invalid subnet mask\"}", 31);
                return true;
            }
            if (!wifi_parse_ipv4(cfg.gateway, &gw) || !gw) {
                airprint_http_send(h, "400 Bad Request", "application/json", "{\"error\":\"invalid gateway\"}", 27);
                return true;
            }
            if (cfg.dns[0] && (!wifi_parse_ipv4(cfg.dns, &dns) || !dns)) {
                airprint_http_send(h, "400 Bad Request", "application/json", "{\"error\":\"invalid DNS server\"}", 30);
                return true;
            }
        }
        if (wifi_save_config(&cfg) != ESP_OK) {
            airprint_http_send(h, "500 Internal Server Error", "application/json", "{\"error\":\"save failed\"}", 23);
            return true;
        }
        ESP_LOGI(TAG, "saved WiFi config for SSID \"%s\" (%s), rebooting", cfg.ssid,
                 cfg.use_static_ip ? cfg.static_ip : "DHCP");
        airprint_http_send(h, "200 OK", "application/json", "{\"ok\":true,\"rebooting\":true}", 28);
        schedule_reboot();
        return true;
    }

    if (post && strcmp(path, "/api/wifi/reset") == 0) {
        if (!require_admin(h))
            return true;
        wifi_clear_config();
        ESP_LOGW(TAG, "cleared WiFi config, rebooting into setup AP");
        airprint_http_send(h, "200 OK", "application/json", "{\"ok\":true,\"rebooting\":true}", 28);
        schedule_reboot();
        return true;
    }

    if (post && strcmp(path, "/api/security") == 0) {
        if (!require_admin(h))
            return true;
        char body[768];
        int n = airprint_http_read_body(h, body, sizeof(body) - 1);
        if (n <= 0) {
            airprint_http_send(h, "400 Bad Request", "application/json", "{\"error\":\"empty body\"}", 22);
            return true;
        }
        body[n] = 0;

        wifi_sec_cfg_t sec = {0};
        wifi_load_sec_config(&sec);
        char old_ssid[33], old_pass[65];
        snprintf(old_ssid, sizeof(old_ssid), "%s", sec.ap_ssid);
        snprintf(old_pass, sizeof(old_pass), "%s", sec.ap_password);

        char tmp_ssid[33];
        if (json_get_str(body, "ap_ssid", tmp_ssid, sizeof(tmp_ssid)))
            memcpy(sec.ap_ssid, tmp_ssid, sizeof(sec.ap_ssid));

        char tmp[65];

        bool clear_ap_pass = false, clear_admin = false;
        json_get_bool(body, "clear_ap_password", &clear_ap_pass);
        json_get_bool(body, "clear_admin_password", &clear_admin);

        if (clear_ap_pass) {
            sec.ap_password[0] = 0;
        } else if (json_get_str(body, "ap_password", tmp, sizeof(tmp)) && tmp[0]) {
            size_t len = strlen(tmp);
            if (len < 8 || len > 63) {
                airprint_http_send(h, "400 Bad Request", "application/json",
                                   "{\"error\":\"AP password must be 8-63 characters\"}", 48);
                return true;
            }
            snprintf(sec.ap_password, sizeof(sec.ap_password), "%s", tmp);
        }

        if (clear_admin) {
            sec.admin_password[0] = 0;
        } else if (json_get_str(body, "new_admin_password", tmp, sizeof(tmp)) && tmp[0]) {
            snprintf(sec.admin_password, sizeof(sec.admin_password), "%s", tmp);
        }

        if (wifi_save_sec_config(&sec) != ESP_OK) {
            airprint_http_send(h, "500 Internal Server Error", "application/json", "{\"error\":\"save failed\"}", 23);
            return true;
        }

        bool ap_changed = strcmp(old_ssid, sec.ap_ssid) != 0 || strcmp(old_pass, sec.ap_password) != 0;
        bool rebooting = ap_changed && wifi_ap_active();
        ESP_LOGI(TAG, "saved security config (admin=%s, ap_pass=%s%s)",
                 sec.admin_password[0] ? "on" : "off", sec.ap_password[0] ? "on" : "off",
                 rebooting ? ", rebooting AP" : "");
        char resp[64];
        int rn = snprintf(resp, sizeof(resp), "{\"ok\":true,\"rebooting\":%s}", rebooting ? "true" : "false");
        airprint_http_send(h, "200 OK", "application/json", resp, (size_t)rn);
        if (rebooting)
            schedule_reboot();
        return true;
    }

    if (post && strcmp(path, "/api/ota") == 0) {
        if (!require_admin(h))
            return true;
        if (s_ota_running || usb_printer_job_active()) {
            airprint_http_send(h, "409 Conflict", "application/json",
                               "{\"error\":\"device busy printing/scanning/updating\"}", 50);
            return true;
        }
        const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
        if (!part) {
            airprint_http_send(h, "500 Internal Server Error", "application/json",
                               "{\"error\":\"no OTA partition available\"}", 38);
            return true;
        }
        int64_t total = airprint_http_content_length(h);
        if (total < 4096 || total > (int64_t)part->size) {
            airprint_http_send(h, "400 Bad Request", "application/json",
                               "{\"error\":\"invalid firmware size (upload firmware.bin)\"}", 54);
            return true;
        }
        uint8_t *buf = malloc(4096);
        if (!buf) {
            airprint_http_send(h, "500 Internal Server Error", "application/json", "{\"error\":\"no memory\"}", 21);
            return true;
        }

        s_ota_running = true;
        const char *err = NULL;
        esp_ota_handle_t ota_h = 0;
        int64_t done = 0;
        int last_pct = -1;
        ESP_LOGI(TAG, "web OTA: receiving %lld bytes into %s", (long long)total, part->label);
        while (done < total && !err) {
            size_t want = (total - done) < 4096 ? (size_t)(total - done) : 4096;
            int got = airprint_http_read_body(h, buf, want);
            if (got <= 0) {
                err = "upload interrupted";
                break;
            }
            if (done == 0) {
                err = check_ota_image(buf, (size_t)got);
                if (!err && esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota_h) != ESP_OK)
                    err = "cannot begin OTA write";
                if (err)
                    break;
            }
            if (esp_ota_write(ota_h, buf, (size_t)got) != ESP_OK) {
                err = "flash write failed";
                break;
            }
            done += got;
            int pct = (int)(done * 10 / total) * 10;
            if (pct != last_pct) {
                last_pct = pct;
                ESP_LOGI(TAG, "web OTA: %d%%", pct);
            }
        }
        free(buf);
        if (!err) {
            esp_err_t e = esp_ota_end(ota_h);
            ota_h = 0;
            if (e == ESP_ERR_OTA_VALIDATE_FAILED)
                err = "image validation failed (corrupted)";
            else if (e != ESP_OK)
                err = "OTA finalize failed";
            else if (esp_ota_set_boot_partition(part) != ESP_OK)
                err = "cannot set boot partition";
        }
        if (ota_h)
            esp_ota_abort(ota_h);
        s_ota_running = false;
        if (err) {
            ESP_LOGW(TAG, "web OTA failed: %s", err);
            char resp[192];
            int rn = snprintf(resp, sizeof(resp), "{\"error\":\"%s\"}", err);
            airprint_http_send(h, "400 Bad Request", "application/json", resp, (size_t)rn);
            return true;
        }
        ESP_LOGW(TAG, "web OTA written %lld bytes to %s, rebooting", (long long)done, part->label);
        char resp[128];
        int rn = snprintf(resp, sizeof(resp), "{\"ok\":true,\"rebooting\":true,\"partition\":\"%s\",\"bytes\":%lld}",
                          part->label, (long long)done);
        airprint_http_send(h, "200 OK", "application/json", resp, (size_t)rn);
        schedule_reboot();
        return true;
    }

    return false;
}
