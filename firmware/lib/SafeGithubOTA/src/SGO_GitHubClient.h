#ifndef SGO_GITHUB_CLIENT_H
#define SGO_GITHUB_CLIENT_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <functional>

struct SGO_ReleaseInfo {
    char tagName[24];
    char assetApiUrl[512];
    uint32_t assetSize;
    bool found;
};

typedef std::function<void(uint32_t bytesWritten, uint32_t totalBytes)> SGO_DownloadProgressCb;
typedef std::function<size_t(const uint8_t* data, size_t len)> SGO_ChunkWriterCb;

class SGO_GitHubClient {
public:
    SGO_GitHubClient();

    void setConnectTimeout(uint32_t ms);
    void setDownloadTimeout(uint32_t ms);

    // Fetch latest release info from GitHub API.
    // Parses JSON for tag_name and the asset matching binFilename.
    // Returns true on success.
    bool fetchLatestRelease(
        const char* owner,
        const char* repo,
        const char* pat,
        const char* binFilename,
        SGO_ReleaseInfo& releaseInfo
    );

    // Download asset binary via GitHub API -> S3 redirect.
    // Calls chunkWriter for each data chunk, progressCb for progress.
    // Returns true on success.
    bool downloadAsset(
        const char* assetApiUrl,
        const char* pat,
        uint32_t expectedSize,
        SGO_ChunkWriterCb chunkWriter,
        SGO_DownloadProgressCb progressCb
    );

    const char* getLastError() const;

private:
    uint32_t _connectTimeoutMs;
    uint32_t _downloadTimeoutMs;
    char _lastError[128];

    void _setError(const char* fmt, ...);

    // JSON parsing helpers (no ArduinoJson)
    bool _extractJsonString(const String& json, const char* key, char* out, size_t outLen);
    bool _extractJsonNumber(const String& json, const char* key, uint32_t* out);
    bool _findMatchingAsset(const String& json, const char* filename,
                            char* urlOut, size_t urlLen, uint32_t* sizeOut);

    // URL parsing
    bool _parseUrl(const char* url, String& host, uint16_t& port, String& path);

    // Read HTTP response status line and headers. Returns status code, or -1 on error.
    int _readHttpResponse(WiFiClientSecure& client, String& headers, uint32_t timeoutMs);

    // Extract header value (case-insensitive key match)
    String _getHeaderValue(const String& headers, const char* headerName);
};

#endif // SGO_GITHUB_CLIENT_H
