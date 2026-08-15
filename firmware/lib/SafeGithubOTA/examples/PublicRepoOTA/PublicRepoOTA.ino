/*
 * SafeGithubOTA - Public Repository Example
 *
 * The simplest possible setup: OTA updates from a PUBLIC GitHub repo
 * with NO Personal Access Token.
 *
 * Demonstrates:
 * - setPatRequired(false) for public repos
 * - A provisioning portal that does NOT ask for a PAT
 * - Manual OTA update check on boot
 * - Automatic firmware rollback protection (auto-confirmed)
 *
 * Usage:
 * 1. Flash this sketch to your ESP32
 * 2. On first boot, connect to the "SafeGithubOTA-Setup" WiFi network
 * 3. Enter only your repo owner, repo name, and .bin filename
 *    (the form will NOT ask for a token)
 * 4. The device will restart and connect to your WiFi network
 * 5. The device checks for updates on each boot
 *
 * GitHub Release Setup:
 * - The repository must be PUBLIC
 * - Create a Release with a semver tag (e.g., "v1.0.1" or "1.0.1")
 * - Upload your compiled .bin file as a release asset
 * - The filename must match what you entered during provisioning
 *
 * Note: Unauthenticated GitHub API requests are rate-limited to
 * 60 per hour per public IP. This is plenty for boot-time or
 * periodic (e.g. daily) update checks.
 */

#include <WiFi.h>
#include <SafeGithubOTA.h>

// Increase loop task stack for TLS operations (default 8KB is not enough)
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ---- Configuration ----
// Update this version string each time you release new firmware.
// It must be valid semver (MAJOR.MINOR.PATCH).
const char* FW_VERSION = "1.0.0";

// WiFi credentials (replace with your own)
const char* WIFI_SSID = "YourWiFiSSID";
const char* WIFI_PASS = "YourWiFiPassword";

SafeGithubOTA ota;

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println();
    Serial.println("=== SafeGithubOTA Public Repo Example ===");
    Serial.printf("Firmware version: %s\n", FW_VERSION);

    // Step 1: Set the current firmware version
    ota.setVersion(FW_VERSION);

    // Step 2: This repo is PUBLIC, so no Personal Access Token is needed.
    // Call setPatRequired(false) BEFORE isProvisioned(),
    // startProvisioningPortal(), or begin(). The provisioning portal
    // will omit the PAT field and no Authorization header is sent to
    // the GitHub API.
    ota.setPatRequired(false);

    // Step 3: Check if OTA credentials are provisioned.
    // If not, launch the captive portal for the developer to enter
    // the public repo details. This only happens once (or after
    // clearCredentials).
    if (!ota.isProvisioned()) {
        Serial.println("OTA not provisioned. Starting setup portal...");
        Serial.println("Connect to WiFi: SafeGithubOTA-Setup");
        Serial.println("Then open http://192.168.4.1 in your browser");

        // This blocks until the developer submits the form
        ota.startProvisioningPortal("SafeGithubOTA-Setup");
    }

    // Step 4: Connect to WiFi
    Serial.printf("Connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());

    // Step 5: Initialize SafeGithubOTA
    // This syncs time via NTP (required for TLS) and handles post-OTA
    // validation. Since no validation callback is set, new firmware is
    // auto-confirmed.
    SGO_Error err = ota.begin();

    // Check if we just rolled back from a failed OTA update.
    // This checks the OTA partition state and persists until the next OTA.
    if (ota.wasRolledBack()) {
        Serial.println("WARNING: Previous OTA update was rolled back!");
    }

    if (err == SGO_Error::OK) {
        // Step 6: Check for updates
        Serial.println("Checking for firmware updates...");
        err = ota.checkAndUpdate();

        if (err == SGO_Error::ALREADY_CURRENT) {
            Serial.println("Firmware is up to date.");
        } else if (err != SGO_Error::OK) {
            Serial.printf("Update check failed: %s\n", ota.getLastError());
        }
        // If err == OK, the device is rebooting into new firmware
    } else if (err == SGO_Error::NOT_PROVISIONED) {
        Serial.println("OTA credentials missing - skipping update check");
    } else {
        Serial.printf("OTA init failed: %s\n", ota.getLastError());
    }

    Serial.println("Setup complete. Running main application...");
}

void loop() {
    // Your application code goes here
    delay(1000);
}
