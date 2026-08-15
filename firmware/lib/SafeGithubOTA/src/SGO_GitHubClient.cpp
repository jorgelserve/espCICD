#include "SGO_GitHubClient.h"
#include "SGO_CACert.h"
#include <WiFiClientSecure.h>

// ESP32 built-in certificate bundle (covers GitHub, Amazon S3, etc.)
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

#define SGO_CHUNK_SIZE 4096
#define SGO_GITHUB_HOST "api.github.com"
#define SGO_GITHUB_PORT 443

SGO_GitHubClient::SGO_GitHubClient()
    : _connectTimeoutMs(10000)
    , _downloadTimeoutMs(120000)
{
    _lastError[0] = '\0';
}

void SGO_GitHubClient::setConnectTimeout(uint32_t ms) {
    _connectTimeoutMs = ms;
}

void SGO_GitHubClient::setDownloadTimeout(uint32_t ms) {
    _downloadTimeoutMs = ms;
}

const char* SGO_GitHubClient::getLastError() const {
    return _lastError;
}

void SGO_GitHubClient::_setError(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(_lastError, sizeof(_lastError), fmt, args);
    va_end(args);
}

// ============================================================
// JSON Parsing Helpers (hand-rolled, no ArduinoJson)
// ============================================================

bool SGO_GitHubClient::_extractJsonString(const String& json, const char* key,
                                           char* out, size_t outLen) {
    // Search for "key" : "value"
    String searchKey = String("\"") + key + "\"";
    int keyIdx = json.indexOf(searchKey);
    if (keyIdx < 0) return false;

    // Find the colon after the key
    int colonIdx = json.indexOf(':', keyIdx + searchKey.length());
    if (colonIdx < 0) return false;

    // Find the opening quote of the value
    int quoteStart = json.indexOf('"', colonIdx + 1);
    if (quoteStart < 0) return false;

    // Find the closing quote (handle escaped quotes)
    int quoteEnd = quoteStart + 1;
    while (quoteEnd < (int)json.length()) {
        if (json.charAt(quoteEnd) == '\\') {
            quoteEnd += 2; // Skip escaped character
            continue;
        }
        if (json.charAt(quoteEnd) == '"') {
            break;
        }
        quoteEnd++;
    }

    if (quoteEnd >= (int)json.length()) return false;

    int valueLen = quoteEnd - quoteStart - 1;
    if (valueLen <= 0 || (size_t)valueLen >= outLen) return false;

    String value = json.substring(quoteStart + 1, quoteEnd);
    strncpy(out, value.c_str(), outLen - 1);
    out[outLen - 1] = '\0';
    return true;
}

bool SGO_GitHubClient::_extractJsonNumber(const String& json, const char* key,
                                            uint32_t* out) {
    String searchKey = String("\"") + key + "\"";
    int keyIdx = json.indexOf(searchKey);
    if (keyIdx < 0) return false;

    int colonIdx = json.indexOf(':', keyIdx + searchKey.length());
    if (colonIdx < 0) return false;

    // Skip whitespace after colon
    int numStart = colonIdx + 1;
    while (numStart < (int)json.length() && json.charAt(numStart) == ' ') {
        numStart++;
    }

    if (numStart >= (int)json.length()) return false;
    if (!isdigit(json.charAt(numStart))) return false;

    String numStr = "";
    int i = numStart;
    while (i < (int)json.length() && isdigit(json.charAt(i))) {
        numStr += json.charAt(i);
        i++;
    }

    *out = (uint32_t)numStr.toInt();
    return true;
}

