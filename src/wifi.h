// WiFi station + NVS provisioning + SoftAP fallback portal.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef struct {
    char ssid[33];
    char password[65];
    char hostname[33];
    char printer_name[65];
} wifi_stored_cfg_t;

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

bool wifi_load_config(wifi_stored_cfg_t *out);
esp_err_t wifi_save_config(const wifi_stored_cfg_t *cfg);
esp_err_t wifi_clear_config(void);
esp_err_t wifi_start_portal(void);
int wifi_scan_json(char *buf, size_t n);
