#include "config_portal.h"
#include "config.h"
#include <WiFi.h>

// ── Global state ─────────────────────────────────────────────────
RuntimeConfig cfg;
bool apMode = false;
static Preferences prefs;
static DNSServer dnsServer;
static const byte DNS_PORT = 53;

// ── NVS Keys (max 15 chars each) ────────────────────────────────
#define NVS_NS "meetcfg"

// ── Load from NVS with config.h fallback ─────────────────────────
void configSetDefaults() {
  cfg.wifiSsid       = WIFI_SSID;
  cfg.wifiPass       = WIFI_PASSWORD;
  cfg.mqttHost       = MQTT_HOST;
  cfg.mqttPort       = MQTT_PORT;
  cfg.mqttUser       = MQTT_USER;
  cfg.mqttPass       = MQTT_PASS;
  cfg.mqttClientId   = MQTT_CLIENT_ID;
  cfg.mqttPrefix     = MQTT_PREFIX;
  cfg.haHost         = HA_HOST;
  cfg.haPort         = HA_PORT;
  cfg.haToken        = HA_TOKEN;
  cfg.haCalendarEntity = HA_CALENDAR_ENTITY;
  cfg.haNextTitle    = HA_NEXT_TITLE;
  cfg.haNextStart    = HA_NEXT_START;
  cfg.roomName       = ROOM_NAME;
  cfg.companyName    = COMPANY_NAME;
  cfg.httpIntervalMs = HTTP_INTERVAL_MS;
  cfg.minDrawMs      = MIN_DRAW_MS;
  cfg.mqttSettleMs   = MQTT_SETTLE_MS;
  cfg.fullRefreshMs  = FULL_REFRESH_MS;
  cfg.ntpServer      = NTP_SERVER;
  cfg.timezone       = TIMEZONE;
}

void configLoadFromNVS() {
  configSetDefaults();

  prefs.begin(NVS_NS, true);  // read-only

  if (prefs.isKey("wifiSsid")) {
    cfg.wifiSsid       = prefs.getString("wifiSsid", cfg.wifiSsid);
    cfg.wifiPass       = prefs.getString("wifiPass",  cfg.wifiPass);
    cfg.mqttHost       = prefs.getString("mqttHost",  cfg.mqttHost);
    cfg.mqttPort       = prefs.getUShort("mqttPort",  cfg.mqttPort);
    cfg.mqttUser       = prefs.getString("mqttUser",  cfg.mqttUser);
    cfg.mqttPass       = prefs.getString("mqttPass",  cfg.mqttPass);
    cfg.mqttClientId   = prefs.getString("mqttClId",  cfg.mqttClientId);
    cfg.mqttPrefix     = prefs.getString("mqttPfx",   cfg.mqttPrefix);
    cfg.haHost         = prefs.getString("haHost",    cfg.haHost);
    cfg.haPort         = prefs.getUShort("haPort",    cfg.haPort);
    cfg.haToken        = prefs.getString("haToken",   cfg.haToken);
    cfg.haCalendarEntity = prefs.getString("haCalEnt", cfg.haCalendarEntity);
    cfg.haNextTitle    = prefs.getString("haNextTtl", cfg.haNextTitle);
    cfg.haNextStart    = prefs.getString("haNextSt",  cfg.haNextStart);
    cfg.roomName       = prefs.getString("roomName",  cfg.roomName);
    cfg.companyName    = prefs.getString("company",   cfg.companyName);
    cfg.httpIntervalMs = prefs.getULong("httpIntMs",  cfg.httpIntervalMs);
    cfg.minDrawMs      = prefs.getULong("minDrawMs",  cfg.minDrawMs);
    cfg.mqttSettleMs   = prefs.getULong("settleMs",   cfg.mqttSettleMs);
    cfg.fullRefreshMs  = prefs.getULong("fullRefMs",  cfg.fullRefreshMs);
    cfg.ntpServer      = prefs.getString("ntpSrv",    cfg.ntpServer);
    cfg.timezone       = prefs.getString("tz",        cfg.timezone);
    Serial.println(F("[CFG] Loaded from NVS"));
  } else {
    Serial.println(F("[CFG] No NVS data, using config.h defaults"));
  }

  prefs.end();
}