bool SGO_GitHubClient::_findMatchingAsset(const String& json, const char* filename,
                                            char* urlOut, size_t urlLen, uint32_t* sizeOut) {
    // Find the "assets" array
    int assetsIdx = json.indexOf("\"assets\"");
    if (assetsIdx < 0) return false;

    int bracketStart = json.indexOf('[', assetsIdx);
    if (bracketStart < 0) return false;

    // Iterate through asset objects looking for matching "name"
    int pos = bracketStart + 1;
    int depth = 0;

    while (pos < (int)json.length()) {
        char c = json.charAt(pos);

        if (c == '{') {
            if (depth == 0) {
                // Start of an asset object - find its closing brace
                int objStart = pos;
                int objDepth = 1;
                int objEnd = pos + 1;

                while (objEnd < (int)json.length() && objDepth > 0) {
                    char oc = json.charAt(objEnd);
                    if (oc == '{') objDepth++;
                    else if (oc == '}') objDepth--;
                    // Skip string contents to avoid counting braces inside strings
                    else if (oc == '"') {
                        objEnd++;
                        while (objEnd < (int)json.length()) {
                            if (json.charAt(objEnd) == '\\') {
                                objEnd += 2;
                                continue;
                            }
                            if (json.charAt(objEnd) == '"') break;
                            objEnd++;
                        }
                    }
                    objEnd++;
                }

                // Extract this single asset object
                String assetObj = json.substring(objStart, objEnd);

                // Check if "name" matches our filename
                char assetName[64] = {0};
                if (_extractJsonString(assetObj, "name", assetName, sizeof(assetName))) {
                    if (strcmp(assetName, filename) == 0) {
                        // Found the matching asset
                        // Extract the "url" field (API URL, NOT browser_download_url)
                        // The "url" field in asset objects is the API endpoint:
                        // https://api.github.com/repos/OWNER/REPO/releases/assets/ID
                        if (!_extractJsonString(assetObj, "url", urlOut, urlLen)) {
                            return false;
                        }

                        // Verify we got the asset API URL (contains /releases/assets/)
                        // and not the uploader URL or browser_download_url
                        if (strstr(urlOut, "/releases/assets/") == nullptr) {
                            return false;
                        }

                        _extractJsonNumber(assetObj, "size", sizeOut);
                        return true;
                    }
                }

                pos = objEnd;
                continue;
            }
            depth++;
        } else if (c == '}') {
            depth--;
        } else if (c == ']' && depth == 0) {
            break; // End of assets array
        }
        pos++;
    }

    return false;
}

// ============================================================
// URL Parsing
// ============================================================

bool SGO_GitHubClient::_parseUrl(const char* url, String& host, uint16_t& port, String& path) {
    String urlStr(url);

    // Strip protocol
    int protoEnd;
    if (urlStr.startsWith("https://")) {
        protoEnd = 8;
        port = 443;
    } else if (urlStr.startsWith("http://")) {
        protoEnd = 7;
        port = 80;
    } else {
        return false;
    }

    // Find path start
    int pathStart = urlStr.indexOf('/', protoEnd);
    if (pathStart < 0) {
        host = urlStr.substring(protoEnd);
        path = "/";
    } else {
        host = urlStr.substring(protoEnd, pathStart);
        path = urlStr.substring(pathStart);
    }

    // Check for port in host
    int colonIdx = host.indexOf(':');
    if (colonIdx >= 0) {
        port = host.substring(colonIdx + 1).toInt();
        host = host.substring(0, colonIdx);
    }

    return host.length() > 0;
}

// ============================================================
// HTTP Response Reading
// ============================================================

int SGO_GitHubClient::_readHttpResponse(WiFiClientSecure& client, String& headers,
                                          uint32_t timeoutMs) {
    uint32_t startMs = millis();
    int statusCode = -1;
    headers = "";

    // Read status line
    String statusLine = "";
    while (client.connected() && (millis() - startMs) < timeoutMs) {
        if (client.available()) {
            char c = client.read();
            if (c == '\n') break;
            if (c != '\r') statusLine += c;
        } else {
            delay(1);
        }
    }

    // Parse status code from "HTTP/1.1 200 OK"
    int spaceIdx = statusLine.indexOf(' ');
    if (spaceIdx > 0) {
        statusCode = statusLine.substring(spaceIdx + 1).toInt();
    }

    // Read headers until blank line
    String currentLine = "";
    while (client.connected() && (millis() - startMs) < timeoutMs) {
        if (client.available()) {
            char c = client.read();
            if (c == '\n') {
                if (currentLine.length() == 0) {
                    break; // Blank line = end of headers
                }
                headers += currentLine + "\n";
                currentLine = "";
            } else if (c != '\r') {
                currentLine += c;
            }
        } else {
            delay(1);
        }
    }

    return statusCode;
}

String SGO_GitHubClient::_getHeaderValue(const String& headers, const char* headerName) {
    String search = String(headerName) + ":";
    search.toLowerCase();

    String lowerHeaders = headers;
    lowerHeaders.toLowerCase();

    int idx = lowerHeaders.indexOf(search);
    if (idx < 0) return "";

    int valueStart = idx + search.length();
    int lineEnd = headers.indexOf('\n', valueStart);
    if (lineEnd < 0) lineEnd = headers.length();

    String value = headers.substring(valueStart, lineEnd);
    value.trim();
    return value;
}

// ============================================================
// Public API: Fetch Latest Release
// ============================================================

