// WiFi station + NVS provisioning + SoftAP fallback portal.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    char ssid[33];
    char password[65];
    char hostname[33];
    char printer_name[65];
    bool use_static_ip;
    char static_ip[16];
    char netmask[16];
    char gateway[16];
    char dns[16];
} wifi_stored_cfg_t;

typedef struct {
    char ap_ssid[33];        // "" = default "AirPrint-Setup-XXXX"
    char ap_password[65];    // "" = open AP, else 8..63 chars WPA2-PSK
    char admin_password[65]; // "" = no password required
} wifi_sec_cfg_t;

// Initializes NVS/netif/WiFi. Uses NVS config if present, else .env defaults.
// Starts SoftAP ("AirPrint-Setup-XXXX") automatically when SSID is empty or
// if STA fails to connect within the fallback window.
esp_err_t wifi_start(const char *default_hostname);
bool wifi_connected(void);
bool wifi_ap_active(void);
void wifi_get_ap_ssid(char *buf, size_t n);
void wifi_get_ap_ip(char *buf, size_t n);
void wifi_get_sta_ssid(char *buf, size_t n);
// The station's IPv4 address as text; false (and "") while not connected.
bool wifi_get_ip(char *buf, size_t n);
// Signal of the access point in dBm, 0 when unknown.
int wifi_rssi(void);

const char *wifi_effective_hostname(void);
const char *wifi_effective_printer_name(void);

bool wifi_parse_ipv4(const char *s, uint32_t *out);
bool wifi_is_valid_hostname(const char *s);
void wifi_sanitize_hostname(const char *src, char *dst, size_t dst_n);
void wifi_get_static_cfg(bool *use_static, char *ip, size_t ip_n, char *mask, size_t mask_n, char *gw, size_t gw_n,
                         char *dns, size_t dns_n);
bool wifi_sta_has_password(void);
void wifi_preserve_sta_password(const char *ssid, char *pass_out, size_t pass_n);

bool wifi_load_config(wifi_stored_cfg_t *out);
esp_err_t wifi_save_config(const wifi_stored_cfg_t *cfg);
esp_err_t wifi_clear_config(void);
bool wifi_load_sec_config(wifi_sec_cfg_t *out);
esp_err_t wifi_save_sec_config(const wifi_sec_cfg_t *cfg);
bool wifi_admin_protected(void);
bool wifi_check_admin(const char *candidate);
bool wifi_ap_has_password(void);
void wifi_get_custom_ap_ssid(char *buf, size_t n);
esp_err_t wifi_start_portal(void);
int wifi_scan_json(char *buf, size_t n);
