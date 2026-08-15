#include "SGO_Provisioning.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>

const char* SGO_Provisioning::NVS_NAMESPACE = "sgo_creds";
const char* SGO_Provisioning::KEY_OWNER = "owner";
const char* SGO_Provisioning::KEY_REPO = "repo";
const char* SGO_Provisioning::KEY_PAT = "pat";
const char* SGO_Provisioning::KEY_BIN = "bin";

bool SGO_Provisioning::loadCredentials(SGO_Credentials& creds, bool patRequired) {
    memset(&creds, 0, sizeof(creds));
    creds.valid = false;

    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true)) {
        return false;
    }

    String owner = prefs.getString(KEY_OWNER, "");
    String repo = prefs.getString(KEY_REPO, "");
    String pat = prefs.getString(KEY_PAT, "");
    String bin = prefs.getString(KEY_BIN, "");
    prefs.end();

    if (owner.length() == 0 || repo.length() == 0 ||
        (patRequired && pat.length() == 0) || bin.length() == 0) {
        return false;
    }

    strncpy(creds.owner, owner.c_str(), sizeof(creds.owner) - 1);
    strncpy(creds.repo, repo.c_str(), sizeof(creds.repo) - 1);
    strncpy(creds.pat, pat.c_str(), sizeof(creds.pat) - 1);
    strncpy(creds.binFilename, bin.c_str(), sizeof(creds.binFilename) - 1);
    creds.valid = true;

    return true;
}

bool SGO_Provisioning::saveCredentials(const SGO_Credentials& creds) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false)) {
        return false;
    }

    prefs.putString(KEY_OWNER, creds.owner);
    prefs.putString(KEY_REPO, creds.repo);
    prefs.putString(KEY_PAT, creds.pat);
    prefs.putString(KEY_BIN, creds.binFilename);
    prefs.end();

    return true;
}

void SGO_Provisioning::clearCredentials() {
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, false)) {
        prefs.clear();
        prefs.end();
    }
}

bool SGO_Provisioning::hasCredentials(bool patRequired) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true)) {
        return false;
    }

    bool has = prefs.getString(KEY_OWNER, "").length() > 0 &&
               prefs.getString(KEY_REPO, "").length() > 0 &&
               (!patRequired || prefs.getString(KEY_PAT, "").length() > 0) &&
               prefs.getString(KEY_BIN, "").length() > 0;
    prefs.end();
    return has;
}

// Captive portal HTML form. Split into three parts so the PAT field can
// be omitted for public repos without duplicating the markup/CSS:
//   HEAD + (PAT field, only if patRequired) + TAIL
static const char SGO_PORTAL_HTML_HEAD[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>SafeGithubOTA Setup</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background: #1a1a2e;
      color: #fff;
      min-height: 100vh;
      display: flex;
      justify-content: center;
      padding: 20px;
    }
    .container {
      max-width: 420px;
      width: 100%;
      margin-top: 20px;
    }
    h2 {
      text-align: center;
      color: #4ade80;
      margin-bottom: 8px;
      font-size: 1.5rem;
    }
    .subtitle {
      text-align: center;
      color: #888;
      font-size: 0.85rem;
      margin-bottom: 30px;
    }
    .card {
      background: rgba(255,255,255,0.05);
      border: 1px solid rgba(255,255,255,0.1);
      border-radius: 16px;
      padding: 28px;
    }
    label {
      display: block;
      margin-top: 18px;
      font-weight: 600;
      color: #ccc;
      font-size: 0.9rem;
    }
    label:first-of-type { margin-top: 0; }
    input {
      width: 100%;
      padding: 11px 14px;
      margin-top: 6px;
      border: 1px solid #333;
      border-radius: 8px;
      background: #16213e;
      color: #fff;
      font-size: 0.95rem;
      outline: none;
      transition: border-color 0.2s;
    }
    input:focus { border-color: #4ade80; }
    input::placeholder { color: #555; }
    .hint {
      font-size: 0.75rem;
      color: #666;
      margin-top: 4px;
    }
    button {
      width: 100%;
      padding: 14px;
      margin-top: 28px;
      background: #4ade80;
      border: none;
      border-radius: 8px;
      color: #000;
      font-weight: 600;
      font-size: 1rem;
      cursor: pointer;
      transition: background 0.2s;
    }
    button:hover { background: #22c55e; }
    .footer {
      text-align: center;
      margin-top: 20px;
      color: #444;
      font-size: 0.75rem;
    }
  </style>
</head>
<body>
  <div class="container">
    <h2>SafeGithubOTA</h2>
    <p class="subtitle">Developer Provisioning Portal</p>
    <div class="card">
      <form method="POST" action="/save">
        <label>Repository Owner</label>
        <input name="owner" placeholder="e.g. mycompany" required maxlength="63">
        <div class="hint">GitHub username or organization</div>

        <label>Repository Name</label>
        <input name="repo" placeholder="e.g. my-firmware" required maxlength="63">
)rawliteral";

// PAT field — appended to HEAD only when a Personal Access Token is
// required (private repos). Omitted via setPatRequired(false).
static const char SGO_PORTAL_HTML_PAT[] PROGMEM = R"rawliteral(
        <label>Personal Access Token</label>
        <input name="pat" type="password" placeholder="ghp_xxxxxxxxxxxx" required maxlength="127">
        <div class="hint">Classic PAT needs 'repo' scope; fine-grained needs 'Contents' read</div>
)rawliteral";

static const char SGO_PORTAL_HTML_TAIL[] PROGMEM = R"rawliteral(
        <label>Firmware Filename</label>
        <input name="bin" placeholder="e.g. firmware.bin" required maxlength="63">
        <div class="hint">Must match the .bin asset name in your GitHub Release</div>

        <button type="submit">Save Configuration</button>
      </form>
    </div>
    <p class="footer">SafeGithubOTA &mdash; Secure OTA from GitHub</p>
  </div>
</body>
</html>
)rawliteral";

