#ifndef SGO_PROVISIONING_H
#define SGO_PROVISIONING_H

#include <Arduino.h>

struct SGO_Credentials {
    char owner[64];
    char repo[64];
    char pat[128];
    char binFilename[64];
    bool valid;
};

class SGO_Provisioning {
public:
    // Load credentials from NVS. Returns true if all required fields
    // are present. When patRequired is false, an empty PAT is allowed.
    static bool loadCredentials(SGO_Credentials& creds, bool patRequired = true);

    // Save credentials to NVS. Returns true on success.
    static bool saveCredentials(const SGO_Credentials& creds);

    // Clear all credentials from NVS.
    static void clearCredentials();

    // Check if credentials exist in NVS. When patRequired is false,
    // a stored PAT is not needed (public repos).
    static bool hasCredentials(bool patRequired = true);

    // Launch blocking captive portal AP with config form.
    // Returns true if credentials were saved.
    static bool launchPortal(
        const char* apSSID,
        const char* apPassword,
        uint32_t timeoutSeconds,
        bool patRequired = true
    );

private:
    static const char* NVS_NAMESPACE;
    static const char* KEY_OWNER;
    static const char* KEY_REPO;
    static const char* KEY_PAT;
    static const char* KEY_BIN;
};

#endif // SGO_PROVISIONING_H
