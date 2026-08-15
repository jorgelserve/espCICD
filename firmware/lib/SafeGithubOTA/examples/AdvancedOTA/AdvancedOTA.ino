/*
 * SafeGithubOTA - Advanced Example
 *
 * Demonstrates:
 * - WiFiManager for easy WiFi configuration (no hardcoded credentials)
 * - All callback types (validation, progress, update available, logging)
 * - Serial command interface for manual OTA control
 * - Button-triggered re-provisioning
 * - Conditional update acceptance
 * - Custom log handler
 * - Separate check and apply workflow
 * - Rollback detection via wasRolledBack()
 *
 * Required Libraries (install via Arduino Library Manager):
 * - SafeGithubOTA
 * - WiFiManager by tzapu
 *
 * Serial Commands:
 *   check    - Check for available updates
 *   update   - Check and apply update if available
 *   version  - Print current firmware version
 *   provision - Enter provisioning mode
 *   confirm  - Manually confirm current firmware
 *   rollback - Force rollback to previous firmware
 *   clear    - Clear stored OTA credentials
 *   status   - Print OTA status info
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <SafeGithubOTA.h>

// Increase loop task stack for TLS operations (default 8KB is not enough)
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ---- Configuration ----
const char* FW_VERSION = "2.0.0";

// GPIO pin for re-provisioning button (GPIO0 = BOOT button on many boards)
const int PROVISION_BUTTON_PIN = 0;
const int PROVISION_HOLD_MS = 3000;   // Hold for 3 seconds to trigger

SafeGithubOTA ota;
WiFiManager wifiManager;

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println();
    Serial.printf("=== SafeGithubOTA Advanced Example (v%s) ===\n", FW_VERSION);

    pinMode(PROVISION_BUTTON_PIN, INPUT_PULLUP);

    // ---- Configure OTA ----

    ota.setVersion(FW_VERSION);

    // Custom log handler - prefix with uptime
    ota.onLog([](const char* msg) {
        Serial.printf("[OTA %lu] %s\n", millis() / 1000, msg);
    });

    // Update available callback - decide whether to proceed.
    // You could check battery level, time of day, user preferences, etc.
    ota.onUpdateAvailable([](const SGO_UpdateInfo& info) -> bool {
        Serial.printf("\n*** Update available: %s -> %s (%u bytes) ***\n",
            info.currentVersion, info.remoteVersion, info.firmwareSize);

        // Example: Only update if firmware is smaller than 2MB
        if (info.firmwareSize > 2 * 1024 * 1024) {
            Serial.println("Skipping: firmware too large");
            return false;
        }

        // Example: Only update if WiFi signal is strong enough for reliable download
        if (WiFi.RSSI() < -80) {
            Serial.println("Skipping: WiFi signal too weak for OTA");
            return false;
        }

        Serial.println("Proceeding with update...");
        return true;
    });

    // Progress callback - show download progress bar
    ota.onProgress([](uint32_t written, uint32_t total) {
        static uint32_t lastPct = 999;
        uint32_t pct = total > 0 ? (written * 100) / total : 0;
        if (pct != lastPct) {
            Serial.printf("\rFlashing: [");
            int bars = pct / 5;
            for (int i = 0; i < 20; i++) {
                Serial.print(i < bars ? "#" : " ");
            }
            Serial.printf("] %u%%", pct);
            if (pct == 100) Serial.println();
            lastPct = pct;
        }
    });

    // Validation callback - verify hardware after OTA update.
    // Runs on first boot after OTA. Return false to rollback.
    ota.onValidation([]() -> bool {
        Serial.println("Running hardware validation...");

        // Check WiFi connectivity
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("FAIL: WiFi disconnected");
            return false;
        }

        // Add your hardware-specific checks here:
        // - Sensor initialization
        // - Peripheral communication
        // - NVS data integrity
        // - Network reachability

        Serial.println("All checks passed.");
        return true;
    });

    // Set timeouts
    ota.setConnectTimeout(15000);
    ota.setDownloadTimeout(180000);

    // Auto-check every hour
    ota.setAutoCheckInterval(3600);

    // ---- OTA Provisioning ----

    if (!ota.isProvisioned()) {
        Serial.println("No OTA credentials. Starting provisioning portal...");
        ota.startProvisioningPortal("MyDevice-Config", "setup1234", 300);
    } else {
        // Check if BOOT button is held to re-provision OTA credentials
        Serial.println("Hold BOOT button for 3s to re-provision...");
        uint32_t holdStart = millis();
        bool buttonHeld = false;

        while ((millis() - holdStart) < PROVISION_HOLD_MS) {
            if (digitalRead(PROVISION_BUTTON_PIN) == HIGH) {
                break; // Button released
            }
            if ((millis() - holdStart) >= PROVISION_HOLD_MS) {
                buttonHeld = true;
            }
            delay(10);
        }

        if (buttonHeld) {
            Serial.println("Button held - entering provisioning mode...");
            ota.startProvisioningPortal("MyDevice-Config", "setup1234", 300);
        }
    }

    // ---- Connect WiFi ----

    // WiFiManager handles WiFi configuration via captive portal.
    // On first boot, creates a "MyDevice-WiFi" AP for configuration.
    // On subsequent boots, auto-connects to the saved network.
    wifiManager.setConfigPortalTimeout(300);
    Serial.println("Connecting to WiFi...");
    if (!wifiManager.autoConnect("MyDevice-WiFi")) {
        Serial.println("WiFi connection failed!");
        // Continue without WiFi - OTA won't work but app may still function
    } else {
        Serial.printf("Connected: %s (RSSI: %d dBm)\n",
            WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }

    // ---- Initialize OTA ----

    SGO_Error err = ota.begin();

    // Check if device just rolled back from a failed OTA update.
    // This checks OTA partition state and persists until the next OTA.
    if (ota.wasRolledBack()) {
        Serial.println("WARNING: Previous OTA update failed validation and was rolled back!");
    }

    Serial.println();
    Serial.println("Type 'help' for available commands.");
    Serial.println("Auto-check running in background.");
}

void loop() {
    // Process auto-check timer
    ota.loop();

    // Process serial commands
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        cmd.toLowerCase();

        if (cmd == "help") {
            Serial.println("Commands:");
            Serial.println("  check     - Check for updates");
            Serial.println("  update    - Check and apply update");
            Serial.println("  version   - Print firmware version");
            Serial.println("  provision - Enter provisioning mode");
            Serial.println("  confirm   - Confirm current firmware");
            Serial.println("  rollback  - Rollback to previous firmware");
            Serial.println("  clear     - Clear OTA credentials");
            Serial.println("  status    - Print OTA status");
        }
        else if (cmd == "check") {
            SGO_UpdateInfo info;
            SGO_Error err = ota.checkForUpdate(&info);
            if (err == SGO_Error::OK) {
                Serial.printf("Update available: %s -> %s (%u bytes)\n",
                    info.currentVersion, info.remoteVersion, info.firmwareSize);
                Serial.println("Type 'update' to apply.");
            } else if (err == SGO_Error::ALREADY_CURRENT) {
                Serial.println("Firmware is up to date.");
            } else {
                Serial.printf("Check failed: %s\n", ota.getLastError());
            }
        }
        else if (cmd == "update") {
            SGO_Error err = ota.checkAndUpdate();
            if (err == SGO_Error::ALREADY_CURRENT) {
                Serial.println("Already up to date.");
            } else if (err != SGO_Error::OK) {
                Serial.printf("Update failed: %s\n", ota.getLastError());
            }
            // If OK, device is rebooting into new firmware
        }
        else if (cmd == "version") {
            Serial.printf("Firmware: %s\n", ota.getVersion());
            Serial.printf("Pending verification: %s\n",
                ota.isPendingVerification() ? "yes" : "no");
        }
        else if (cmd == "provision") {
            Serial.println("Starting provisioning portal (5 min timeout)...");
            ota.startProvisioningPortal("MyDevice-Config", "setup1234", 300);
        }
        else if (cmd == "confirm") {
            SGO_Error err = ota.confirmFirmware();
            Serial.printf("Confirm: %s\n",
                err == SGO_Error::OK ? "Success" : ota.getLastError());
        }
        else if (cmd == "rollback") {
            Serial.println("Rolling back...");
            ota.rollback();
            Serial.println("Rollback failed (no previous firmware?)");
        }
        else if (cmd == "clear") {
            ota.clearCredentials();
            Serial.println("OTA credentials cleared.");
        }
        else if (cmd == "status") {
            Serial.printf("Version: %s\n", ota.getVersion());
            Serial.printf("Provisioned: %s\n", ota.isProvisioned() ? "yes" : "no");
            Serial.printf("Pending verification: %s\n",
                ota.isPendingVerification() ? "yes" : "no");
            Serial.printf("Rolled back: %s\n",
                ota.wasRolledBack() ? "yes" : "no");
            Serial.printf("WiFi: %s (RSSI: %d)\n",
                WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
                WiFi.RSSI());
            Serial.printf("Last error: %s\n", ota.getLastError());
        }
        else if (cmd.length() > 0) {
            Serial.printf("Unknown command: '%s'. Type 'help' for commands.\n", cmd.c_str());
        }
    }

    delay(10);
}
