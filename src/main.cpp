/**
 * Meetingroom E-Paper Display v4.0
 * Hardware: Elecrow CrowPanel 4.2" ESP32-S3, SSD1683, 400x300
 * SPI: Bit-Banging (Hardware SPI funktioniert nicht auf CrowPanel)
 *
 * v4.0: Web-Config Portal + WiFi AP Fallback (Captive Portal) + NVS Storage
 * v3.1: OTA Updates (ArduinoOTA + Web Upload)
 * v3:   Battery display, Deep Sleep, Spontan-Meeting via Buttons
 * Konfiguration: NVS (persistent) → config.h (Defaults/Fallback)
 * Logo: helo_logo.h (austauschbar)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <Update.h>
#include "EPD_SPI.h"
#include "EPD.h"
#include "EPD_GUI.h"
#include "config.h"
#include "config_portal.h"
#if SHOW_LOGO
#include "helo_logo.h"
#endif

typedef unsigned char UBYTE;

// ── Hardware Buttons (active LOW) ─────────────────────────────────
#define BTN_HOME  2
#define BTN_EXIT  1
#define BTN_PREV  6
#define BTN_NEXT  4
#define BTN_OK    5

// ── Battery ADC ──────────────────────────────────────────────────
// BAT connector routes through voltage divider to ADC
// ESP32-S3 ADC: 0-3.3V → 0-4095
// Voltage divider: factor ~2 (measure and calibrate!)
#ifndef BAT_ADC_PIN
#define BAT_ADC_PIN  9   // GPIO9 — adjust if your board differs
#endif
#ifndef BAT_VDIV_FACTOR
#define BAT_VDIV_FACTOR 2.0f  // Voltage divider ratio
#endif
#define BAT_FULL_V  4.2f
#define BAT_EMPTY_V 3.2f

// ── Deep Sleep ───────────────────────────────────────────────────
#ifndef SLEEP_BETWEEN_UPDATES
#define SLEEP_BETWEEN_UPDATES 0   // 0=disabled (always on), 1=deep sleep
#endif
#ifndef SLEEP_DURATION_MIN
#define SLEEP_DURATION_MIN 5      // Minutes between wake-ups
#endif

// ── Display ───────────────────────────────────────────────────────
#define EPD_WIDTH  400
#define EPD_HEIGHT 300
UBYTE BlackImage[15000];  // Statischer Framebuffer (400/8 * 300)

// ── MQTT Topics (aus Prefix) ─────────────────────────────────────
String topicOccupied;
String topicCurTitle;
String topicCurStart;
String topicCurEnd;
String topicNextTitle;
String topicNextStart;

// ── State ─────────────────────────────────────────────────────────
struct RoomState {
  bool   occupied  = false;
  String curTitle  = "";
  String curStart  = "";
  String curEnd    = "";
  String nextTitle = "";
  String nextStart = "";
  bool   dirty     = false;
  unsigned long dirtyAt = 0;
};

RoomState room;
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
WebServer    otaWeb(80);

unsigned long lastHttpFetch   = 0;
unsigned long lastDraw        = 0;
unsigned long lastFullRefresh = 0;

// ── Forward declarations ─────────────────────────────────────────
void drawDisplay();
void drawSpontanMenu();

// ── Spontan-Meeting State ────────────────────────────────────────
bool    spontanActive   = false;
time_t  spontanEnd      = 0;
bool    inSpontanMenu   = false;
int     spontanChoice   = 0;      // 0=15min, 1=30min, 2=60min, 3=cancel
const int spontanOptions[] = {15, 30, 60};
const int numSpontanOpts = 3;

// ── Helpers ───────────────────────────────────────────────────────

// Replace UTF-8 umlauts with ASCII equivalents for EPD font
String sanitize(const String& s) {
  String r = s;
  r.replace("\xC3\xA4", "ae");  // ä
  r.replace("\xC3\xB6", "oe");  // ö
  r.replace("\xC3\xBC", "ue");  // ü
  r.replace("\xC3\x84", "Ae");  // Ä
  r.replace("\xC3\x96", "Oe");  // Ö
  r.replace("\xC3\x9C", "Ue");  // Ü
  r.replace("\xC3\x9F", "ss");  // ß
  return r;
}

String extractTime(const String& dt) {
  int tPos = dt.indexOf('T');
  if (tPos >= 0 && dt.length() >= (unsigned)(tPos + 6))
    return dt.substring(tPos + 1, tPos + 6);
  int sPos = dt.indexOf(' ');
  if (sPos >= 0 && dt.length() >= (unsigned)(sPos + 6))
    return dt.substring(sPos + 1, sPos + 6);
  return dt.length() >= 5 ? dt.substring(0, 5) : dt;
}

String extractDate(const String& dt) {
  if (dt.length() >= 10)
    return dt.substring(8, 10) + "." + dt.substring(5, 7);
  return dt;
}

// ── Battery ──────────────────────────────────────────────────────
float readBatteryVoltage() {
  int raw = analogRead(BAT_ADC_PIN);
  float v = (raw / 4095.0f) * 3.3f * BAT_VDIV_FACTOR;
  return v;
}

int batteryPercent(float voltage) {
  if (voltage >= BAT_FULL_V) return 100;
  if (voltage <= BAT_EMPTY_V) return 0;
  return (int)(((voltage - BAT_EMPTY_V) / (BAT_FULL_V - BAT_EMPTY_V)) * 100.0f);
}

void drawBatteryIcon(int x, int y, int pct) {
  // Battery outline: 24x12 px
  EPD_DrawRectangle(x, y, x + 24, y + 12, BLACK, 0);
  EPD_DrawRectangle(x + 24, y + 3, x + 27, y + 9, BLACK, 1); // tip
  // Fill based on percent
  int fillW = (int)(20.0f * pct / 100.0f);
  if (fillW > 0) {
    EPD_DrawRectangle(x + 2, y + 2, x + 2 + fillW, y + 10, BLACK, 1);
  }
  // Percent text
  char buf[6];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  EPD_ShowString(x + 30, y, buf, 12, BLACK);
}

// ── WiFi (now uses config_portal) ─────────────────────────────────
void connectWifi() {
  configStartWiFi(15000);
}

// ── HTTP ──────────────────────────────────────────────────────────
String haGet(const String& path) {
  if (WiFi.status() != WL_CONNECTED || apMode) return "";
  HTTPClient http;
  http.begin("http://" + cfg.haHost + ":" + String(cfg.haPort) + path);
  http.addHeader("Authorization", String("Bearer ") + cfg.haToken);
  int code = http.GET();
  String body = (code == 200) ? http.getString() : "";
  http.end();
  return body;
}

void fetchFromHA() {
  Serial.println("Fetching HA...");
  bool changed = false;

  String body = haGet("/api/states/" + cfg.haCalendarEntity);
  if (body.length() > 0) {
    JsonDocument doc;
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
      JsonObject attrs = doc["attributes"].as<JsonObject>();
      bool   no = (doc["state"].as<String>() == "on");
      String nt = attrs["message"].as<String>();
      String ns = attrs["start_time"].as<String>();
      String ne = attrs["end_time"].as<String>();
      if (no != room.occupied || nt != room.curTitle ||
          ns != room.curStart  || ne != room.curEnd) {
        room.occupied = no; room.curTitle = nt;
        room.curStart = ns; room.curEnd   = ne;
        changed = true;
      }
    }
  }

  body = haGet("/api/states/" + cfg.haNextTitle);
  if (body.length() > 0) {
    JsonDocument doc;
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
      String v = doc["state"].as<String>();
      if (v != room.nextTitle) { room.nextTitle = v; changed = true; }
    }
  }

  body = haGet("/api/states/" + cfg.haNextStart);
  if (body.length() > 0) {
    JsonDocument doc;
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
      String v = doc["state"].as<String>();
      if (v != room.nextStart) { room.nextStart = v; changed = true; }
    }
  }

  if (changed) {
    Serial.println("HA data changed");
    room.dirtyAt = millis();
  }
}

// ── MQTT ──────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String t = String(topic);
  String v = String((char*)payload).substring(0, len);
  Serial.println("MQTT: " + t + " = " + v);

  bool newOcc = (v == "true");
  if      (t == topicOccupied  && newOcc != room.occupied)  { room.occupied  = newOcc; room.dirtyAt = millis(); }
  else if (t == topicCurTitle  && v != room.curTitle)       { room.curTitle  = v;      room.dirtyAt = millis(); }
  else if (t == topicCurStart  && v != room.curStart)       { room.curStart  = v;      room.dirtyAt = millis(); }
  else if (t == topicCurEnd    && v != room.curEnd)         { room.curEnd    = v;      room.dirtyAt = millis(); }
  else if (t == topicNextTitle && v != room.nextTitle)      { room.nextTitle = v;      room.dirtyAt = millis(); }
  else if (t == topicNextStart && v != room.nextStart)      { room.nextStart = v;      room.dirtyAt = millis(); }
}

void connectMqtt() {
  if (apMode) return;  // No MQTT in AP mode
  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
  Serial.print("MQTT...");
  if (mqtt.connect(cfg.mqttClientId.c_str(), cfg.mqttUser.c_str(), cfg.mqttPass.c_str())) {
    Serial.println(" OK");
    mqtt.subscribe(topicOccupied.c_str());
    mqtt.subscribe(topicCurTitle.c_str());
    mqtt.subscribe(topicCurStart.c_str());
    mqtt.subscribe(topicCurEnd.c_str());
    mqtt.subscribe(topicNextTitle.c_str());
    mqtt.subscribe(topicNextStart.c_str());
  } else {
    Serial.println(" FAIL: " + String(mqtt.state()));
  }
}

// ── Meeting Progress ──────────────────────────────────────────────
time_t parseDateTime(const String& dt) {
  struct tm tm = {};
  if (dt.length() < 16) return 0;
  tm.tm_year = dt.substring(0, 4).toInt() - 1900;
  tm.tm_mon  = dt.substring(5, 7).toInt() - 1;
  tm.tm_mday = dt.substring(8, 10).toInt();
  tm.tm_hour = dt.substring(11, 13).toInt();
  tm.tm_min  = dt.substring(14, 16).toInt();
  if (dt.length() >= 19) tm.tm_sec = dt.substring(17, 19).toInt();
  tm.tm_isdst = -1;
  return mktime(&tm);
}

// ── Spontan-Meeting ──────────────────────────────────────────────
void drawSpontanMenu() {
  EPD_Init_Fast(Fast_Seconds_1_5s);
  Paint_NewImage(BlackImage, EPD_WIDTH, EPD_HEIGHT, ROTATE_0, WHITE);
  EPD_Full(WHITE);

  EPD_DrawRectangle(0, 0, 399, 49, BLACK, 1);
  EPD_ShowString(80, 14, "Spontanes Meeting", 24, WHITE);

  int y = 70;
  EPD_ShowString(30, y, "Dauer waehlen:", 24, BLACK);
  y += 40;

  for (int i = 0; i < numSpontanOpts; i++) {
    char label[20];
    snprintf(label, sizeof(label), "%d Minuten", spontanOptions[i]);
    if (i == spontanChoice) {
      EPD_DrawRectangle(20, y - 4, 380, y + 28, BLACK, 1);
      EPD_ShowString(40, y, label, 24, WHITE);
    } else {
      EPD_DrawRectangle(20, y - 4, 380, y + 28, BLACK, 0);
      EPD_ShowString(40, y, label, 24, BLACK);
    }
    y += 40;
  }

  y += 10;
  EPD_ShowString(20, y, "[Hoch/Runter] Waehlen", 16, BLACK);
  EPD_ShowString(20, y + 20, "[OK] Bestaetigen  [Exit] Abbrechen", 16, BLACK);

  EPD_Display_Fast(BlackImage);
  EPD_Sleep();
}

void startSpontanMeeting(int minutes) {
  struct tm tnow;
  if (!getLocalTime(&tnow)) return;

  time_t now = mktime(&tnow);
  spontanEnd = now + (minutes * 60);
  spontanActive = true;

  // Build time strings
  struct tm tmEnd;
  localtime_r(&spontanEnd, &tmEnd);

  char startStr[25], endStr[25];
  strftime(startStr, sizeof(startStr), "%Y-%m-%d %H:%M:%S", &tnow);
  strftime(endStr, sizeof(endStr), "%Y-%m-%d %H:%M:%S", &tmEnd);

  // Override room state
  room.occupied = true;
  room.curTitle = "Spontanes Meeting";
  room.curStart = String(startStr);
  room.curEnd   = String(endStr);

  // Publish via MQTT so HA knows
  mqtt.publish(topicOccupied.c_str(), "true", true);
  mqtt.publish(topicCurTitle.c_str(), "Spontanes Meeting", true);
  mqtt.publish(topicCurStart.c_str(), startStr, true);
  mqtt.publish(topicCurEnd.c_str(), endStr, true);

  Serial.printf("Spontan Meeting: %d min, bis %s\n", minutes, endStr);
}

void checkSpontanExpiry() {
  if (!spontanActive) return;
  struct tm tnow;
  if (!getLocalTime(&tnow)) return;
  time_t now = mktime(&tnow);
  if (now >= spontanEnd) {
    spontanActive = false;
    room.occupied = false;
    room.curTitle = "";
    room.curStart = "";
    room.curEnd   = "";
    mqtt.publish(topicOccupied.c_str(), "false", true);
    mqtt.publish(topicCurTitle.c_str(), "", true);
    mqtt.publish(topicCurStart.c_str(), "", true);
    mqtt.publish(topicCurEnd.c_str(), "", true);
    room.dirty = true;
    Serial.println("Spontan Meeting ended");
  }
}

// ── Buttons ──────────────────────────────────────────────────────
void handleButtons() {
  // Check for HOME button → open/close spontan menu
  if (digitalRead(BTN_HOME) == LOW) {
    delay(200);  // debounce
    if (digitalRead(BTN_HOME) == LOW) {
      if (!inSpontanMenu && !room.occupied) {
        inSpontanMenu = true;
        spontanChoice = 0;
        drawSpontanMenu();
      } else if (inSpontanMenu) {
        inSpontanMenu = false;
        drawDisplay();
      }
      while (digitalRead(BTN_HOME) == LOW) delay(50);  // wait release
    }
  }

  if (!inSpontanMenu) return;

  // PREV (Up)
  if (digitalRead(BTN_PREV) == LOW) {
    delay(200);
    if (spontanChoice > 0) spontanChoice--;
    drawSpontanMenu();
    while (digitalRead(BTN_PREV) == LOW) delay(50);
  }

  // NEXT (Down)
  if (digitalRead(BTN_NEXT) == LOW) {
    delay(200);
    if (spontanChoice < numSpontanOpts - 1) spontanChoice++;
    drawSpontanMenu();
    while (digitalRead(BTN_NEXT) == LOW) delay(50);
  }

  // OK → start meeting
  if (digitalRead(BTN_OK) == LOW) {
    delay(200);
    startSpontanMeeting(spontanOptions[spontanChoice]);
    inSpontanMenu = false;
    drawDisplay();
    while (digitalRead(BTN_OK) == LOW) delay(50);
  }

  // EXIT → cancel
  if (digitalRead(BTN_EXIT) == LOW) {
    delay(200);
    inSpontanMenu = false;
    drawDisplay();
    while (digitalRead(BTN_EXIT) == LOW) delay(50);
  }
}


// ── Draw ──────────────────────────────────────────────────────────
void drawDisplay() {
  Serial.println("Drawing...");
  EPD_Init_Fast(Fast_Seconds_1_5s);
  Paint_NewImage(BlackImage, EPD_WIDTH, EPD_HEIGHT, ROTATE_0, WHITE);
  EPD_Full(WHITE);

  // ── Header ────────────────────────────────────────────────────
  EPD_DrawRectangle(0, 0, 399, 49, BLACK, 1);

#if SHOW_LOGO
  EPD_ShowPicture(8, 7, LOGO_W, LOGO_H, helo_logo, WHITE);
#else
  EPD_ShowString(10, 12, cfg.roomName.c_str(), 24, WHITE);
#endif

  struct tm ti;
  if (getLocalTime(&ti)) {
    char tbuf[10], dbuf[12];
    strftime(tbuf, sizeof(tbuf), "%H:%M", &ti);
    strftime(dbuf, sizeof(dbuf), "%d.%m.%Y", &ti);
    EPD_ShowString(310, 6,  tbuf, 24, WHITE);
    EPD_ShowString(310, 32, dbuf, 12, WHITE);
  }

  // ── Status ────────────────────────────────────────────────────
  int y = 58;
  if (room.occupied) {
    EPD_DrawRectangle(0, y, 399, y + 44, BLACK, 1);
    EPD_ShowString(164, y + 10, "BELEGT", 24, WHITE);
    y += 52;

    String title = room.curTitle;
    if (title.length() > 34) title = title.substring(0, 31) + "...";
    EPD_ShowString(12, y, sanitize(title).c_str(), 24, BLACK);
    y += 28;

    if (room.curStart.length() > 0) {
      String zeitraum = extractTime(room.curStart) + " - " + extractTime(room.curEnd) + " Uhr";
      EPD_ShowString(12, y, sanitize(zeitraum).c_str(), 16, BLACK);
      y += 24;
    }

    // ── Fortschrittsbalken ──────────────────────────────────────
    int barX = 12;
    int barW = 376;
    int barH = 14;
    EPD_DrawRectangle(barX, y, barX + barW, y + barH, BLACK, 0);

    time_t tStart = parseDateTime(room.curStart);
    time_t tEnd   = parseDateTime(room.curEnd);
    struct tm tnow;
    if (getLocalTime(&tnow) && tStart > 0 && tEnd > tStart) {
      time_t tNow = mktime(&tnow);
      float progress = (float)(tNow - tStart) / (float)(tEnd - tStart);
      if (progress < 0.0f) progress = 0.0f;
      if (progress > 1.0f) progress = 1.0f;
      int fillW = (int)(progress * (barW - 4));
      if (fillW > 0) {
        EPD_DrawRectangle(barX + 2, y + 2, barX + 2 + fillW, y + barH - 2, BLACK, 1);
      }
      char pctBuf[6];
      snprintf(pctBuf, sizeof(pctBuf), "%d%%", (int)(progress * 100));
      int remaining = (int)((tEnd - tNow) / 60);
      if (remaining > 0) {
        char remBuf[20];
        snprintf(remBuf, sizeof(remBuf), "noch %d Min", remaining);
        EPD_ShowString(barX, y + barH + 3, remBuf, 12, BLACK);
      }
      EPD_ShowString(barX + barW - 35, y + barH + 3, pctBuf, 12, BLACK);
    }
    y += barH + 18;
  } else {
    EPD_DrawRectangle(4, y, 395, y + 60, BLACK, 0);
    EPD_DrawRectangle(6, y+2, 393, y+58, BLACK, 0);
    EPD_ShowString(176, y + 16, "FREI", 24, BLACK);
    y += 70;
  }

  // ── Trennlinie ────────────────────────────────────────────────
  y += 4;
  EPD_DrawLine(10, y, 389, y, BLACK);
  y += 10;

  // ── Naechstes Meeting ─────────────────────────────────────────
  EPD_ShowString(12, y, "Naechstes Meeting", 16, BLACK);
  EPD_DrawLine(12, y + 18, 180, y + 18, BLACK);
  y += 26;

  if (room.nextTitle.length() > 0 &&
      room.nextTitle != "-" &&
      room.nextTitle != "Keine Termine") {
    String nt = room.nextTitle;
    if (nt.length() > 34) nt = nt.substring(0, 31) + "...";
    EPD_ShowString(12, y, sanitize(nt).c_str(), 16, BLACK);
    y += 22;
    if (room.nextStart.length() >= 16) {
      String info = extractTime(room.nextStart) + " Uhr  |  " + extractDate(room.nextStart);
      EPD_ShowString(12, y, sanitize(info).c_str(), 16, BLACK);
    }
  } else {
    EPD_ShowString(12, y, "Keine weiteren Termine", 16, BLACK);
  }

  // ── Footer ────────────────────────────────────────────────────
  EPD_DrawLine(0, 284, 399, 284, BLACK);
  struct tm tf;
  if (getLocalTime(&tf)) {
    char sbuf[30];
    strftime(sbuf, sizeof(sbuf), "Aktualisiert: %H:%M", &tf);
    EPD_ShowString(8, 287, sbuf, 12, BLACK);
  }

  // Battery icon in footer (before IP)
  float batV = readBatteryVoltage();
  int batPct = batteryPercent(batV);
  drawBatteryIcon(140, 287, batPct);

  // IP address for OTA access
  String ip = WiFi.localIP().toString();
  EPD_ShowString(180, 287, ip.c_str(), 12, BLACK);

  EPD_ShowString(300, 287, cfg.companyName.c_str(), 12, BLACK);

  // ── Hint: Home button for spontan meeting ─────────────────────
  if (!room.occupied) {
    EPD_ShowString(110, 268, "[Home] Spontanes Meeting", 12, BLACK);
  }

  // ── Refresh ───────────────────────────────────────────────────
  EPD_Display_Fast(BlackImage);
  EPD_Sleep();

  room.dirty   = false;
  room.dirtyAt = 0;
  lastDraw        = millis();
  lastFullRefresh = millis();
  Serial.println("Draw done.");
}


// ── Deep Sleep ───────────────────────────────────────────────────
#if SLEEP_BETWEEN_UPDATES
void enterDeepSleep() {
  Serial.printf("Deep sleep for %d min...\n", SLEEP_DURATION_MIN);
  // Wake on HOME button (GPIO2) or timer
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_HOME, 0);  // LOW = pressed
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_DURATION_MIN * 60ULL * 1000000ULL);
  // Power off display
  digitalWrite(7, LOW);
  WiFi.disconnect(true);
  esp_deep_sleep_start();
}
#endif

// ── OTA Web Upload ───────────────────────────────────────────────
const char OTA_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Meetingroom Display OTA</title>
<style>
body{font-family:sans-serif;max-width:500px;margin:40px auto;padding:0 20px;background:#1a1a2e;color:#e0e0e0}
h1{color:#00d4ff}
.box{background:#16213e;padding:20px;border-radius:8px;margin:20px 0}
input[type=file]{margin:10px 0}
button{background:#00d4ff;color:#000;border:none;padding:10px 24px;border-radius:4px;cursor:pointer;font-size:16px}
button:hover{background:#00a8cc}
#prog{width:100%;height:20px;margin:10px 0;display:none}
#status{margin:10px 0;font-weight:bold}
.info{color:#888;font-size:13px}
</style></head><body>
<h1>&#x1f4e1; OTA Update</h1>
<div class="box">
<p>Firmware (.bin) hochladen:</p>
<form id="f" method="POST" action="/update" enctype="multipart/form-data">
<input type="file" name="firmware" accept=".bin" required><br>
<button type="submit">Firmware flashen</button>
</form>
<progress id="prog" value="0" max="100"></progress>
<div id="status"></div>
</div>
<div class="box info">
<p><strong>Hostname:</strong> meetingroom-display.local</p>
<p><strong>Version:</strong> v4.0</p>
<p><strong>IP:</strong> <span id="ip">-</span></p>
</div>
<script>
document.getElementById('ip').textContent=location.hostname;
document.getElementById('f').addEventListener('submit',function(e){
  e.preventDefault();
  var f=new FormData(this);
  var x=new XMLHttpRequest();
  x.open('POST','/update');
  var p=document.getElementById('prog');
  var s=document.getElementById('status');
  p.style.display='block';
  x.upload.onprogress=function(e){if(e.lengthComputable){p.value=Math.round(e.loaded/e.total*100);}};
  x.onload=function(){s.textContent=x.status==200?'OK! Neustart...':'Fehler: '+x.responseText;
    if(x.status==200)setTimeout(function(){location.reload()},10000);};
  x.onerror=function(){s.textContent='Upload fehlgeschlagen';};
  s.textContent='Uploading...';
  x.send(f);
});
</script></body></html>
)rawliteral";

bool otaInProgress = false;

void setupOTA() {
  // ── ArduinoOTA (PlatformIO / espota) ──────────────────────────
  ArduinoOTA.setHostname("meetingroom-display");
  ArduinoOTA.setPassword(cfg.wifiPass.c_str());  // Use WiFi password for OTA auth
  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    Serial.println("OTA Start...");
    // Show update screen
    EPD_Init_Fast(Fast_Seconds_1_5s);
    Paint_NewImage(BlackImage, EPD_WIDTH, EPD_HEIGHT, ROTATE_0, WHITE);
    EPD_Full(WHITE);
    EPD_ShowString(100, 130, "OTA Update...", 24, BLACK);
    EPD_Display_Fast(BlackImage);
    EPD_Sleep();
  });
  ArduinoOTA.onEnd([]() {
    otaInProgress = false;
    Serial.println("OTA Done!");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA: %u%%\r", (progress * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    Serial.printf("OTA Error[%u]\n", error);
  });
  ArduinoOTA.begin();

  // ── Web OTA (Browser Upload) ──────────────────────────────────
  otaWeb.on("/", HTTP_GET, []() {
    otaWeb.send_P(200, "text/html", OTA_PAGE);
  });

  otaWeb.on("/update", HTTP_POST, []() {
    otaWeb.sendHeader("Connection", "close");
    otaWeb.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
    delay(500);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = otaWeb.upload();
    if (upload.status == UPLOAD_FILE_START) {
      otaInProgress = true;
      Serial.printf("Web OTA: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("Web OTA OK: %u bytes\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
      otaInProgress = false;
    }
  });

  otaWeb.on("/info", HTTP_GET, []() {
    String json = "{\"version\":\"v4.0\",\"ip\":\"" + WiFi.localIP().toString() +
                  "\",\"hostname\":\"meetingroom-display\",\"uptime\":" + String(millis()/1000) +
                  ",\"rssi\":" + String(WiFi.RSSI()) + "}";
    otaWeb.send(200, "application/json", json);
  });

  otaWeb.begin();
  Serial.println("OTA ready: http://" + WiFi.localIP().toString() + " / meetingroom-display.local");
}

// ── Clock Update ─────────────────────────────────────────────────
unsigned long lastClockUpdate = 0;
char lastTimeStr[6] = "";

// ── Setup ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Meetingroom Display v4.0 ===");

  // 1. Load config from NVS (or config.h defaults)
  configLoadFromNVS();

  // MQTT Topics aus Prefix bauen
  topicOccupied  = cfg.mqttPrefix + "/occupied";
  topicCurTitle  = cfg.mqttPrefix + "/current/title";
  topicCurStart  = cfg.mqttPrefix + "/current/start";
  topicCurEnd    = cfg.mqttPrefix + "/current/end";
  topicNextTitle = cfg.mqttPrefix + "/next/title";
  topicNextStart = cfg.mqttPrefix + "/next/start";

  // Display Power
  pinMode(7, OUTPUT);
  digitalWrite(7, HIGH);

  // Buttons (active LOW with internal pullup)
  pinMode(BTN_HOME, INPUT_PULLUP);
  pinMode(BTN_EXIT, INPUT_PULLUP);
  pinMode(BTN_PREV, INPUT_PULLUP);
  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_OK,   INPUT_PULLUP);

  // Battery ADC
  analogReadResolution(12);
  pinMode(BAT_ADC_PIN, INPUT);

  // EPD Init + Splash
  EPD_GPIOInit();
  EPD_Clear();
  Paint_NewImage(BlackImage, EPD_WIDTH, EPD_HEIGHT, ROTATE_0, WHITE);
  EPD_Full(WHITE);
  EPD_ShowString(20, 130, "Verbinde mit WiFi...", 24, BLACK);
  EPD_Display_Part(0, 0, EPD_WIDTH, EPD_HEIGHT, BlackImage);
  EPD_Sleep();

  // 2. WiFi: STA first, AP fallback
  if (!configStartWiFi(15000)) {
    // WiFi failed → AP mode with captive portal
    configStartAP();

    // Show AP info on display
    EPD_Init_Fast(Fast_Seconds_1_5s);
    Paint_NewImage(BlackImage, EPD_WIDTH, EPD_HEIGHT, ROTATE_0, WHITE);
    EPD_Full(WHITE);
    EPD_DrawRectangle(0, 0, 399, 49, BLACK, 1);
    EPD_ShowString(100, 14, "WiFi Setup", 24, WHITE);
    EPD_ShowString(20, 70, "Kein WiFi gefunden!", 24, BLACK);
    EPD_ShowString(20, 110, "Verbinde dich mit:", 16, BLACK);
    EPD_ShowString(20, 135, "SSID: Meetingroom-Setup", 24, BLACK);
    EPD_ShowString(20, 175, "Dann oeffne:", 16, BLACK);
    String url = "http://" + WiFi.softAPIP().toString() + "/config";
    EPD_ShowString(20, 200, url.c_str(), 16, BLACK);
    EPD_ShowString(20, 240, "oder warte auf Captive Portal", 12, BLACK);
    EPD_Display_Fast(BlackImage);
    EPD_Sleep();
  }

  // 3. WebServer: Config routes + OTA (works in both modes)
  configSetupRoutes(otaWeb);

  if (!apMode) {
    // Normal mode: NTP + HA + MQTT + OTA
    configTime(0, 0, cfg.ntpServer.c_str());
    setenv("TZ", cfg.timezone.c_str(), 1);
    tzset();
    delay(1500);

    fetchFromHA();
    connectMqtt();

    // Warten auf MQTT retained Messages
    unsigned long t = millis();
    while (millis() - t < 3000) {
      mqtt.loop();
      delay(50);
    }

    setupOTA();
    drawDisplay();
    Serial.println("Setup done. Normal mode.");
  } else {
    // AP mode: only start web server (OTA setup needs WiFi STA)
    otaWeb.begin();
    Serial.println("Setup done. AP config mode.");
  }
}

// ── Loop ──────────────────────────────────────────────────────────
void loop() {
#if SLEEP_BETWEEN_UPDATES
  // In deep sleep mode: fetch, draw, sleep
  enterDeepSleep();
#endif

  // DNS for captive portal
  configLoopDNS();

  // Web server (always active — config portal + OTA)
  otaWeb.handleClient();

  // AP mode: only serve config portal
  if (apMode) {
    delay(10);
    return;
  }

  // OTA handlers
  ArduinoOTA.handle();
  if (otaInProgress) return;  // Don't do anything else during OTA

  if (WiFi.status() != WL_CONNECTED) connectWifi();
  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();

  unsigned long now = millis();

  // Button handling
  handleButtons();

  // Check spontan meeting expiry
  checkSpontanExpiry();

  // Settling
  if (room.dirtyAt > 0 && (now - room.dirtyAt > cfg.mqttSettleMs)) {
    room.dirty   = true;
    room.dirtyAt = 0;
  }

  // HTTP Polling
  if (now - lastHttpFetch > cfg.httpIntervalMs) {
    fetchFromHA();
    lastHttpFetch = now;
  }

  // Anti-Ghosting
  if (now - lastFullRefresh > cfg.fullRefreshMs) {
    room.dirty = true;
  }

  // Draw
  if (room.dirty && (now - lastDraw > cfg.minDrawMs)) {
    drawDisplay();
  }

  // Uhrzeit jede Minute aktualisieren
  if (now - lastClockUpdate > 60000UL) {
    struct tm tc;
    if (getLocalTime(&tc)) {
      char tbuf[6];
      strftime(tbuf, sizeof(tbuf), "%H:%M", &tc);
      if (strcmp(tbuf, lastTimeStr) != 0) {
        strcpy(lastTimeStr, tbuf);
        Serial.println("Clock changed: " + String(tbuf));
        drawDisplay();
      }
    }
    lastClockUpdate = now;
  }

  delay(100);  // 100ms statt 200ms für bessere Button-Reaktion
}
