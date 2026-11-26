// Captive portal DNS responder + HTTP API for runtime WiFi provisioning.
#pragma once

#include <stdbool.h>

#include "airprint.h"

void wifi_portal_dns_start(void);
void wifi_portal_dns_stop(void);
bool wifi_portal_http(airprint_http_t *h, const char *method, const char *path);
