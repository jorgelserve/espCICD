#include "SGO_Semver.h"
#include <stdlib.h>

SGO_Version SGO_Semver::parse(const char* versionStr) {
    SGO_Version ver = {0, 0, 0, false};

    if (versionStr == nullptr || versionStr[0] == '\0') {
        return ver;
    }

    const char* ptr = versionStr;

    // Skip leading 'v' or 'V'
    if (*ptr == 'v' || *ptr == 'V') {
        ptr++;
    }

    // Parse major
    if (!isdigit((unsigned char)*ptr)) {
        return ver;
    }
    char* endPtr;
    long major = strtol(ptr, &endPtr, 10);
    if (endPtr == ptr || major < 0 || major > 65535) {
        return ver;
    }
    ver.major = (uint16_t)major;

    // Expect '.'
    if (*endPtr != '.') {
        return ver;
    }
    ptr = endPtr + 1;

    // Parse minor
    if (!isdigit((unsigned char)*ptr)) {
        return ver;
    }
    long minor = strtol(ptr, &endPtr, 10);
    if (endPtr == ptr || minor < 0 || minor > 65535) {
        return ver;
    }
    ver.minor = (uint16_t)minor;

    // Expect '.'
    if (*endPtr != '.') {
        return ver;
    }
    ptr = endPtr + 1;

    // Parse patch
    if (!isdigit((unsigned char)*ptr)) {
        return ver;
    }
    long patch = strtol(ptr, &endPtr, 10);
    if (endPtr == ptr || patch < 0 || patch > 65535) {
        return ver;
    }
    ver.patch = (uint16_t)patch;

    // Valid if we parsed all three components
    ver.valid = true;
    return ver;
}

int SGO_Semver::compare(const SGO_Version& a, const SGO_Version& b) {
    if (a.major != b.major) return a.major > b.major ? 1 : -1;
    if (a.minor != b.minor) return a.minor > b.minor ? 1 : -1;
    if (a.patch != b.patch) return a.patch > b.patch ? 1 : -1;
    return 0;
}

int SGO_Semver::compare(const char* versionA, const char* versionB) {
    SGO_Version a = parse(versionA);
    SGO_Version b = parse(versionB);

    if (!a.valid || !b.valid) {
        return 0; // Treat invalid versions as equal (no update)
    }

    return compare(a, b);
}
