/**
 * Meetingroom E-Paper Display v2
 * Hardware: Elecrow CrowPanel 4.2" ESP32-S3, SSD1683, 400x300
 * SPI: Bit-Banging (Hardware SPI funktioniert nicht auf CrowPanel)
 *
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

// ── Helpers ───────────────────────────────────────────────────────
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
    EPD_ShowString(298, 32, dbuf, 12, WHITE);
  }

  // ── Status ────────────────────────────────────────────────────
  int y = 58;
  if (room.occupied) {
    EPD_DrawRectangle(0, y, 399, y + 44, BLACK, 1);
    EPD_ShowString(140, y + 10, "BELEGT", 24, WHITE);
    y += 52;

    String title = room.curTitle;
    if (title.length() > 34) title = title.substring(0, 31) + "...";
    EPD_ShowString(12, y, title.c_str(), 24, BLACK);
    y += 28;

    if (room.curStart.length() > 0) {
      String zeitraum = extractTime(room.curStart) + " - " + extractTime(room.curEnd) + " Uhr";
      EPD_ShowString(12, y, zeitraum.c_str(), 16, BLACK);
      y += 24;
    }
  } else {
    EPD_DrawRectangle(4, y, 395, y + 60, BLACK, 0);
    EPD_DrawRectangle(6, y+2, 393, y+58, BLACK, 0);
    EPD_ShowString(155, y + 16, "FREI", 24, BLACK);
    y += 70;
  }

  // ── Trennlinie ────────────────────────────────────────────────
  y += 4;
  EPD_DrawLine(10, y, 389, y, BLACK);
  EPD_DrawLine(10, y+1, 389, y+1, BLACK);
  y += 12;

  // ── Nächstes Meeting ──────────────────────────────────────────
  EPD_ShowString(12, y, "Naechstes Meeting", 16, BLACK);
  EPD_DrawLine(12, y + 18, 180, y + 18, BLACK);
  y += 26;

  if (room.nextTitle.length() > 0 &&
      room.nextTitle != "-" &&
      room.nextTitle != "Keine Termine") {
    String nt = room.nextTitle;
    if (nt.length() > 34) nt = nt.substring(0, 31) + "...";
    EPD_ShowString(12, y, nt.c_str(), 16, BLACK);
    y += 22;
    if (room.nextStart.length() >= 16) {
      String info = extractTime(room.nextStart) + " Uhr  |  " + extractDate(room.nextStart);
      EPD_ShowString(12, y, info.c_str(), 16, BLACK);
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
  EPD_ShowString(300, 287, COMPANY_NAME, 12, BLACK);

  // ── Refresh ───────────────────────────────────────────────────
  EPD_Display_Fast(BlackImage);
  EPD_Sleep();

  room.dirty   = false;
  room.dirtyAt = 0;
  lastDraw        = millis();
  lastFullRefresh = millis();
  Serial.println("Draw done.");
}

// ── Setup ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Meetingroom Display v2 ===");

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
  if (WiFi.status() != WL_CONNECTED) connectWifi();
  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();

  unsigned long now = millis();

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

  delay(200);
}