bool SGO_GitHubClient::fetchLatestRelease(
    const char* owner,
    const char* repo,
    const char* pat,
    const char* binFilename,
    SGO_ReleaseInfo& releaseInfo
) {
    memset(&releaseInfo, 0, sizeof(releaseInfo));
    releaseInfo.found = false;

    WiFiClientSecure client;
    client.setCACertBundle(rootca_crt_bundle_start);
    client.setTimeout(_connectTimeoutMs / 1000);

    Serial.printf("[SafeGithubOTA] Connecting to %s...\n", SGO_GITHUB_HOST);

    if (!client.connect(SGO_GITHUB_HOST, SGO_GITHUB_PORT)) {
        char errBuf[128];
        int errCode = client.lastError(errBuf, sizeof(errBuf));
        Serial.printf("[SafeGithubOTA] TLS error %d: %s\n", errCode, errBuf);

        // Retry with embedded DigiCert root CA as fallback
        Serial.println("[SafeGithubOTA] Retrying with embedded CA cert...");
        client.setCACert(SGO_DIGICERT_ROOT_CA);
        if (!client.connect(SGO_GITHUB_HOST, SGO_GITHUB_PORT)) {
            errCode = client.lastError(errBuf, sizeof(errBuf));
            Serial.printf("[SafeGithubOTA] TLS error %d: %s\n", errCode, errBuf);
            _setError("Failed to connect to %s", SGO_GITHUB_HOST);
            return false;
        }
    }

    // Build request
    String path = String("/repos/") + owner + "/" + repo + "/releases/latest";

    // Only send the Authorization header when a PAT is present. Public
    // repos work without it; private repos still require it.
    String request = String("GET ") + path + " HTTP/1.1\r\n" +
                     "Host: " + SGO_GITHUB_HOST + "\r\n";
    if (pat != nullptr && pat[0] != '\0') {
        request += String("Authorization: Bearer ") + pat + "\r\n";
    }
    request += String("Accept: application/vnd.github+json\r\n") +
               "User-Agent: SafeGithubOTA/1.0\r\n" +
               "Connection: close\r\n\r\n";
    client.print(request);

    // Read response
    String headers;
    int statusCode = _readHttpResponse(client, headers, _connectTimeoutMs);

    if (statusCode != 200) {
        client.stop();
        if (statusCode == 404) {
            _setError("No releases found (404). Check repo name and PAT permissions.");
        } else if (statusCode == 401) {
            _setError("Authentication failed (401). Check your PAT.");
        } else if (statusCode == 403) {
            _setError("Access forbidden (403). PAT may lack 'repo' scope or rate limited.");
        } else {
            _setError("GitHub API returned HTTP %d", statusCode);
        }
        return false;
    }

    // Read body
    String body = "";
    uint32_t bodyStart = millis();
    while (client.connected() || client.available()) {
        if ((millis() - bodyStart) > _connectTimeoutMs) {
            _setError("Timeout reading response body");
            client.stop();
            return false;
        }
        if (client.available()) {
            char c = client.read();
            body += c;
        } else {
            delay(1);
        }
    }
    client.stop();

    if (body.length() == 0) {
        _setError("Empty response body from GitHub API");
        return false;
    }

    // Parse tag_name
    if (!_extractJsonString(body, "tag_name", releaseInfo.tagName, sizeof(releaseInfo.tagName))) {
        _setError("Failed to parse tag_name from release JSON");
        return false;
    }

    Serial.printf("[SafeGithubOTA] Latest release: %s\n", releaseInfo.tagName);

    // Find matching asset
    if (!_findMatchingAsset(body, binFilename, releaseInfo.assetApiUrl,
                            sizeof(releaseInfo.assetApiUrl), &releaseInfo.assetSize)) {
        _setError("Asset '%s' not found in release %s", binFilename, releaseInfo.tagName);
        return false;
    }

    Serial.printf("[SafeGithubOTA] Found asset: %s (%u bytes)\n", binFilename, releaseInfo.assetSize);
    releaseInfo.found = true;
    return true;
}

// ============================================================
// Public API: Download Asset (with redirect handling)
// ============================================================

