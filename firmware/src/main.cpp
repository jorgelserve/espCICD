#include <Arduino.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPTelnetStream.h>
#include <SafeGithubOTA.h>
#include <SGO_Provisioning.h>
#include <stdarg.h>

const char *FW_VERSION = "0.2.6";
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// esp32oled-ci: WiFi provisioning portal (like esp32ToneHub) + OLED status.
// No saved network (or all fail) -> AP "esp32oled-ci" with captive portal at
// 192.168.4.1: scan, pick your network, type the password, save. Up to 5
// profiles in NVS, tried in slot order (slot 0 = home, slot 1 = hotspot...).

const char *AP_SSID = "esp32oled-ci";
const char *AP_PASSWORD = "esp32oled123";
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GW(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);
const uint8_t kMaxProfiles = 5;
const uint32_t kWifiTimeoutMs = 10000;
const uint16_t kTelnetPort = 23;
const uint8_t kOledCols = 12;  // visible chars: 72 px / 6 px font
const uint16_t kScrollStepMs = 70;  // 1 px per tick -> smooth linear scroll
const uint8_t kScrollPauseTicks = 10;  // ~700 ms pause at edges

class OledDisplay {
 public:
  void begin();
  void show(const String &l1, const String &l2, const String &l3 = "");
  void renderStatus(const String &mode, const String &ssid, const String &ip);
  void loop();

 private:
  void render();
  U8G2_SSD1306_72X40_ER_F_HW_I2C display{U8G2_R0, U8X8_PIN_NONE, 6, 5};
  String lines[3];
  uint16_t scrollOff[3] = {0, 0, 0};  // px into the line
  uint16_t textW[3] = {0, 0, 0};      // line width in px
  int16_t scrollDir[3] = {1, 1, 1};   // 1 = advancing, -1 = rewinding
  uint8_t scrollPause[3] = {0, 0, 0}; // ticks to wait at extremes
  uint32_t lastScrollMs = 0;
};

class WifiManager {
 public:
  bool begin();
  void loop();
  String mode() { return g_mode; }
  String ssid() { return g_ssid; }
  String ip() { return g_ip; }
  static String slotSsid(uint8_t i);
  static void saveSlot(uint8_t i, const String &ssid, const String &pass);
  static void clearSlot(uint8_t i);

 private:
  static String slotPass(uint8_t i);
  bool tryProfiles();
  void startAp();
  static Preferences prefs;  // static: shared by the static slot* helpers
  String g_mode = "boot";    // sta | prov
  String g_ssid = "-";
  String g_ip = "0.0.0.0";
};

class TelnetLogger {
 public:
  void begin();
  void loop() { telnetStream.loop(); }
  bool isConnected() { return telnetStream.isConnected(); }
  int available() { return telnetStream.available(); }
  int read() { return telnetStream.read(); }
  void print(const char *fmt, ...);

 private:
  ESPTelnetStream telnetStream;
  const uint16_t port = kTelnetPort;
};

class WebPortal {
 public:
  void begin();
  void handleClient() { server.handleClient(); }
  void processDns();

 private:
  void sendPortal();
  WebServer server{80};
  DNSServer dns;
  HTTPUpdateServer httpUpdater;
};

class GitHubUpdater {
 public:
  void begin(const String &version);
  void loop();
  void checkNow();

 private:
  SafeGithubOTA ghOta;
  bool ready = false;
};

Preferences WifiManager::prefs;

OledDisplay oled;
WifiManager wifi;
TelnetLogger logger;
WebPortal portal;
GitHubUpdater updater;

// ********** OledDisplay **********

void OledDisplay::begin() {
  display.begin();
  display.setFont(u8g2_font_6x10_tf);
}

void OledDisplay::show(const String &l1, const String &l2, const String &l3) {
  const String *in[3] = {&l1, &l2, &l3};
  for (uint8_t i = 0; i < 3; ++i) {
    const String &s = *in[i];
    if (s == lines[i]) continue;  // new content: restart that line's scroll
    lines[i] = s;
    scrollOff[i] = 0;
    scrollDir[i] = 1;
    scrollPause[i] = 0;
    textW[i] = s.length() ? display.getUTF8Width(s.c_str()) : 0;
  }
  render();
}

