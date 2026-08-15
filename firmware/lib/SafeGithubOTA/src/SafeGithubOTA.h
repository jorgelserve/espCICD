#ifndef SAFE_GITHUB_OTA_H
#define SAFE_GITHUB_OTA_H

#if !defined(ARDUINO_ARCH_ESP32)
  #error "SafeGithubOTA only supports ESP32 boards."
#endif

#include <Arduino.h>
#include <functional>

// ============================================================
// Error Codes
// ============================================================

enum class SGO_Error : int8_t {
    OK                    =  0,
    NOT_PROVISIONED       = -1,
    WIFI_NOT_CONNECTED    = -2,
    CONNECT_FAILED        = -3,
    HTTP_ERROR            = -4,
    JSON_PARSE_ERROR      = -5,
    NO_MATCHING_ASSET     = -6,
    ALREADY_CURRENT       = -7,
    DOWNLOAD_FAILED       = -8,
    UPDATE_BEGIN_FAILED   = -9,
    UPDATE_WRITE_FAILED   = -10,
    UPDATE_END_FAILED     = -11,
    ROLLBACK_FAILED       = -12,
    VALIDATION_FAILED     = -13,
    REDIRECT_FAILED       = -14,
    TIMEOUT               = -15,
};

// ============================================================
// Update Info
// ============================================================

struct SGO_UpdateInfo {
    const char* currentVersion;
    const char* remoteVersion;
    uint32_t    firmwareSize;
};

// ============================================================
// Callback Types
// ============================================================

typedef std::function<void(uint32_t bytesWritten, uint32_t totalBytes)> SGO_ProgressCallback;
typedef std::function<bool()> SGO_ValidationCallback;
typedef std::function<bool(const SGO_UpdateInfo& info)> SGO_UpdateAvailableCallback;
typedef std::function<void(const char* message)> SGO_LogCallback;

// ============================================================
// SafeGithubOTA Class
// ============================================================

class SafeGithubOTA {
public:
    SafeGithubOTA();

    // ---- Configuration (call before begin()) ----

    // Set the current firmware version. REQUIRED. Must be semver: "1.0.0"
    void setVersion(const char* version);

    // Set callback invoked when an update is found. Return true to proceed,
    // false to skip. If not set, updates proceed automatically.
    void onUpdateAvailable(SGO_UpdateAvailableCallback cb);

    // Set progress callback for download reporting.
    void onProgress(SGO_ProgressCallback cb);

    // Set validation callback. Called on first boot after OTA, before
    // confirming the new firmware. Return true to confirm, false to rollback.
    // If not set, firmware is auto-confirmed.
    void onValidation(SGO_ValidationCallback cb);

    // Set log callback. If not set, logs go to Serial.
    void onLog(SGO_LogCallback cb);

    // Set auto-check interval in seconds. 0 = disabled (default).
    // Minimum: 60 seconds.
    void setAutoCheckInterval(uint32_t seconds);

    // Set connection timeout in milliseconds (default: 10000).
    void setConnectTimeout(uint32_t ms);

    // Set download timeout in milliseconds (default: 120000).
    void setDownloadTimeout(uint32_t ms);

    // Set whether a GitHub Personal Access Token is required.
    // Default: true. Call setPatRequired(false) for PUBLIC repositories:
    // the provisioning portal then omits the PAT field and no
    // Authorization header is sent to the GitHub API.
    // Must be called before begin(), isProvisioned(), or
    // startProvisioningPortal().
    void setPatRequired(bool required);

    // ---- Lifecycle ----

    // Initialize the library. Loads NVS credentials and handles post-OTA
    // validation if this is the first boot after an update.
    // Call in setup() AFTER WiFi is connected (for validation checks).
    SGO_Error begin();

    // Must be called in loop() for auto-check timer.
    void loop();

    // ---- Provisioning Portal ----

    // Launch captive portal AP for entering GitHub credentials.
    // Blocks until submitted or timeout. AP is torn down after.
    bool startProvisioningPortal(
        const char* apSSID = "SafeGithubOTA-Setup",
        const char* apPassword = nullptr,
        uint32_t timeoutSeconds = 0
    );

    // Check if device has been provisioned.
    bool isProvisioned() const;

    // Clear all stored credentials from NVS.
    void clearCredentials();

    // ---- OTA Operations ----

    // Check for update and apply if available. Returns OK if update applied
    // (device will reboot), ALREADY_CURRENT if up to date, or an error code.
    SGO_Error checkAndUpdate();

    // Check for update without applying. Populates info if provided.
    SGO_Error checkForUpdate(SGO_UpdateInfo* info = nullptr);

    // Apply a previously-found update. Call after checkForUpdate() returned OK.
    SGO_Error applyUpdate();

    // ---- Rollback ----

    // Manually confirm current firmware as valid.
    SGO_Error confirmFirmware();

    // Force rollback to previous firmware. Reboots device.
    SGO_Error rollback();

    // True if current boot is first boot after OTA (pending verification).
    bool isPendingVerification() const;

    // True if device rolled back from a failed OTA update.
    // Check after begin() returns.
    bool wasRolledBack() const;

    // ---- Getters ----

    const char* getVersion() const;
    const char* getLastError() const;
    SGO_Error   getLastErrorCode() const;
    bool        getPatRequired() const;

private:
    char _version[24];
    char _lastErrorMsg[128];
    SGO_Error _lastError;

    SGO_ProgressCallback        _progressCb;
    SGO_ValidationCallback      _validationCb;
    SGO_UpdateAvailableCallback _updateAvailableCb;
    SGO_LogCallback             _logCb;

    uint32_t _autoCheckIntervalMs;
    uint32_t _lastCheckMs;
    uint32_t _connectTimeoutMs;
    uint32_t _downloadTimeoutMs;

    bool     _rolledBack;
    bool     _patRequired;

    // Cached from last checkForUpdate()
    bool     _updateAvailable;
    char     _remoteVersion[24];
    char     _assetApiUrl[512];
    uint32_t _assetSize;

    void _log(const char* fmt, ...);
    SGO_Error _setError(SGO_Error code, const char* msg);
    SGO_Error _handleValidation();
};

#endif // SAFE_GITHUB_OTA_H
