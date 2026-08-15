#include "SafeGithubOTA.h"
#include "SGO_GitHubClient.h"
#include "SGO_Provisioning.h"
#include "SGO_Updater.h"
#include "SGO_Semver.h"
#include <WiFi.h>
#include <time.h>

SafeGithubOTA::SafeGithubOTA()
    : _lastError(SGO_Error::OK)
    , _autoCheckIntervalMs(0)
    , _lastCheckMs(0)
    , _connectTimeoutMs(10000)
    , _downloadTimeoutMs(120000)
    , _rolledBack(false)
    , _patRequired(true)
    , _updateAvailable(false)
    , _assetSize(0)
{
    _version[0] = '\0';
    _lastErrorMsg[0] = '\0';
    _remoteVersion[0] = '\0';
    _assetApiUrl[0] = '\0';
}

// ============================================================
// Configuration
// ============================================================

void SafeGithubOTA::setVersion(const char* version) {
    strncpy(_version, version, sizeof(_version) - 1);
    _version[sizeof(_version) - 1] = '\0';
}

void SafeGithubOTA::onUpdateAvailable(SGO_UpdateAvailableCallback cb) {
    _updateAvailableCb = cb;
}

void SafeGithubOTA::onProgress(SGO_ProgressCallback cb) {
    _progressCb = cb;
}

void SafeGithubOTA::onValidation(SGO_ValidationCallback cb) {
    _validationCb = cb;
}

void SafeGithubOTA::onLog(SGO_LogCallback cb) {
    _logCb = cb;
}

void SafeGithubOTA::setAutoCheckInterval(uint32_t seconds) {
    if (seconds > 0 && seconds < 60) seconds = 60;
    _autoCheckIntervalMs = seconds * 1000UL;
}

void SafeGithubOTA::setConnectTimeout(uint32_t ms) {
    _connectTimeoutMs = ms;
}

void SafeGithubOTA::setDownloadTimeout(uint32_t ms) {
    _downloadTimeoutMs = ms;
}

void SafeGithubOTA::setPatRequired(bool required) {
    _patRequired = required;
}

// ============================================================
// Logging & Errors
// ============================================================

void SafeGithubOTA::_log(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (_logCb) {
        _logCb(buf);
    } else {
        Serial.print("[SafeGithubOTA] ");
        Serial.println(buf);
    }
}

SGO_Error SafeGithubOTA::_setError(SGO_Error code, const char* msg) {
    _lastError = code;
    strncpy(_lastErrorMsg, msg, sizeof(_lastErrorMsg) - 1);
    _lastErrorMsg[sizeof(_lastErrorMsg) - 1] = '\0';
    _log("ERROR: %s", msg);
    return code;
}

// ============================================================
// Lifecycle
// ============================================================

