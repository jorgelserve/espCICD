#ifndef SGO_UPDATER_H
#define SGO_UPDATER_H

#include <Arduino.h>

class SGO_Updater {
public:
    // Start OTA update. size=0 means unknown size.
    static bool beginUpdate(uint32_t size);

    // Write a chunk of firmware data. Returns bytes written.
    static size_t writeChunk(const uint8_t* data, size_t len);

    // Finalize the update and set boot partition. Returns true on success.
    static bool endUpdate();

    // Abort an in-progress update.
    static void abortUpdate();

    // Check if current firmware is pending verification (first boot after OTA).
    static bool isPendingVerification();

    // Confirm current firmware as valid. Cancels rollback.
    static bool confirmFirmware();

    // Mark firmware invalid and reboot to previous version.
    // This function does not return on success.
    static bool rollbackAndReboot();

    // Check if device just rolled back from a failed OTA update.
    // Returns true if the non-running OTA partition is in INVALID state.
    static bool didRollback();

    // Get human-readable error from last Update operation.
    static const char* getUpdateError();
};

#endif // SGO_UPDATER_H