static void configSaveToNVS() {
  prefs.begin(NVS_NS, false);  // read-write
  prefs.putString("wifiSsid",  cfg.wifiSsid);
  prefs.putString("wifiPass",  cfg.wifiPass);
  prefs.putString("mqttHost",  cfg.mqttHost);
  prefs.putUShort("mqttPort",  cfg.mqttPort);
  prefs.putString("mqttUser",  cfg.mqttUser);
  prefs.putString("mqttPass",  cfg.mqttPass);
  prefs.putString("mqttClId",  cfg.mqttClientId);
  prefs.putString("mqttPfx",   cfg.mqttPrefix);
  prefs.putString("haHost",    cfg.haHost);
  prefs.putUShort("haPort",    cfg.haPort);
  prefs.putString("haToken",   cfg.haToken);
  prefs.putString("haCalEnt",  cfg.haCalendarEntity);
  prefs.putString("haNextTtl", cfg.haNextTitle);
  prefs.putString("haNextSt",  cfg.haNextStart);
  prefs.putString("roomName",  cfg.roomName);
  prefs.putString("company",   cfg.companyName);
  prefs.putULong("httpIntMs",  cfg.httpIntervalMs);
  prefs.putULong("minDrawMs",  cfg.minDrawMs);
  prefs.putULong("settleMs",   cfg.mqttSettleMs);
  prefs.putULong("fullRefMs",  cfg.fullRefreshMs);
  prefs.putString("ntpSrv",    cfg.ntpServer);
  prefs.putString("tz",        cfg.timezone);
  prefs.end();
  Serial.println(F("[CFG] Saved to NVS"));
}

// ── WiFi STA ─────────────────────────────────────────────────────
bool configStartWiFi(unsigned long timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
  Serial.printf("[CFG] WiFi STA connecting to '%s'...", cfg.wifiSsid.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" OK: " + WiFi.localIP().toString());
    apMode = false;
    return true;
  }

  Serial.println(F(" FAIL"));
  return false;
}

// ── WiFi AP + Captive Portal ─────────────────────────────────────
void configStartAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Meetingroom-Setup");
  delay(100);  // let AP stabilize
  IPAddress apIP = WiFi.softAPIP();
  Serial.println("[CFG] AP Mode: " + apIP.toString());

  // DNS: redirect everything to our IP (captive portal)
  dnsServer.start(DNS_PORT, "*", apIP);
  apMode = true;
}

void configLoopDNS() {
  if (apMode) {
    dnsServer.processNextRequest();
  }
}

// ── HTML Helpers ─────────────────────────────────────────────────
static String htmlEscape(const String& s) {
  String r = s;
  r.replace("&", "&amp;");
  r.replace("<", "&lt;");
  r.replace(">", "&gt;");
  r.replace("\"", "&quot;");
  return r;
}

static String inputField(const char* label, const char* name, const String& value, const char* type = "text") {
  return "<label>" + String(label) + "</label>"
         "<input type=\"" + String(type) + "\" name=\"" + String(name) +
         "\" value=\"" + htmlEscape(value) + "\">";
}

static String numField(const char* label, const char* name, unsigned long value) {
  return "<label>" + String(label) + "</label>"
         "<input type=\"number\" name=\"" + String(name) +
         "\" value=\"" + String(value) + "\">";
}

