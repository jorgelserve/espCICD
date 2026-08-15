#include "SGO_Updater.h"
#include <Update.h>
#include "esp_ota_ops.h"

// Override the Arduino core's weak verifyRollbackLater() (defined in
// esp32-hal-misc.c with C linkage) so initArduino() does NOT auto-confirm
// new OTA firmware. This defers verification to SafeGithubOTA::begin(),
// which runs the user's validation callback.
// extern "C" is required because the weak symbol has C linkage.
extern "C" bool verifyRollbackLater() {
    return true;
}

bool SGO_Updater::beginUpdate(uint32_t size) {
    if (size == 0) {
        size = UPDATE_SIZE_UNKNOWN;
    }
    if (!Update.begin(size, U_FLASH)) {
        Serial.printf("[SafeGithubOTA] Update.begin failed: %s\n", Update.errorString());
        return false;
    }
    return true;
}

size_t SGO_Updater::writeChunk(const uint8_t* data, size_t len) {
    size_t written = Update.write(const_cast<uint8_t*>(data), len);
    if (written != len) {
        Serial.printf("[SafeGithubOTA] Update.write failed: %s\n", Update.errorString());
    }
    return written;
}

bool SGO_Updater::endUpdate() {
    if (!Update.end(true)) {
        Serial.printf("[SafeGithubOTA] Update.end failed: %s\n", Update.errorString());
        return false;
    }
    return true;
}

void SGO_Updater::abortUpdate() {
    Update.abort();
}

bool SGO_Updater::isPendingVerification() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == nullptr) {
        return false;
    }

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return false;
    }

    return (state == ESP_OTA_IMG_PENDING_VERIFY);
}

bool SGO_Updater::confirmFirmware() {
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        Serial.printf("[SafeGithubOTA] Failed to confirm firmware: %s\n", esp_err_to_name(err));
        return false;
    }
    Serial.println("[SafeGithubOTA] Firmware confirmed as valid");
    return true;
}

bool SGO_Updater::rollbackAndReboot() {
    Serial.println("[SafeGithubOTA] Rolling back to previous firmware...");
    Serial.flush();
    delay(100);
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    // If we reach here, rollback failed
    Serial.printf("[SafeGithubOTA] Rollback failed: %s\n", esp_err_to_name(err));
    return false;
}

bool SGO_Updater::didRollback() {
    // Find the non-running OTA partition and check if it's INVALID,
    // which indicates we just rolled back from a failed update.
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == nullptr) return false;

    // Get the "next" OTA partition (the one we're NOT running from)
    const esp_partition_t* other = esp_ota_get_next_update_partition(running);
    if (other == nullptr) return false;

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(other, &state) != ESP_OK) return false;

    return (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED);
}

const char* SGO_Updater::getUpdateError() {
    return Update.errorString();
}