static const char SGO_PORTAL_SUCCESS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Configuration Saved</title>
  <style>
    body {
      font-family: -apple-system, sans-serif;
      background: #1a1a2e;
      color: #fff;
      display: flex;
      justify-content: center;
      align-items: center;
      min-height: 100vh;
      margin: 0;
    }
    .container {
      text-align: center;
      padding: 40px;
    }
    .check {
      font-size: 4rem;
      margin-bottom: 20px;
    }
    h2 { color: #4ade80; margin-bottom: 10px; }
    p { color: #888; }
  </style>
</head>
<body>
  <div class="container">
    <div class="check">&#10003;</div>
    <h2>Configuration Saved</h2>
    <p>You can now disconnect from this network.<br>The device will use these settings for OTA updates.</p>
  </div>
</body>
</html>
)rawliteral";

bool SGO_Provisioning::launchPortal(
    const char* apSSID,
    const char* apPassword,
    uint32_t timeoutSeconds,
    bool patRequired
) {
    // Save current WiFi mode to restore later
    wifi_mode_t previousMode = WiFi.getMode();

    // Start AP
    WiFi.mode(WIFI_AP);
    delay(100);

    // Lower TX power for ESP32-C3 boards (many have inadequate power supply
    // design that prevents WiFi from working at default 19.5dBm)
#if CONFIG_IDF_TARGET_ESP32C3
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    delay(100);
#endif

    if (apPassword != nullptr && strlen(apPassword) >= 8) {
        WiFi.softAP(apSSID, apPassword);
    } else {
        WiFi.softAP(apSSID);
    }
    delay(500);

    IPAddress apIP = WiFi.softAPIP();
    Serial.print("[SafeGithubOTA] Portal started at: ");
    Serial.println(apIP);
    Serial.print("[SafeGithubOTA] Connect to WiFi: ");
    Serial.println(apSSID);

    // DNS server for captive portal redirect
    DNSServer dnsServer;
    dnsServer.start(53, "*", apIP);

    WebServer server(80);
    volatile bool done = false;
    volatile bool saved = false;

    // Serve the config form. The PAT field is included only when a
    // Personal Access Token is required (private repos).
    server.on("/", HTTP_GET, [&server, patRequired]() {
        String html = FPSTR(SGO_PORTAL_HTML_HEAD);
        if (patRequired) html += FPSTR(SGO_PORTAL_HTML_PAT);
        html += FPSTR(SGO_PORTAL_HTML_TAIL);
        server.send(200, "text/html", html);
    });

    // Handle form submission
    server.on("/save", HTTP_POST, [&server, &done, &saved, patRequired]() {
        String owner = server.arg("owner");
        String repo = server.arg("repo");
        String pat = server.arg("pat");
        String bin = server.arg("bin");

        owner.trim();
        repo.trim();
        pat.trim();
        bin.trim();

        if (owner.length() == 0 || repo.length() == 0 ||
            (patRequired && pat.length() == 0) || bin.length() == 0) {
            server.send(400, "text/html",
                "<html><body style='background:#1a1a2e;color:#f87171;font-family:sans-serif;text-align:center;padding:40px;'>"
                "<h2>All fields are required</h2>"
                "<p><a href='/' style='color:#4ade80;'>Go back</a></p>"
                "</body></html>");
            return;
        }

        SGO_Credentials creds;
        memset(&creds, 0, sizeof(creds));
        strncpy(creds.owner, owner.c_str(), sizeof(creds.owner) - 1);
        strncpy(creds.repo, repo.c_str(), sizeof(creds.repo) - 1);
        strncpy(creds.pat, pat.c_str(), sizeof(creds.pat) - 1);
        strncpy(creds.binFilename, bin.c_str(), sizeof(creds.binFilename) - 1);

        if (saveCredentials(creds)) {
            server.send_P(200, "text/html", SGO_PORTAL_SUCCESS_HTML);
            saved = true;
            done = true;
            Serial.println("[SafeGithubOTA] Credentials saved successfully");
        } else {
            server.send(500, "text/html",
                "<html><body style='background:#1a1a2e;color:#f87171;font-family:sans-serif;text-align:center;padding:40px;'>"
                "<h2>Failed to save</h2>"
                "<p>NVS write error. <a href='/' style='color:#4ade80;'>Try again</a></p>"
                "</body></html>");
        }
    });

    // Captive portal: redirect all unknown requests to the form
    server.onNotFound([&server]() {
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "");
    });

    server.begin();

    uint32_t startMs = millis();
    uint32_t timeoutMs = (timeoutSeconds > 0) ? (timeoutSeconds * 1000UL) : 0;

    while (!done) {
        dnsServer.processNextRequest();
        server.handleClient();
        delay(1);

        // Check timeout
        if (timeoutMs > 0 && (millis() - startMs) >= timeoutMs) {
            Serial.println("[SafeGithubOTA] Portal timed out");
            break;
        }
    }

    // Give the success page a moment to render before tearing down
    if (saved) {
        delay(2000);
    }

    // Tear down
    server.stop();
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    delay(100);

    // Restore previous WiFi mode
    WiFi.mode(previousMode);

    return saved;
}