// Smooth scroll: 1 px every kScrollStepMs, with pause at edges.
void OledDisplay::loop() {
  if (millis() - lastScrollMs < kScrollStepMs) return;
  lastScrollMs = millis();
  const uint16_t width = display.getWidth();
  bool moved = false;
  for (uint8_t i = 0; i < 3; ++i) {
    if (lines[i].length() <= kOledCols || textW[i] <= width) continue;
    const uint16_t maxOff = textW[i] - width;
    if (scrollPause[i] > 0) {
      scrollPause[i]--;
      moved = true;
      continue;
    }
    if (scrollDir[i] > 0 && scrollOff[i] >= maxOff) {
      scrollDir[i] = -1;
      scrollPause[i] = kScrollPauseTicks;
    } else if (scrollDir[i] < 0 && scrollOff[i] <= 0) {
      scrollDir[i] = 1;
      scrollPause[i] = kScrollPauseTicks;
    } else {
      scrollOff[i] = scrollOff[i] + scrollDir[i];
    }
    moved = true;
  }
  if (moved) render();
}

void OledDisplay::render() {
  display.clearBuffer();
  for (uint8_t i = 0; i < 3; ++i) {
    if (!lines[i].length()) continue;
    display.drawUTF8(-static_cast<int16_t>(scrollOff[i]), 10 + 10 * i, lines[i].c_str());
  }
  display.sendBuffer();
}

void OledDisplay::renderStatus(const String &mode, const String &ssid, const String &ip) {
  show(mode, ssid, ip);
}

// ********** WifiManager **********

String WifiManager::slotSsid(uint8_t i) { return prefs.getString(("ssid" + String(i)).c_str(), ""); }
String WifiManager::slotPass(uint8_t i) { return prefs.getString(("pass" + String(i)).c_str(), ""); }

void WifiManager::saveSlot(uint8_t i, const String &ssid, const String &pass) {
  prefs.putString(("ssid" + String(i)).c_str(), ssid);
  prefs.putString(("pass" + String(i)).c_str(), pass);
}

void WifiManager::clearSlot(uint8_t i) {
  prefs.remove(("ssid" + String(i)).c_str());
  prefs.remove(("pass" + String(i)).c_str());
}

bool WifiManager::begin() {
  prefs.begin("esp32oled", false);
  if (tryProfiles()) return true;
  startAp();
  return false;
}

bool WifiManager::tryProfiles() {
  WiFi.mode(WIFI_STA);
  for (uint8_t i = 0; i < kMaxProfiles; ++i) {
    const String ssid = slotSsid(i);
    if (!ssid.length()) continue;
    oled.show("wifi", ssid);
    logger.print("trying %s", ssid.c_str());
    WiFi.begin(ssid.c_str(), slotPass(i).c_str());
    const uint32_t start = millis();
    while (millis() - start < kWifiTimeoutMs) {
      if (WiFi.status() == WL_CONNECTED) {
        g_mode = "sta";
        g_ssid = ssid;
        g_ip = WiFi.localIP().toString();
        logger.print(" -> ok, ip %s\n", g_ip.c_str());
        return true;
      }
      delay(200);
    }
    logger.print(" -> failed\n");
    WiFi.disconnect();
  }
  return false;
}

void WifiManager::startAp() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_GW, AP_SUBNET);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  g_mode = "prov";
  g_ssid = AP_SSID;
  g_ip = "192.168.4.1";
  logger.print("provisioning AP %s, portal at %s\n", AP_SSID, g_ip.c_str());
}

void WifiManager::loop() {
  // Keep OLED in sync with actual WiFi state (IP can change via DHCP).
  static uint32_t lastRefresh = 0;
  if (millis() - lastRefresh > 3000) {
    lastRefresh = millis();
    if (g_mode == "sta") {
      if (WiFi.status() == WL_CONNECTED) {
        g_ip = WiFi.localIP().toString();
        oled.renderStatus(g_mode, g_ssid, g_ip);
      } else {
        oled.show("wifi lost", "retry");
      }
    }
  }

  // In STA mode: if WiFi drops for a while, reboot and retry profiles.
  static uint32_t lostSince = 0;
  if (g_mode == "sta") {
    if (WiFi.status() != WL_CONNECTED) {
      if (!lostSince) lostSince = millis();
      if (millis() - lostSince > 20000) ESP.restart();
    } else {
      lostSince = 0;
    }
  }
}

// ********** TelnetLogger **********

// Status messages go to USB serial and to the telnet client (if any).
void TelnetLogger::print(const char *fmt, ...) {
  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print(buf);
  if (isConnected()) telnetStream.print(buf);
}