SGO_Error SafeGithubOTA::begin() {
    if (_version[0] == '\0') {
        return _setError(SGO_Error::NOT_PROVISIONED, "Version not set. Call setVersion() before begin().");
    }

    _log("SafeGithubOTA v%s starting", _version);

    // Check if we just rolled back from a failed update
    _rolledBack = SGO_Updater::didRollback();
    if (_rolledBack) {
        _log("Detected rollback from a failed OTA update");
    }

    // Handle post-OTA validation
    SGO_Error valResult = _handleValidation();
    if (valResult != SGO_Error::OK) {
        return valResult;
    }

    // Sync time via NTP (required for TLS certificate validation)
    if (WiFi.status() == WL_CONNECTED) {
        _log("Syncing time via NTP...");
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");

        // Wait up to 10 seconds for time to sync
        time_t now = 0;
        for (int i = 0; i < 20; i++) {
            time(&now);
            if (now > 1700000000) break;  // Reasonable timestamp (after 2023)
            delay(500);
        }

        if (now > 1700000000) {
            struct tm timeinfo;
            gmtime_r(&now, &timeinfo);
            _log("Time synced: %04d-%02d-%02d %02d:%02d:%02d UTC",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        } else {
            _log("WARNING: NTP time sync failed. TLS connections may fail.");
        }
    }

    // Initialize auto-check timer
    _lastCheckMs = millis();

    // Check provisioning status
    if (!SGO_Provisioning::hasCredentials(_patRequired)) {
        _log("No OTA credentials configured");
        return _setError(SGO_Error::NOT_PROVISIONED, "No OTA credentials. Call startProvisioningPortal().");
    }

    _log("Initialized successfully. Firmware version: %s", _version);
    _lastError = SGO_Error::OK;
    _lastErrorMsg[0] = '\0';
    return SGO_Error::OK;
}

void SafeGithubOTA::loop() {
    if (_autoCheckIntervalMs == 0) return;

    uint32_t now = millis();
    if ((now - _lastCheckMs) >= _autoCheckIntervalMs) {
        _lastCheckMs = now;

        if (WiFi.status() != WL_CONNECTED) {
            _log("Auto-check skipped: WiFi not connected");
            return;
        }

        _log("Auto-check: looking for updates...");
        SGO_Error err = checkAndUpdate();
        if (err == SGO_Error::ALREADY_CURRENT) {
            _log("Auto-check: firmware is current (%s)", _version);
        } else if (err != SGO_Error::OK) {
            _log("Auto-check: %s", _lastErrorMsg);
        }
        // If OK, device is rebooting
    }
}

// ============================================================
// Provisioning Portal
// ============================================================

bool SafeGithubOTA::startProvisioningPortal(
    const char* apSSID,
    const char* apPassword,
    uint32_t timeoutSeconds
) {
    _log("Starting provisioning portal (SSID: %s)", apSSID);
    return SGO_Provisioning::launchPortal(apSSID, apPassword, timeoutSeconds, _patRequired);
}

bool SafeGithubOTA::isProvisioned() const {
    return SGO_Provisioning::hasCredentials(_patRequired);
}

void SafeGithubOTA::clearCredentials() {
    SGO_Provisioning::clearCredentials();
    _log("Credentials cleared");
}

// ============================================================
// OTA Operations
// ============================================================

SGO_Error SafeGithubOTA::checkForUpdate(SGO_UpdateInfo* info) {
    _updateAvailable = false;

    if (_version[0] == '\0') {
        return _setError(SGO_Error::NOT_PROVISIONED, "Version not set");
    }

    if (WiFi.status() != WL_CONNECTED) {
        return _setError(SGO_Error::WIFI_NOT_CONNECTED, "WiFi not connected");
    }

    // Load credentials
    SGO_Credentials creds;
    if (!SGO_Provisioning::loadCredentials(creds, _patRequired)) {
        return _setError(SGO_Error::NOT_PROVISIONED, "No OTA credentials configured");
    }

    // Fetch latest release
    SGO_GitHubClient github;
    github.setConnectTimeout(_connectTimeoutMs);
    github.setDownloadTimeout(_downloadTimeoutMs);

    SGO_ReleaseInfo release;
    if (!github.fetchLatestRelease(creds.owner, creds.repo, creds.pat,
                                    creds.binFilename, release)) {
        return _setError(SGO_Error::CONNECT_FAILED, github.getLastError());
    }

    // Compare versions
    int cmp = SGO_Semver::compare(_version, release.tagName);
    if (cmp >= 0) {
        _log("Firmware is current: local=%s, remote=%s", _version, release.tagName);
        _lastError = SGO_Error::ALREADY_CURRENT;
        strncpy(_lastErrorMsg, "Firmware is already current", sizeof(_lastErrorMsg));
        return SGO_Error::ALREADY_CURRENT;
    }

    // Update available
    _log("Update available: %s -> %s (%u bytes)", _version, release.tagName, release.assetSize);

    _updateAvailable = true;
    strncpy(_remoteVersion, release.tagName, sizeof(_remoteVersion) - 1);
    strncpy(_assetApiUrl, release.assetApiUrl, sizeof(_assetApiUrl) - 1);
    _assetSize = release.assetSize;

    if (info != nullptr) {
        info->currentVersion = _version;
        info->remoteVersion = _remoteVersion;
        info->firmwareSize = _assetSize;
    }

    return SGO_Error::OK;
}

SGO_Error SafeGithubOTA::applyUpdate() {
    if (!_updateAvailable || _assetApiUrl[0] == '\0') {
        return _setError(SGO_Error::ALREADY_CURRENT, "No update available. Call checkForUpdate() first.");
    }

    // Load credentials for PAT
    SGO_Credentials creds;
    if (!SGO_Provisioning::loadCredentials(creds, _patRequired)) {
        return _setError(SGO_Error::NOT_PROVISIONED, "No OTA credentials");
    }

    // Begin OTA flash
    if (!SGO_Updater::beginUpdate(_assetSize)) {
        return _setError(SGO_Error::UPDATE_BEGIN_FAILED, SGO_Updater::getUpdateError());
    }

    _log("Downloading and flashing firmware...");

    // Download and write
    SGO_GitHubClient github;
    github.setConnectTimeout(_connectTimeoutMs);
    github.setDownloadTimeout(_downloadTimeoutMs);

    bool downloadOk = github.downloadAsset(
        _assetApiUrl,
        creds.pat,
        _assetSize,
        // Chunk writer lambda
        [](const uint8_t* data, size_t len) -> size_t {
            return SGO_Updater::writeChunk(data, len);
        },
        // Progress callback passthrough
        _progressCb
    );

    if (!downloadOk) {
        SGO_Updater::abortUpdate();
        return _setError(SGO_Error::DOWNLOAD_FAILED, github.getLastError());
    }

    // Finalize the update
    if (!SGO_Updater::endUpdate()) {
        return _setError(SGO_Error::UPDATE_END_FAILED, SGO_Updater::getUpdateError());
    }

    _log("Update successful! Rebooting into %s...", _remoteVersion);
    delay(500);
    ESP.restart();

    // Should not reach here
    return SGO_Error::OK;
}

SGO_Error SafeGithubOTA::checkAndUpdate() {
    SGO_UpdateInfo info;
    SGO_Error checkResult = checkForUpdate(&info);

    if (checkResult != SGO_Error::OK) {
        return checkResult;
    }

    // Ask user callback if we should proceed
    if (_updateAvailableCb) {
        if (!_updateAvailableCb(info)) {
            _log("Update declined by callback");
            _updateAvailable = false;
            _lastError = SGO_Error::ALREADY_CURRENT;
            strncpy(_lastErrorMsg, "Update declined by callback", sizeof(_lastErrorMsg));
            return SGO_Error::ALREADY_CURRENT;
        }
    }

    return applyUpdate();
}

// ============================================================
// Rollback
// ============================================================

SGO_Error SafeGithubOTA::_handleValidation() {
    if (!SGO_Updater::isPendingVerification()) {
        return SGO_Error::OK;
    }

    _log("Post-OTA boot detected: firmware pending verification");

    if (_validationCb) {
        _log("Running validation callback...");
        bool valid = _validationCb();
        if (valid) {
            _log("Validation passed, confirming firmware");
            if (SGO_Updater::confirmFirmware()) {
                return SGO_Error::OK;
            } else {
                return _setError(SGO_Error::ROLLBACK_FAILED, "Failed to confirm firmware");
            }
        } else {
            _log("Validation FAILED, rolling back to previous firmware!");
            SGO_Updater::rollbackAndReboot();
            // Does not return on success
            return _setError(SGO_Error::VALIDATION_FAILED, "Validation failed, rollback initiated");
        }
    } else {
        _log("No validation callback set, auto-confirming firmware");
        if (SGO_Updater::confirmFirmware()) {
            return SGO_Error::OK;
        } else {
            return _setError(SGO_Error::ROLLBACK_FAILED, "Failed to auto-confirm firmware");
        }
    }
}

SGO_Error SafeGithubOTA::confirmFirmware() {
    if (SGO_Updater::confirmFirmware()) {
        _log("Firmware manually confirmed as valid");
        return SGO_Error::OK;
    }
    return _setError(SGO_Error::ROLLBACK_FAILED, "Failed to confirm firmware");
}

SGO_Error SafeGithubOTA::rollback() {
    _log("Manual rollback requested");
    if (SGO_Updater::rollbackAndReboot()) {
        return SGO_Error::OK; // Won't reach here
    }
    return _setError(SGO_Error::ROLLBACK_FAILED, "Rollback failed - no previous firmware available");
}

bool SafeGithubOTA::isPendingVerification() const {
    return SGO_Updater::isPendingVerification();
}

bool SafeGithubOTA::wasRolledBack() const {
    return _rolledBack;
}

// ============================================================
// Getters
// ============================================================

const char* SafeGithubOTA::getVersion() const {
    return _version;
}

const char* SafeGithubOTA::getLastError() const {
    return _lastErrorMsg;
}

SGO_Error SafeGithubOTA::getLastErrorCode() const {
    return _lastError;
}

bool SafeGithubOTA::getPatRequired() const {
    return _patRequired;
}
