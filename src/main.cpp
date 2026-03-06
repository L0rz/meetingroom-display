/**
 * Meetingroom E-Paper Display v3
 * Hardware: Elecrow CrowPanel 4.2" ESP32-S3, SSD1683, 400x300
 * SPI: Bit-Banging (Hardware SPI funktioniert nicht auf CrowPanel)
 *
 * v3: Battery display, Deep Sleep, Spontan-Meeting via Buttons
 * Konfiguration: config.h (WiFi, MQTT, HA, Timing, Logo)
 * Logo: helo_logo.h (austauschbar)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include "EPD_SPI.h"
#include "EPD.h"
#include "EPD_GUI.h"
#include "config.h"
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

// ── WiFi ──────────────────────────────────────────────────────────
void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi...");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500); Serial.print("."); tries++;
  }
  Serial.println(WiFi.status() == WL_CONNECTED
    ? " OK: " + WiFi.localIP().toString() : " FAIL");
}

// ── HTTP ──────────────────────────────────────────────────────────
String haGet(const String& path) {
  if (WiFi.status() != WL_CONNECTED) return "";
  HTTPClient http;
  http.begin("http://" + String(HA_HOST) + ":" + String(HA_PORT) + path);
  http.addHeader("Authorization", String("Bearer ") + HA_TOKEN);
  int code = http.GET();
  String body = (code == 200) ? http.getString() : "";
  http.end();
  return body;
}

void fetchFromHA() {
  Serial.println("Fetching HA...");
  bool changed = false;

  String body = haGet("/api/states/" + String(HA_CALENDAR_ENTITY));
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

  body = haGet("/api/states/" + String(HA_NEXT_TITLE));
  if (body.length() > 0) {
    JsonDocument doc;
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
      String v = doc["state"].as<String>();
      if (v != room.nextTitle) { room.nextTitle = v; changed = true; }
    }
  }

  body = haGet("/api/states/" + String(HA_NEXT_START));
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
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
  Serial.print("MQTT...");
  if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
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
  EPD_ShowString(10, 12, ROOM_NAME, 24, WHITE);
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

  // Battery icon in footer
  float batV = readBatteryVoltage();
  int batPct = batteryPercent(batV);
  drawBatteryIcon(220, 287, batPct);

  EPD_ShowString(300, 287, COMPANY_NAME, 12, BLACK);

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

// ── Clock Update ─────────────────────────────────────────────────
unsigned long lastClockUpdate = 0;
char lastTimeStr[6] = "";

// ── Setup ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Meetingroom Display v3 ===");

  // MQTT Topics aus Prefix bauen
  String prefix = String(MQTT_PREFIX);
  topicOccupied  = prefix + "/occupied";
  topicCurTitle  = prefix + "/current/title";
  topicCurStart  = prefix + "/current/start";
  topicCurEnd    = prefix + "/current/end";
  topicNextTitle = prefix + "/next/title";
  topicNextStart = prefix + "/next/start";

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

  // WiFi + NTP
  connectWifi();
  configTime(0, 0, NTP_SERVER);
  setenv("TZ", TIMEZONE, 1);
  tzset();
  delay(1500);

  // Daten + MQTT
  fetchFromHA();
  connectMqtt();

  // Warten auf MQTT retained Messages
  unsigned long t = millis();
  while (millis() - t < 3000) {
    mqtt.loop();
    delay(50);
  }

  drawDisplay();
  Serial.println("Setup done.");
}

// ── Loop ──────────────────────────────────────────────────────────
void loop() {
#if SLEEP_BETWEEN_UPDATES
  // In deep sleep mode: fetch, draw, sleep
  enterDeepSleep();
#endif

  if (WiFi.status() != WL_CONNECTED) connectWifi();
  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();

  unsigned long now = millis();

  // Button handling
  handleButtons();

  // Check spontan meeting expiry
  checkSpontanExpiry();

  // Settling
  if (room.dirtyAt > 0 && (now - room.dirtyAt > MQTT_SETTLE_MS)) {
    room.dirty   = true;
    room.dirtyAt = 0;
  }

  // HTTP Polling
  if (now - lastHttpFetch > HTTP_INTERVAL_MS) {
    fetchFromHA();
    lastHttpFetch = now;
  }

  // Anti-Ghosting
  if (now - lastFullRefresh > FULL_REFRESH_MS) {
    room.dirty = true;
  }

  // Draw
  if (room.dirty && (now - lastDraw > MIN_DRAW_MS)) {
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
