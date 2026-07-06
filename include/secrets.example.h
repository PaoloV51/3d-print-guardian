#pragma once

// Copy this file to include/secrets.h and fill the local values.
// include/secrets.h is ignored by Git on purpose.

#define GUARDIAN_WIFI_SSID ""
#define GUARDIAN_WIFI_PASSWORD ""

#define GUARDIAN_AP_SSID "ESP32-IMPRESORAS"
#define GUARDIAN_AP_PASSWORD "change-me-long-password"
#define GUARDIAN_MDNS_HOSTNAME "impresoras3d"

#define GUARDIAN_DEFAULT_PRINTER_IP_1 "192.168.0.101"
#define GUARDIAN_DEFAULT_PRINTER_IP_2 "192.168.0.102"

#define GUARDIAN_OTA_METADATA_URL "https://example.com/ota/guardian/manifest.json"
#define GUARDIAN_OTA_AUTH_TOKEN ""
#define GUARDIAN_OTA_CA_CERT_PEM ""
#define GUARDIAN_OTA_ALLOW_INSECURE_HTTPS false

#define GUARDIAN_WEB_AUTH_USERNAME "admin"
#define GUARDIAN_WEB_AUTH_PASSWORD "change-me-panel-password"
#define GUARDIAN_WEB_AUTH_PROTECT_STATUS true
#define GUARDIAN_WEB_AUTH_PROTECT_CONFIG_GET true

// Used by ArduinoOTA runtime. Keep it separate from Web Auth when possible.
// PlatformIO OTA upload must send the same password with espota --auth.
#define GUARDIAN_ARDUINO_OTA_PASSWORD "change-me-ota-password"