// ── Config Page HTML ─────────────────────────────────────────────
static const char CONFIG_CSS[] PROGMEM = R"rawliteral(
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;max-width:600px;margin:0 auto;padding:16px;background:#0f0f23;color:#ccc}
h1{color:#00d4ff;margin-bottom:8px;font-size:22px}
.status{background:#16213e;padding:12px;border-radius:6px;margin:8px 0;font-size:13px;line-height:1.6}
.status b{color:#00d4ff}
fieldset{border:1px solid #333;border-radius:6px;padding:12px;margin:12px 0}
legend{color:#00d4ff;font-weight:bold;padding:0 6px}
label{display:block;margin:8px 0 2px;font-size:13px;color:#aaa}
input,select{width:100%;padding:8px;background:#16213e;border:1px solid #333;border-radius:4px;color:#eee;font-size:14px}
input:focus{border-color:#00d4ff;outline:none}
.row{display:flex;gap:10px}
.row>*{flex:1}
button{background:#00d4ff;color:#000;border:none;padding:10px 24px;border-radius:4px;cursor:pointer;font-size:15px;font-weight:bold;width:100%;margin-top:16px}
button:hover{background:#00a8cc}
.btn-reset{background:#ff4444;margin-top:8px}
.btn-reset:hover{background:#cc0000}
.warn{color:#ff4;font-size:12px;margin-top:4px}
.ok{color:#0f0}
.err{color:#f44}
)rawliteral";

static void handleConfigPage(WebServer& server) {
  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Meetingroom Display Config</title><style>");
  html += FPSTR(CONFIG_CSS);
  html += F("</style></head><body>");
  html += F("<h1>&#x2699; Meetingroom Display</h1>");

  // Status box
  html += F("<div class='status'>");
  if (apMode) {
    html += F("<b>Modus:</b> <span class='warn'>AP Setup</span><br>");
    html += F("<b>AP IP:</b> ") + WiFi.softAPIP().toString() + "<br>";
  } else {
    html += F("<b>Modus:</b> <span class='ok'>WiFi verbunden</span><br>");
    html += F("<b>IP:</b> ") + WiFi.localIP().toString() + "<br>";
    html += F("<b>RSSI:</b> ") + String(WiFi.RSSI()) + " dBm<br>";
  }
  html += F("<b>Uptime:</b> ") + String(millis() / 60000) + " min<br>";
  html += F("<b>Free Heap:</b> ") + String(ESP.getFreeHeap() / 1024) + " KB<br>";
  html += F("<b>Firmware:</b> v4.0");
  html += F("</div>");

  // Form
  html += F("<form method='POST' action='/config/save'>");

  // WiFi
  html += F("<fieldset><legend>WiFi</legend>");
  html += inputField("SSID", "wifiSsid", cfg.wifiSsid);
  html += inputField("Passwort", "wifiPass", cfg.wifiPass, "password");
  html += F("</fieldset>");

  // MQTT
  html += F("<fieldset><legend>MQTT</legend>");
  html += F("<div class='row'>");
  html += "<div>" + inputField("Host", "mqttHost", cfg.mqttHost) + "</div>";
  html += "<div>" + numField("Port", "mqttPort", cfg.mqttPort) + "</div>";
  html += F("</div>");
  html += inputField("User", "mqttUser", cfg.mqttUser);
  html += inputField("Passwort", "mqttPass", cfg.mqttPass, "password");
  html += F("<div class='row'>");
  html += "<div>" + inputField("Client ID", "mqttClId", cfg.mqttClientId) + "</div>";
  html += "<div>" + inputField("Topic Prefix", "mqttPfx", cfg.mqttPrefix) + "</div>";
  html += F("</div></fieldset>");

  // Home Assistant
  html += F("<fieldset><legend>Home Assistant</legend>");
  html += F("<div class='row'>");
  html += "<div>" + inputField("Host", "haHost", cfg.haHost) + "</div>";
  html += "<div>" + numField("Port", "haPort", cfg.haPort) + "</div>";
  html += F("</div>");
  html += inputField("Token", "haToken", cfg.haToken, "password");
  html += inputField("Kalender Entity", "haCalEnt", cfg.haCalendarEntity);
  html += F("<div class='row'>");
  html += "<div>" + inputField("Next Title Entity", "haNextTtl", cfg.haNextTitle) + "</div>";
  html += "<div>" + inputField("Next Start Entity", "haNextSt", cfg.haNextStart) + "</div>";
  html += F("</div></fieldset>");

  // Display
  html += F("<fieldset><legend>Display</legend>");
  html += F("<div class='row'>");
  html += "<div>" + inputField("Raumname", "roomName", cfg.roomName) + "</div>";
  html += "<div>" + inputField("Firmenname", "company", cfg.companyName) + "</div>";
  html += F("</div></fieldset>");

  // Timing
  html += F("<fieldset><legend>Timing (Sekunden)</legend>");
  html += F("<div class='row'>");
  html += "<div>" + numField("HTTP Poll", "httpIntS", cfg.httpIntervalMs / 1000) + "</div>";
  html += "<div>" + numField("Min. Redraw", "minDrawS", cfg.minDrawMs / 1000) + "</div>";
  html += F("</div><div class='row'>");
  html += "<div>" + numField("MQTT Settle", "settleS", cfg.mqttSettleMs / 1000) + "</div>";
  html += "<div>" + numField("Full Refresh", "fullRefS", cfg.fullRefreshMs / 1000) + "</div>";
  html += F("</div></fieldset>");

  // NTP
  html += F("<fieldset><legend>Zeit</legend>");
  html += F("<div class='row'>");
  html += "<div>" + inputField("NTP Server", "ntpSrv", cfg.ntpServer) + "</div>";
  html += "<div>" + inputField("Timezone", "tz", cfg.timezone) + "</div>";
  html += F("</div></fieldset>");

  html += F("<button type='submit'>&#x1F4BE; Speichern &amp; Neustart</button>");
  html += F("</form>");

  // Factory reset
  html += F("<form method='POST' action='/config/reset'>"
    "<button class='btn-reset' onclick=\"return confirm('Alle Einstellungen zuruecksetzen?')\">"
    "&#x1F5D1; Factory Reset</button></form>");

  html += F("</body></html>");
  server.send(200, "text/html", html);
}

static void handleConfigSave(WebServer& server) {
  cfg.wifiSsid       = server.arg("wifiSsid");
  cfg.wifiPass       = server.arg("wifiPass");
  cfg.mqttHost       = server.arg("mqttHost");
  cfg.mqttPort       = server.arg("mqttPort").toInt();
  cfg.mqttUser       = server.arg("mqttUser");
  cfg.mqttPass       = server.arg("mqttPass");
  cfg.mqttClientId   = server.arg("mqttClId");
  cfg.mqttPrefix     = server.arg("mqttPfx");
  cfg.haHost         = server.arg("haHost");
  cfg.haPort         = server.arg("haPort").toInt();
  cfg.haToken        = server.arg("haToken");
  cfg.haCalendarEntity = server.arg("haCalEnt");
  cfg.haNextTitle    = server.arg("haNextTtl");
  cfg.haNextStart    = server.arg("haNextSt");
  cfg.roomName       = server.arg("roomName");
  cfg.companyName    = server.arg("company");
  cfg.httpIntervalMs = server.arg("httpIntS").toInt() * 1000UL;
  cfg.minDrawMs      = server.arg("minDrawS").toInt() * 1000UL;
  cfg.mqttSettleMs   = server.arg("settleS").toInt() * 1000UL;
  cfg.fullRefreshMs  = server.arg("fullRefS").toInt() * 1000UL;
  cfg.ntpServer      = server.arg("ntpSrv");
  cfg.timezone       = server.arg("tz");

  configSaveToNVS();

  server.send(200, "text/html",
    F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<style>body{background:#0f0f23;color:#0f0;font-family:sans-serif;"
      "display:flex;justify-content:center;align-items:center;height:100vh}"
      "</style></head><body>"
      "<div style='text-align:center'>"
      "<h2>&#x2705; Gespeichert!</h2>"
      "<p>Neustart in 3 Sekunden...</p>"
      "</div></body></html>"));

  delay(3000);
  ESP.restart();
}

static void handleConfigReset(WebServer& server) {
  prefs.begin(NVS_NS, false);
  prefs.clear();
  prefs.end();
  Serial.println(F("[CFG] NVS cleared — factory reset"));

  server.send(200, "text/html",
    F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<style>body{background:#0f0f23;color:#f44;font-family:sans-serif;"
      "display:flex;justify-content:center;align-items:center;height:100vh}"
      "</style></head><body>"
      "<div style='text-align:center'>"
      "<h2>&#x1F5D1; Reset!</h2>"
      "<p>Neustart mit config.h Defaults...</p>"
      "</div></body></html>"));

  delay(3000);
  ESP.restart();
}

// ── Captive Portal catch-all ─────────────────────────────────────
static void handleCaptiveRedirect(WebServer& server) {
  server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/config");
  server.send(302, "text/plain", "");
}

// ── Setup Routes ─────────────────────────────────────────────────
void configSetupRoutes(WebServer& server) {
  server.on("/config", HTTP_GET, [&server]() { handleConfigPage(server); });
  server.on("/config/save", HTTP_POST, [&server]() { handleConfigSave(server); });
  server.on("/config/reset", HTTP_POST, [&server]() { handleConfigReset(server); });

  if (apMode) {
    // Captive portal: redirect all unknown URLs to /config
    server.on("/generate_204", [&server]() { handleCaptiveRedirect(server); });  // Android
    server.on("/hotspot-detect.html", [&server]() { handleCaptiveRedirect(server); });  // Apple
    server.on("/connecttest.txt", [&server]() { handleCaptiveRedirect(server); });  // Windows
    server.on("/redirect", [&server]() { handleCaptiveRedirect(server); });
    server.on("/success.txt", [&server]() { server.send(200, "text/plain", "success"); });
    server.onNotFound([&server]() { handleCaptiveRedirect(server); });
  }
}
