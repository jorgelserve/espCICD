# SafeGithubOTA

Safe OTA firmware updates from GitHub releases for ESP32 — public or private repositories.

SafeGithubOTA lets your ESP32 devices check for new firmware releases on GitHub, download and flash them over-the-air, and automatically roll back to the previous version if the new firmware fails a validation check. It works with both public and private repositories.

## Features

- **Public & Private Repo Support** — Downloads firmware binaries from GitHub releases. Private repos use a Personal Access Token (PAT); public repos need no token at all (`setPatRequired(false)`)
- **Automatic Rollback Protection** — If new firmware fails your validation callback, the ESP32 bootloader automatically reverts to the previous working version
- **Captive Portal Provisioning** — Built-in WiFi AP with a web form for entering GitHub repo details (and a PAT for private repos; the PAT field is omitted for public repos). Credentials are stored in NVS (non-volatile storage)
- **Semantic Version Comparison** — Compares local and remote versions using semver (MAJOR.MINOR.PATCH) to determine if an update is available
- **Auto-Check Timer** — Configurable periodic update checks (e.g., every 6 hours) that run in the background
- **Callbacks** — Optional callbacks for validation, progress reporting, update gating, and custom logging
- **Zero External Dependencies** — Only uses libraries built into the ESP32 Arduino core (WiFi, WiFiClientSecure, WebServer, Update, Preferences)

## How It Works

```
1. Device boots → begin() syncs time via NTP and handles post-OTA validation
2. checkForUpdate() → queries GitHub API for latest release → compares semver tags
3. applyUpdate() → downloads .bin via GitHub API → follows S3 redirect → streams to flash
4. Device reboots → begin() detects pending verification → runs validation callback
5. Callback returns true → firmware confirmed | returns false → automatic rollback
```

## Quick Start

```cpp
#include <WiFi.h>
#include <SafeGithubOTA.h>

// Required: increase stack size for TLS operations
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

const char* FW_VERSION = "1.0.0";
SafeGithubOTA ota;

void setup() {
    Serial.begin(115200);

    ota.setVersion(FW_VERSION);

    // First-time provisioning via captive portal
    if (!ota.isProvisioned()) {
        ota.startProvisioningPortal("MyDevice-Setup");
    }

    // Connect to WiFi (use WiFiManager, hardcoded creds, etc.)
    WiFi.begin("YourSSID", "YourPassword");
    while (WiFi.status() != WL_CONNECTED) delay(500);

    // Initialize — handles NTP sync and post-OTA validation
    ota.begin();

    // Check if we rolled back from a failed update
    if (ota.wasRolledBack()) {
        Serial.println("Previous update was rolled back!");
    }

    // Check for updates
    ota.checkAndUpdate(); // Downloads, flashes, and reboots if update found
}

void loop() {
    delay(1000);
}
```

## Installation

### Arduino Library Manager (Recommended)

1. Open the Arduino IDE
2. Go to **Sketch > Include Library > Manage Libraries...**
3. Search for **SafeGithubOTA**
4. Click **Install**

### Manual Installation

Clone into your Arduino libraries folder:

```
cd ~/Arduino/libraries
git clone https://github.com/gibz104/SafeGithubOTA.git
```

