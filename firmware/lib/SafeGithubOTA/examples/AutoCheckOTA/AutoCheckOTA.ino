/*
 * SafeGithubOTA - Auto-Check with WiFiManager Example
 *
 * Demonstrates:
 * - WiFiManager for easy WiFi configuration (no hardcoded credentials)
 * - Automatic periodic update checks (configurable interval)
 * - Custom validation callback for rollback protection
 * - Progress reporting during download
 * - Rollback detection via wasRolledBack()
 *
 * Required Libraries (install via Arduino Library Manager):
 * - SafeGithubOTA
 * - WiFiManager by tzapu
 *
 * The device checks for updates at a regular interval while running.
 * After an OTA update, the validation callback verifies the new firmware
 * is working correctly before confirming it. If validation fails, the
 * ESP32 bootloader automatically reverts to the previous firmware.
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <SafeGithubOTA.h>

// Increase loop task stack for TLS operations (default 8KB is not enough)
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ---- Configuration ----
const char* FW_VERSION = "1.0.0";

SafeGithubOTA ota;
WiFiManager wifiManager;

// Validation callback - runs on first boot after OTA update.
// Return true if the new firmware is working correctly.
// Return false to automatically rollback to the previous firmware.
bool validateFirmware() {
    Serial.println("Running post-OTA validation...");

    // Example checks you might perform:

    // 1. Verify WiFi is still connected
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("FAIL: WiFi not connected");
        return false;
    }

    // 2. Verify a sensor is responding
    // if (!mySensor.begin()) {
    //     Serial.println("FAIL: Sensor not responding");
    //     return false;
    // }

    // 3. Verify critical data in NVS is intact
    // Preferences prefs;
    // prefs.begin("myapp", true);
    // String val = prefs.getString("key", "");
    // prefs.end();
    // if (val.length() == 0) {
    //     Serial.println("FAIL: NVS data missing");
    //     return false;
    // }

    Serial.println("Validation passed!");
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println();
    Serial.printf("=== SafeGithubOTA Auto-Check Example (v%s) ===\n", FW_VERSION);

    // Configure OTA before begin()
    ota.setVersion(FW_VERSION);

    // Set the validation callback for rollback protection.
    // After an OTA update, this runs on first boot. If it returns false,
    // the ESP32 bootloader automatically reverts to the previous firmware.
    ota.onValidation(validateFirmware);

    // Set progress callback for download status
    ota.onProgress([](uint32_t written, uint32_t total) {
        if (total > 0) {
            uint32_t pct = (written * 100) / total;
            Serial.printf("Download: %u%% (%u / %u bytes)\n", pct, written, total);
        } else {
            Serial.printf("Download: %u bytes\n", written);
        }
    });

    // Auto-check for updates every 6 hours
    ota.setAutoCheckInterval(6 * 60 * 60);

    // Provisioning (developer-only, before shipping the device)
    if (!ota.isProvisioned()) {
        Serial.println("OTA not provisioned. Starting setup portal...");
        Serial.println("Connect to WiFi: MyDevice-Setup");
        ota.startProvisioningPortal("MyDevice-Setup");
    }

    // Connect to WiFi using WiFiManager.
    // On first boot, creates a "MyDevice-WiFi" AP for WiFi configuration.
    // On subsequent boots, auto-connects to the saved network.
    wifiManager.setConfigPortalTimeout(300);
    Serial.println("Connecting to WiFi...");
    if (!wifiManager.autoConnect("MyDevice-WiFi")) {
        Serial.println("WiFi connection failed! Restarting...");
        delay(3000);
        ESP.restart();
    }
    Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());

    // Initialize OTA (must be called after WiFi is connected).
    // This syncs time via NTP, handles post-OTA validation, and loads
    // credentials from NVS.
    SGO_Error err = ota.begin();

    // Check if we rolled back from a failed OTA update.
    // This checks OTA partition state and persists until the next OTA.
    if (ota.wasRolledBack()) {
        Serial.println("WARNING: Previous OTA update failed validation and was rolled back!");
    }

    Serial.println("Setup complete. Auto-check running in the background.");
}

void loop() {
    // IMPORTANT: Call ota.loop() to process the auto-check timer.
    // When a new version is found, it downloads, flashes, and reboots
    // automatically.
    ota.loop();

    // Your application code goes here
    delay(10);
}
