#ifndef SGO_SEMVER_H
#define SGO_SEMVER_H

#include <Arduino.h>

struct SGO_Version {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
    bool valid;
};

class SGO_Semver {
public:
    // Parse version string like "1.2.3" or "v1.2.3" into components.
    // Strips leading 'v' or 'V' if present.
    // Returns {0,0,0,false} if parsing fails.
    static SGO_Version parse(const char* versionStr);

    // Compare two versions.
    // Returns: -1 if a < b, 0 if a == b, 1 if a > b
    static int compare(const SGO_Version& a, const SGO_Version& b);

    // Convenience: compare two version strings.
    // Returns: -1 if a < b, 0 if a == b, 1 if a > b
    // Returns 0 if either fails to parse.
    static int compare(const char* versionA, const char* versionB);
};

#endif // SGO_SEMVER_H