Or download the ZIP from the [Releases](https://github.com/gibz104/SafeGithubOTA/releases) page and install via **Sketch > Include Library > Add .ZIP Library**.

### Board Support

Requires the **ESP32 Arduino Core** (v2.x or v3.x). Tested on:
- ESP32
- ESP32-S3
- ESP32-C3

## GitHub Setup

1. **Authentication**
   - **Public repos:** no token needed. Call `ota.setPatRequired(false)` before `isProvisioned()`/`startProvisioningPortal()`/`begin()` — the portal skips the PAT field and no `Authorization` header is sent. (Unauthenticated GitHub API requests are rate-limited to 60/hour per IP — fine for boot-time or daily checks.)
   - **Private repos:** create a GitHub Personal Access Token (PAT)
     - Go to GitHub > Settings > Developer settings > Personal access tokens
     - Create a fine-grained token with `Contents: Read` permission for your repo
     - Or create a classic token with `repo` scope

2. **Create a GitHub Release**
   - Tag it with a semver version (e.g., `v1.0.0` or `1.0.0`)
   - Upload your compiled `.bin` file as a release asset

3. **Provision your device**
   - On first boot, connect to the device's WiFi AP
   - Enter: repo owner, repo name, PAT, and the `.bin` filename
   - For public repos (`setPatRequired(false)`), the PAT field is omitted — just enter owner, name, and `.bin` filename
   - Credentials are saved to NVS and persist across reboots

## API Reference

### Configuration (call before `begin()`)

| Method | Description |
|---|---|
| `setVersion(const char* version)` | **Required.** Set current firmware version (semver: `"1.0.0"`) |
| `onValidation(callback)` | Set post-OTA validation callback. Return `true` to confirm, `false` to rollback. If not set, firmware is auto-confirmed |
| `onProgress(callback)` | Set download progress callback: `void(uint32_t written, uint32_t total)` |
| `onUpdateAvailable(callback)` | Set update gating callback. Return `true` to proceed, `false` to skip |
| `onLog(callback)` | Set custom log handler: `void(const char* message)`. Default: Serial |
| `setAutoCheckInterval(uint32_t seconds)` | Set periodic check interval. `0` = disabled (default). Minimum: 60s |
| `setConnectTimeout(uint32_t ms)` | Connection timeout (default: 10000ms) |
| `setDownloadTimeout(uint32_t ms)` | Download timeout (default: 120000ms) |
| `setPatRequired(bool required)` | Set whether a PAT is required. Default: `true`. Pass `false` for **public** repos — the portal omits the PAT field and no auth header is sent |

### Lifecycle

| Method | Description |
|---|---|
| `begin()` | Initialize library. Syncs NTP, loads credentials, handles post-OTA validation. Call after WiFi is connected |
| `loop()` | Call in `loop()` to process the auto-check timer |

### Provisioning

| Method | Description |
|---|---|
| `startProvisioningPortal(ssid, password, timeout)` | Launch captive portal AP with config form. Blocks until submitted or timeout |
| `isProvisioned()` | Check if credentials exist in NVS |
| `clearCredentials()` | Clear all stored credentials |

### OTA Operations

| Method | Description |
|---|---|
| `checkAndUpdate()` | Check for update and apply if available. Reboots on success |
| `checkForUpdate(SGO_UpdateInfo* info)` | Check only, without applying. Populates `info` if provided |
| `applyUpdate()` | Apply a previously found update. Call after `checkForUpdate()` returned `OK` |

### Rollback

| Method | Description |
|---|---|
| `confirmFirmware()` | Manually confirm current firmware as valid |
| `rollback()` | Force rollback to previous firmware. Reboots device |
| `isPendingVerification()` | `true` if current boot is first boot after OTA (awaiting confirmation) |
| `wasRolledBack()` | `true` if device rolled back from a failed OTA update. Persists until next OTA |

### Error Codes

| Code | Meaning |
|---|---|
| `OK` | Success |
| `NOT_PROVISIONED` | No credentials configured |
| `WIFI_NOT_CONNECTED` | WiFi not connected |
| `CONNECT_FAILED` | Could not connect to GitHub API |
| `HTTP_ERROR` | Unexpected HTTP response |
| `JSON_PARSE_ERROR` | Failed to parse GitHub API response |
| `NO_MATCHING_ASSET` | No release asset matching the configured filename |
| `ALREADY_CURRENT` | Firmware is already up to date |
| `DOWNLOAD_FAILED` | Firmware download failed |
| `UPDATE_BEGIN_FAILED` | Could not start OTA flash |
| `UPDATE_WRITE_FAILED` | Error writing firmware to flash |
| `UPDATE_END_FAILED` | Error finalizing OTA flash |
| `ROLLBACK_FAILED` | Rollback operation failed |
| `VALIDATION_FAILED` | Post-OTA validation callback returned false |
| `REDIRECT_FAILED` | Failed to follow GitHub-to-S3 redirect |
| `TIMEOUT` | Operation timed out |

## Validation Callback Example

The validation callback runs on the first boot after an OTA update. Use it to verify that your hardware is functioning correctly before confirming the new firmware:

```cpp
ota.onValidation([]() -> bool {
    // Check that sensors are responding
    if (!sht30.begin(0x44)) return false;
    if (!lightMeter.begin()) return false;

    // Check that readings are sane
    float temp = sht30.readTemperature();
    if (isnan(temp)) return false;

    // All good — confirm the new firmware
    return true;
});
```

If the callback returns `false` (or the device crashes before `begin()` runs), the ESP32 bootloader automatically reverts to the previous firmware on the next reboot.

## Important Notes

- **`SET_LOOP_TASK_STACK_SIZE(16 * 1024)`** must be included in your `.ino` file (before `setup()`). The default 8KB loop task stack is not enough for TLS operations and will cause crashes. This macro cannot be placed inside a library.

- **NTP time sync** is performed during `begin()` because TLS certificate validation requires an accurate clock. If WiFi is not connected when `begin()` is called, time sync will be skipped and TLS connections may fail.

- **Credentials are stored in NVS** (ESP32 non-volatile storage) using the Preferences library. They persist across reboots and OTA updates. For private repos the PAT is stored in plaintext on the device — treat provisioned devices accordingly. For public repos (`setPatRequired(false)`) no token is stored.

- **GitHub rate limits** apply: 5,000 requests/hour with a PAT (private repos), or 60 requests/hour per IP unauthenticated (public repos). Boot-time and daily auto-checks stay well within either limit.

## Examples

- **[BasicOTA](examples/BasicOTA/BasicOTA.ino)** — Minimal setup with manual update check on boot
- **[PublicRepoOTA](examples/PublicRepoOTA/PublicRepoOTA.ino)** — Simplest setup for a **public** repo: no PAT, `setPatRequired(false)`
- **[AutoCheckOTA](examples/AutoCheckOTA/AutoCheckOTA.ino)** — Periodic background checks with WiFiManager and validation callback
- **[AdvancedOTA](examples/AdvancedOTA/AdvancedOTA.ino)** — Full-featured with serial commands, all callbacks, button re-provisioning, and WiFiManager

## License

[MIT](LICENSE)