void TelnetLogger::begin() {
  telnetStream.onConnect([](String ip) {
    logger.print("\n*** esp32oled-ci telnet ***\n");
    logger.print("client %s\nmode %s | ssid %s | ip %s\n", ip.c_str(),
                 wifi.mode().c_str(), wifi.ssid().c_str(), wifi.ip().c_str());
  });
  telnetStream.onDisconnect([](String ip) { logger.print("telnet: %s disconnected\n", ip.c_str()); });
  telnetStream.begin(port);
  logger.print("telnet server on port %u\n", port);
}

// ********** WebPortal **********

void WebPortal::sendPortal() {
  // Scanning from inside the AP takes ~2-3 s; do it before building the page.
  logger.print("scanning for portal page\n");
  const int n = WiFi.scanNetworks();

  String html = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>esp32oled-ci</title><style>
body{font-family:sans-serif;max-width:430px;margin:24px auto;padding:0 12px}
.net{display:block;padding:10px;margin:6px 0;border:1px solid #ccc;border-radius:6px;text-decoration:none;color:#000}
.net:hover{background:#f0f0f0}.rssi{float:right;color:#888}
input{width:100%;padding:10px;margin:6px 0;box-sizing:border-box}
button{padding:10px 18px}a{color:#0366d6}small{color:#666}
</style></head><body>
<h2>esp32oled-ci</h2>
<p><small>MODE &middot; IP</small></p>
<h3>Networks</h3>)HTML";

  if (n <= 0) {
    html += "<p>no networks found, <a href='/'>rescan</a></p>";
  }
  for (int i = 0; i < n; ++i) {
    if (WiFi.SSID(i).length() == 0) continue;
    String ssid = WiFi.SSID(i);
    // escape quotes for the query param
    String enc;
    for (size_t k = 0; k < ssid.length(); ++k) {
      const char c = ssid[k];
      if (c == '"' || c == '\\' || c == '&') enc += '%', enc += String(c, HEX);
      else enc += c;
    }
    html += "<a class='net' href='/net?ssid=" + enc + "'>" + ssid +
            "<span class='rssi'>" + String(WiFi.RSSI(i)) + " dBm</span></a>";
  }
  WiFi.scanDelete();

  html += "<h3>Configure</h3><form method='POST' action='/save'>";
  html += "<small>SSID</small><input name='ssid' id='ssid' value='" + server.arg("ssid") + "' required>";
  html += "<small>Password (empty for open)</small><input name='pass' type='password'>";
  html += "<small>Slot</small><select name='slot'>";
  for (uint8_t i = 0; i < kMaxProfiles; ++i) {
    const String saved = WifiManager::slotSsid(i);
    html += "<option value='" + String(i) + "'>slot " + String(i) +
            (saved.length() ? ": " + saved : ": empty") + "</option>";
  }
  html += "</select><br><br><button type='submit'>Save &amp; connect</button></form>";

  html += "<h3>Saved</h3><ul>";
  for (uint8_t i = 0; i < kMaxProfiles; ++i) {
    const String saved = WifiManager::slotSsid(i);
    if (saved.length()) {
      html += "<li>slot " + String(i) + ": " + saved +
              " <a href='/forget?i=" + String(i) + "'>forget</a></li>";
    }
  }
  html += "</ul><p><a href='/'>rescan</a> &middot; <a href='/status'>status JSON</a></p></body></html>";

  html.replace("MODE", wifi.mode());
  html.replace("IP", wifi.ip());
  server.send(200, "text/html", html);
}

void WebPortal::begin() {
  server.on("/", HTTP_GET, [this] { sendPortal(); });
  server.on("/net", HTTP_GET, [this] { sendPortal(); });  // same page with ssid prefilled

  server.on("/status", HTTP_GET, [this] {
    String body = "{\"mode\":\"" + wifi.mode() + "\",\"ssid\":\"" + wifi.ssid() +
                  "\",\"ip\":\"" + wifi.ip() + "\",\"profiles\":[";
    bool first = true;
    for (uint8_t i = 0; i < kMaxProfiles; ++i) {
      const String ssid = WifiManager::slotSsid(i);
      if (ssid.length()) {
        if (!first) body += ",";
        body += "{\"slot\":" + String(i) + ",\"ssid\":\"" + ssid + "\"}";
        first = false;
      }
    }
    body += "]}";
    server.send(200, "application/json", body);
  });

  server.on("/save", HTTP_POST, [this] {
    const String ssid = server.arg("ssid");
    const String pass = server.arg("pass");
    long slot = 0;
    if (server.hasArg("slot")) slot = server.arg("slot").toInt();
    if (!ssid.length() || slot < 0 || slot >= kMaxProfiles) {
      server.send(400, "text/plain", "bad request");
      return;
    }
    WifiManager::saveSlot(static_cast<uint8_t>(slot), ssid, pass);
    server.send(200, "text/html",
                "Saved. Rebooting to connect to " + ssid +
                "... (if it fails, the portal comes back)");
    oled.show("saved", ssid);
    delay(750);
    ESP.restart();
  });

  server.on("/forget", HTTP_GET, [this] {
    const long slot = server.arg("i").toInt();
    if (slot >= 0 && slot < kMaxProfiles) WifiManager::clearSlot(static_cast<uint8_t>(slot));
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  });

  server.onNotFound([this] {
    if (wifi.mode() == "prov") {
      server.sendHeader("Location", "http://192.168.4.1/");
      server.send(302, "text/plain", "");
    } else {
      server.send(404, "text/plain", "Not Found");
    }
  });

  server.begin();

  // Official Espressif OTA updater: POST /update (multipart, field "update").
  httpUpdater.setup(&server);

  if (wifi.mode() == "prov") dns.start(53, "*", AP_IP);
  logger.print("HTTP server started (GET /, /status, /save; POST /update for OTA)\n");
}

void WebPortal::processDns() {
  if (wifi.mode() == "prov") dns.processNextRequest();
}

// ********** GitHubUpdater **********

void GitHubUpdater::begin(const String &version) {
  ghOta.setPatRequired(false);
  ghOta.setVersion(version.c_str());

  // Public repo defaults: stored once in NVS so SafeGithubOTA can check.
  if (!ghOta.isProvisioned()) {
    SGO_Credentials creds;
    creds.valid = true;
    strncpy(creds.owner, "jorgelserve", sizeof(creds.owner) - 1);
    creds.owner[sizeof(creds.owner) - 1] = '\0';
    strncpy(creds.repo, "espCICD", sizeof(creds.repo) - 1);
    creds.repo[sizeof(creds.repo) - 1] = '\0';
    creds.pat[0] = '\0';
    strncpy(creds.binFilename, "firmware.bin", sizeof(creds.binFilename) - 1);
    creds.binFilename[sizeof(creds.binFilename) - 1] = '\0';
    SGO_Provisioning::saveCredentials(creds);
  }

  ghOta.setAutoCheckInterval(21600);  // 6h in seconds
  ghOta.onProgress([](uint32_t written, uint32_t total) {
    logger.print("ota %u%%\n", total ? written * 100 / total : 0);
  });
  ghOta.onValidation([]() {
    logger.print("ota validation: ok, confirming\n");
    return true;
  });
  ghOta.begin();
  ready = true;
}

void GitHubUpdater::loop() {
  if (!ready) return;
  ghOta.loop();  // fires checkAndUpdate() on the auto-check interval
}

void GitHubUpdater::checkNow() {
  if (!ready) return;
  SGO_UpdateInfo info;
  const SGO_Error err = ghOta.checkForUpdate(&info);
  if (err == SGO_Error::OK) {
    logger.print("update %s -> %s (%u bytes), applying...\n",
                 info.currentVersion, info.remoteVersion, info.firmwareSize);
    ghOta.applyUpdate();  // reboots on success
    logger.print("ota apply failed: %s\n", ghOta.getLastError());
  } else if (err == SGO_Error::ALREADY_CURRENT) {
    logger.print("firmware is current (%s)\n", FW_VERSION);
  } else {
    logger.print("ota check failed: %s\n", ghOta.getLastError());
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  logger.print("\nesp32oled-ci starting\n");

  oled.begin();
  oled.show("esp32oled-ci", FW_VERSION);

  wifi.begin();
  logger.begin();
  portal.begin();
  updater.begin(FW_VERSION);
  oled.renderStatus(wifi.mode(), wifi.ssid(), wifi.ip());
}

void loop() {
  logger.loop();
  portal.processDns();
  portal.handleClient();
  updater.loop();
  wifi.loop();
  oled.loop();

  if (logger.isConnected()) {
    while (logger.isConnected() && logger.available()) {
      char c = (char)logger.read();
      static String line;
      if (c == '\r') continue;
      if (c == '\n') {
        line.trim();
        if (line.equalsIgnoreCase("check")) {
          logger.print("checking for updates...\n");
          updater.checkNow();
        } else if (line.equalsIgnoreCase("status")) {
          logger.print("mode=%s ssid=%s ip=%s version=%s\n",
                       wifi.mode().c_str(), wifi.ssid().c_str(),
                       wifi.ip().c_str(), FW_VERSION);
        } else {
          logger.print("commands: check | status\n");
        }
        line = "";
      } else {
        line += c;
      }
    }
  }
}