bool SGO_GitHubClient::downloadAsset(
    const char* assetApiUrl,
    const char* pat,
    uint32_t expectedSize,
    SGO_ChunkWriterCb chunkWriter,
    SGO_DownloadProgressCb progressCb
) {
    // ---- Phase 1: Request asset from GitHub API (expect 302 redirect) ----

    String host, path;
    uint16_t port;
    if (!_parseUrl(assetApiUrl, host, port, path)) {
        _setError("Failed to parse asset URL");
        return false;
    }

    WiFiClientSecure client;
    client.setCACertBundle(rootca_crt_bundle_start);
    client.setTimeout(_connectTimeoutMs / 1000);

    Serial.println("[SafeGithubOTA] Requesting asset download...");

    if (!client.connect(host.c_str(), port)) {
        _setError("Failed to connect to %s for asset download", host.c_str());
        return false;
    }

    // Only send the Authorization header when a PAT is present (private
    // repos). The S3 redirect target below must never receive it.
    String request = String("GET ") + path + " HTTP/1.1\r\n" +
                     "Host: " + host + "\r\n";
    if (pat != nullptr && pat[0] != '\0') {
        request += String("Authorization: Bearer ") + pat + "\r\n";
    }
    request += String("Accept: application/octet-stream\r\n") +
               "User-Agent: SafeGithubOTA/1.0\r\n" +
               "Connection: close\r\n\r\n";
    client.print(request);

    String headers;
    int statusCode = _readHttpResponse(client, headers, _connectTimeoutMs);
    client.stop();

    String redirectUrl;

    if (statusCode == 200) {
        // GitHub served the binary directly (unusual but possible)
        // We already consumed headers, would need to read body here.
        // This case is rare for private repos; they almost always redirect.
        _setError("Unexpected direct 200 response. Expected 302 redirect.");
        return false;
    } else if (statusCode == 302 || statusCode == 301) {
        redirectUrl = _getHeaderValue(headers, "Location");
        if (redirectUrl.length() == 0) {
            _setError("302 redirect but no Location header found");
            return false;
        }
        Serial.println("[SafeGithubOTA] Following redirect to download binary...");
    } else {
        _setError("Asset request returned HTTP %d (expected 302)", statusCode);
        return false;
    }

    // ---- Phase 2: Download binary from S3 (NO auth header) ----

    String s3Host, s3Path;
    uint16_t s3Port;
    if (!_parseUrl(redirectUrl.c_str(), s3Host, s3Port, s3Path)) {
        _setError("Failed to parse redirect URL");
        return false;
    }

    WiFiClientSecure s3Client;
    s3Client.setCACertBundle(rootca_crt_bundle_start);
    s3Client.setTimeout(_downloadTimeoutMs / 1000);

    if (!s3Client.connect(s3Host.c_str(), s3Port)) {
        _setError("Failed to connect to %s for binary download", s3Host.c_str());
        return false;
    }

    // CRITICAL: No Authorization header for the S3 request
    s3Client.print(String("GET ") + s3Path + " HTTP/1.1\r\n" +
                   "Host: " + s3Host + "\r\n" +
                   "User-Agent: SafeGithubOTA/1.0\r\n" +
                   "Connection: close\r\n\r\n");

    String s3Headers;
    int s3Status = _readHttpResponse(s3Client, s3Headers, _connectTimeoutMs);

    if (s3Status != 200) {
        s3Client.stop();
        _setError("Binary download returned HTTP %d", s3Status);
        return false;
    }

    // Get Content-Length
    String contentLenStr = _getHeaderValue(s3Headers, "Content-Length");
    uint32_t contentLength = 0;
    if (contentLenStr.length() > 0) {
        contentLength = (uint32_t)contentLenStr.toInt();
    }

    if (contentLength == 0 && expectedSize > 0) {
        contentLength = expectedSize;
    }

    Serial.printf("[SafeGithubOTA] Downloading %u bytes...\n", contentLength);

    // ---- Phase 3: Stream binary data to chunk writer ----

    uint8_t buf[SGO_CHUNK_SIZE];
    uint32_t bytesWritten = 0;
    uint32_t lastProgressMs = millis();
    uint32_t downloadStart = millis();

    while (s3Client.connected() || s3Client.available()) {
        // Check timeout
        if ((millis() - downloadStart) > _downloadTimeoutMs) {
            s3Client.stop();
            _setError("Download timed out after %u bytes", bytesWritten);
            return false;
        }

        if (s3Client.available()) {
            int bytesRead = s3Client.read(buf, SGO_CHUNK_SIZE);
            if (bytesRead > 0) {
                size_t written = chunkWriter(buf, bytesRead);
                if (written != (size_t)bytesRead) {
                    s3Client.stop();
                    _setError("Flash write failed at byte %u", bytesWritten);
                    return false;
                }
                bytesWritten += bytesRead;

                // Report progress (at most every 500ms to avoid spamming)
                if (progressCb && (millis() - lastProgressMs > 500)) {
                    progressCb(bytesWritten, contentLength);
                    lastProgressMs = millis();
                }
            }
        } else {
            delay(1);
        }
    }

    s3Client.stop();

    // Final progress callback
    if (progressCb) {
        progressCb(bytesWritten, contentLength);
    }

    // Verify we got enough data
    if (contentLength > 0 && bytesWritten < contentLength) {
        _setError("Incomplete download: %u of %u bytes", bytesWritten, contentLength);
        return false;
    }

    Serial.printf("[SafeGithubOTA] Download complete: %u bytes\n", bytesWritten);
    return true;
}
