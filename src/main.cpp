/**
 * Meetingraum-Display
 * Elecrow CrowPanel 4.2" E-Paper (SSD1683, 400x300px)
 * ESP32-S3 + Home Assistant REST API
 *
 * Zeigt:
 *  - FREI / BELEGT Status (groß)
 *  - Aktuelles Meeting: Titel + Uhrzeit
 *  - Nächstes Meeting: Titel + Startzeit
 *  - Raumname + aktuelle Uhrzeit/Datum
 *
 * Update-Intervall: 5 Minuten
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <time.h>

// GxEPD2 Display Library
#include <GxEPD2_BW.h>
// Direkt den SSD1683 4.2" Treiber einbinden
#include <epd/GxEPD2_420_GDEY042T81.h>

// Fonts (Adafruit GFX)
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>

#include "secrets.h"

// ─────────────────────────────────────────────
// Hardware-Pins (ESP32-S3)
// ─────────────────────────────────────────────
#define EPD_CLK   12
#define EPD_MOSI  11
#define EPD_CS    45
#define EPD_DC    46
#define EPD_RST   47
#define EPD_BUSY  48

// ─────────────────────────────────────────────
// Home Assistant Konfiguration
// ─────────────────────────────────────────────
#define HA_HOST         "http://192.168.130.3:8123"
#define HA_TOKEN        "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiIzMTc2NGM5ODhkNjE0MDdmODAzMjVhNmVlMTM0ZDY4YiIsImlhdCI6MTc3MjQ2NTkwMCwiZXhwIjoyMDg3ODI1OTAwfQ.qiD_fzX867luKzvReK6quV5BYK4-rvmnw_B8SVNeEHw"

#define HA_ENTITY_CAL   "/api/states/calendar.test2"
#define HA_ENTITY_NEXT_TITLE "/api/states/input_text.meetingraum_next_title"
#define HA_ENTITY_NEXT_START "/api/states/input_text.meetingraum_next_start"

// ─────────────────────────────────────────────
// App-Konfiguration
// ─────────────────────────────────────────────
#define ROOM_NAME           "Meetingraum"
#define UPDATE_INTERVAL_MS  (5UL * 60UL * 1000UL)   // 5 Minuten

// NTP
#define NTP_SERVER      "pool.ntp.org"
#define TZ_OFFSET_SEC   3600    // UTC+1 (Winter); für Sommer: 7200
#define DST_OFFSET_SEC  3600    // 1h DST

// ─────────────────────────────────────────────
// Display-Setup
// ─────────────────────────────────────────────
// SSD1683, 400x300, 4.2"
// Klasse: GxEPD2_420_GDEY042T81
// Falls dein Display einen anderen Controller hat, hier anpassen.
// Weitere Klassen: GxEPD2_420, GxEPD2_420_GYE042A87, etc.
SPIClass epd_spi(HSPI);

GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> display(
    GxEPD2_420_GDEY042T81(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

// ─────────────────────────────────────────────
// Datenstrturen
// ─────────────────────────────────────────────
struct MeetingInfo {
    bool    active    = false;
    String  title     = "";
    String  startTime = "";
    String  endTime   = "";
};

struct NextMeetingInfo {
    String  title     = "";
    String  startTime = "";
    bool    valid     = false;
};

// ─────────────────────────────────────────────
// Hilfsfunktionen
// ─────────────────────────────────────────────

/**
 * Extrahiert "HH:MM" aus ISO-8601 String
 * z.B. "2024-01-15T14:30:00+01:00" → "14:30"
 */
String extractTime(const String& iso) {
    // Format: YYYY-MM-DDTHH:MM:SS...
    int tIdx = iso.indexOf('T');
    if (tIdx < 0) return iso.substring(0, 5);  // Fallback
    String timePart = iso.substring(tIdx + 1);
    return timePart.substring(0, 5);  // HH:MM
}

/**
 * Extrahiert "TT.MM.JJJJ" aus ISO-8601 String
 */
String extractDate(const String& iso) {
    if (iso.length() < 10) return iso;
    // YYYY-MM-DD → TT.MM.JJJJ
    String year  = iso.substring(0, 4);
    String month = iso.substring(5, 7);
    String day   = iso.substring(8, 10);
    return day + "." + month + "." + year;
}

/**
 * Gibt den aktuellen Wochentag auf Deutsch zurück
 */
String weekdayDE(int wday) {
    const char* days[] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
    return String(days[wday % 7]);
}

/**
 * Gibt aktuelles Datum/Zeit als String zurück
 */
String getCurrentDateTime() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return "--:-- --.--.----";
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %02d.%02d.%04d  %02d:%02d",
        weekdayDE(timeinfo.tm_wday).c_str(),
        timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900,
        timeinfo.tm_hour, timeinfo.tm_min
    );
    return String(buf);
}

// ─────────────────────────────────────────────
// HTTP-Hilfsfunktion
// ─────────────────────────────────────────────
String httpGetHA(const String& path) {
    HTTPClient http;
    String url = String(HA_HOST) + path;
    http.begin(url);
    http.addHeader("Authorization", String("Bearer ") + HA_TOKEN);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(10000);

    int code = http.GET();
    String payload = "";
    if (code == 200) {
        payload = http.getString();
    } else {
        Serial.printf("[HTTP] Fehler %d bei %s\n", code, path.c_str());
    }
    http.end();
    return payload;
}

// ─────────────────────────────────────────────
// HA-Daten abrufen
// ─────────────────────────────────────────────
MeetingInfo fetchCurrentMeeting() {
    MeetingInfo info;
    String json = httpGetHA(HA_ENTITY_CAL);
    if (json.isEmpty()) return info;

    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        Serial.println("[JSON] Parse-Fehler (calendar)");
        return info;
    }

    const char* state = doc["state"];
    info.active = (state && strcmp(state, "on") == 0);

    if (info.active) {
        auto attrs = doc["attributes"];
        info.title     = attrs["message"]    | "";
        info.startTime = attrs["start_time"] | "";
        info.endTime   = attrs["end_time"]   | "";
    }

    return info;
}

NextMeetingInfo fetchNextMeeting() {
    NextMeetingInfo next;

    // Titel
    String jsonTitle = httpGetHA(HA_ENTITY_NEXT_TITLE);
    if (!jsonTitle.isEmpty()) {
        JsonDocument doc;
        if (deserializeJson(doc, jsonTitle) == DeserializationError::Ok) {
            const char* s = doc["state"];
            if (s && strlen(s) > 0 && strcmp(s, "unknown") != 0) {
                next.title = String(s);
                next.valid = true;
            }
        }
    }

    // Startzeit
    String jsonStart = httpGetHA(HA_ENTITY_NEXT_START);
    if (!jsonStart.isEmpty()) {
        JsonDocument doc;
        if (deserializeJson(doc, jsonStart) == DeserializationError::Ok) {
            const char* s = doc["state"];
            if (s && strlen(s) > 0 && strcmp(s, "unknown") != 0) {
                next.startTime = String(s);
            }
        }
    }

    return next;
}

// ─────────────────────────────────────────────
// Display-Rendering
// ─────────────────────────────────────────────

/**
 * Text zentriert ausgeben (x = Mittelpunkt)
 */
void drawCenteredText(const String& text, int cx, int y) {
    int16_t  x1, y1;
    uint16_t w, h;
    display.getTextBounds(text.c_str(), 0, 0, &x1, &y1, &w, &h);
    display.setCursor(cx - w / 2, y);
    display.print(text);
}

/**
 * Langer Text auf mehrere Zeilen umbrechen (max. lineWidth Pixel breit)
 * Gibt neue Y-Position zurück
 */
int drawWrappedText(const String& text, int x, int y, int lineWidth) {
    String word, line;
    int16_t  bx, by;
    uint16_t bw, bh;
    int lineHeight = 20;  // Schätzwert, wird unten korrigiert

    // Zeilenhöhe ermitteln
    display.getTextBounds("Ag", 0, 0, &bx, &by, &bw, &bh);
    lineHeight = bh + 4;

    String remaining = text;
    remaining.trim();

    while (remaining.length() > 0) {
        int spaceIdx = remaining.indexOf(' ');
        if (spaceIdx < 0) {
            word = remaining;
            remaining = "";
        } else {
            word = remaining.substring(0, spaceIdx);
            remaining = remaining.substring(spaceIdx + 1);
        }

        String testLine = line.isEmpty() ? word : line + " " + word;
        display.getTextBounds(testLine.c_str(), x, 0, &bx, &by, &bw, &bh);

        if (bw > (uint16_t)lineWidth && !line.isEmpty()) {
            display.setCursor(x, y);
            display.print(line);
            y += lineHeight;
            line = word;
        } else {
            line = testLine;
        }
    }

    if (!line.isEmpty()) {
        display.setCursor(x, y);
        display.print(line);
        y += lineHeight;
    }

    return y;
}

/**
 * Hauptrendering – wird im GxEPD2-Page-Loop aufgerufen
 */
void renderDisplay(const MeetingInfo& meeting, const NextMeetingInfo& next) {
    display.fillScreen(GxEPD_WHITE);

    // ── Header ──────────────────────────────────────────────────────
    // Hintergrundsbalken
    display.fillRect(0, 0, 400, 38, GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);

    // Raumname links
    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(8, 26);
    display.print(ROOM_NAME);

    // Datum + Uhrzeit rechts
    String datetime = getCurrentDateTime();
    display.setFont(&FreeSans9pt7b);
    int16_t bx, by; uint16_t bw, bh;
    display.getTextBounds(datetime.c_str(), 0, 0, &bx, &by, &bw, &bh);
    display.setCursor(400 - bw - 8, 26);
    display.print(datetime);

    // ── Trennlinie ───────────────────────────────────────────────────
    display.drawLine(0, 39, 400, 39, GxEPD_BLACK);

    // ── Status-Banner ────────────────────────────────────────────────
    if (meeting.active) {
        // BELEGT – schwarzer Block
        display.fillRect(0, 45, 400, 120, GxEPD_BLACK);
        display.setTextColor(GxEPD_WHITE);
        display.setFont(&FreeSansBold24pt7b);
        drawCenteredText("BELEGT", 200, 115);
    } else {
        // FREI – weißer Block mit Rahmen
        display.drawRect(10, 50, 380, 108, GxEPD_BLACK);
        display.drawRect(12, 52, 376, 104, GxEPD_BLACK);
        display.setTextColor(GxEPD_BLACK);
        display.setFont(&FreeSansBold24pt7b);
        drawCenteredText("FREI", 200, 120);
    }

    display.setTextColor(GxEPD_BLACK);

    // ── Aktuelles Meeting ────────────────────────────────────────────
    if (meeting.active) {
        display.drawLine(0, 172, 400, 172, GxEPD_BLACK);

        // Titel
        display.setFont(&FreeSansBold12pt7b);
        display.setCursor(10, 197);
        // Titel kürzen wenn zu lang
        String title = meeting.title;
        display.getTextBounds(title.c_str(), 0, 0, &bx, &by, &bw, &bh);
        while (bw > 380 && title.length() > 3) {
            title = title.substring(0, title.length() - 4) + "...";
            display.getTextBounds(title.c_str(), 0, 0, &bx, &by, &bw, &bh);
        }
        display.print(title);

        // Zeitraum
        if (meeting.startTime.length() > 0) {
            String timeRange = extractTime(meeting.startTime) + " – " + extractTime(meeting.endTime);
            display.setFont(&FreeSans9pt7b);
            display.setCursor(10, 220);
            display.print(timeRange);
        }
    }

    // ── Nächstes Meeting ─────────────────────────────────────────────
    int divY = meeting.active ? 235 : 180;
    display.drawLine(0, divY, 400, divY, GxEPD_BLACK);

    display.setFont(&FreeSans9pt7b);
    display.setCursor(10, divY + 20);
    display.print("Naechstes Meeting:");

    if (next.valid && next.title.length() > 0) {
        display.setFont(&FreeSansBold9pt7b);
        String nextTitle = next.title;
        display.getTextBounds(nextTitle.c_str(), 0, 0, &bx, &by, &bw, &bh);
        while (bw > 380 && nextTitle.length() > 3) {
            nextTitle = nextTitle.substring(0, nextTitle.length() - 4) + "...";
            display.getTextBounds(nextTitle.c_str(), 0, 0, &bx, &by, &bw, &bh);
        }
        display.setCursor(10, divY + 42);
        display.print(nextTitle);

        if (next.startTime.length() > 0) {
            display.setFont(&FreeSans9pt7b);
            display.setCursor(10, divY + 62);
            // Start kann ISO oder HH:MM sein
            String startStr = next.startTime;
            if (startStr.indexOf('T') >= 0) {
                startStr = extractTime(startStr) + " Uhr";
            }
            display.print("Start: " + startStr);
        }
    } else {
        display.setFont(&FreeSans9pt7b);
        display.setCursor(10, divY + 42);
        display.print("Kein weiteres Meeting geplant");
    }

    // ── Footer ───────────────────────────────────────────────────────
    // Kleiner Hinweis unten rechts (Update-Indikator)
    display.setFont(nullptr);  // Default 5x7 Font
    display.setTextSize(1);
    display.setCursor(320, 292);
    display.print("Update: 5min");
}

// ─────────────────────────────────────────────
// Display aktualisieren
// ─────────────────────────────────────────────
void updateDisplay(const MeetingInfo& meeting, const NextMeetingInfo& next) {
    Serial.println("[Display] Starte Update...");

    display.setFullWindow();
    display.firstPage();
    do {
        renderDisplay(meeting, next);
    } while (display.nextPage());

    Serial.println("[Display] Update abgeschlossen.");
}

// ─────────────────────────────────────────────
// WiFi verbinden
// ─────────────────────────────────────────────
bool connectWiFi() {
    Serial.printf("[WiFi] Verbinde mit '%s'...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Verbunden! IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    } else {
        Serial.println("[WiFi] Verbindung fehlgeschlagen!");
        return false;
    }
}

// ─────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== Meetingraum-Display ===");

    // SPI mit custom Pins initialisieren
    epd_spi.begin(EPD_CLK, -1, EPD_MOSI, EPD_CS);

    // Display initialisieren
    display.epd2.selectSPI(epd_spi, SPISettings(4000000, MSBFIRST, SPI_MODE0));
    display.init(115200, true, 2, false);
    display.setRotation(0);   // 0 = Portrait 400x300; ggf. 1 für Landscape

    Serial.println("[Display] Initialisiert.");

    // Startbild anzeigen (Ladebildschirm)
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setFont(&FreeSansBold12pt7b);
        display.setCursor(10, 50);
        display.print(ROOM_NAME);
        display.setFont(&FreeSans9pt7b);
        display.setCursor(10, 80);
        display.print("Verbinde mit WiFi...");
    } while (display.nextPage());

    // WiFi verbinden
    if (!connectWiFi()) {
        // Fehler anzeigen
        display.setFullWindow();
        display.firstPage();
        do {
            display.fillScreen(GxEPD_WHITE);
            display.setTextColor(GxEPD_BLACK);
            display.setFont(&FreeSansBold12pt7b);
            display.setCursor(10, 80);
            display.print("WiFi Fehler!");
            display.setFont(&FreeSans9pt7b);
            display.setCursor(10, 110);
            display.print("Bitte Zugangsdaten in");
            display.setCursor(10, 130);
            display.print("secrets.h pruefen.");
        } while (display.nextPage());
        // Trotzdem weitermachen, Display zeigt Fehler
        return;
    }

    // NTP Zeitsync
    Serial.println("[NTP] Synchronisiere Zeit...");
    configTime(TZ_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER, "time.google.com");

    // Auf Zeitsync warten (max. 10s)
    struct tm timeinfo;
    int ntpAttempts = 0;
    while (!getLocalTime(&timeinfo) && ntpAttempts < 20) {
        delay(500);
        ntpAttempts++;
    }

    if (ntpAttempts < 20) {
        char timeBuf[32];
        strftime(timeBuf, sizeof(timeBuf), "%d.%m.%Y %H:%M:%S", &timeinfo);
        Serial.printf("[NTP] Zeit: %s\n", timeBuf);
    } else {
        Serial.println("[NTP] Zeitsync fehlgeschlagen – weiter ohne Zeit.");
    }

    // Ersten Datenabruf und Display-Update
    MeetingInfo    meeting = fetchCurrentMeeting();
    NextMeetingInfo next   = fetchNextMeeting();

    Serial.printf("[Status] Meeting aktiv: %s\n", meeting.active ? "JA" : "NEIN");
    if (meeting.active) {
        Serial.printf("[Status] Titel: %s, %s – %s\n",
            meeting.title.c_str(),
            meeting.startTime.c_str(),
            meeting.endTime.c_str());
    }

    updateDisplay(meeting, next);
}

// ─────────────────────────────────────────────
// Loop
// ─────────────────────────────────────────────
void loop() {
    static unsigned long lastUpdate = 0;

    // WiFi-Reconnect falls nötig
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Verbindung unterbrochen – reconnecte...");
        WiFi.reconnect();
        unsigned long t = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
            delay(500);
        }
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Reconnect fehlgeschlagen.");
            delay(30000);
            return;
        }
        Serial.println("[WiFi] Reconnect erfolgreich.");
    }

    unsigned long now = millis();
    if (lastUpdate == 0 || (now - lastUpdate) >= UPDATE_INTERVAL_MS) {
        lastUpdate = now;

        Serial.println("[Loop] Daten werden aktualisiert...");

        MeetingInfo    meeting = fetchCurrentMeeting();
        NextMeetingInfo next   = fetchNextMeeting();

        Serial.printf("[Loop] Meeting aktiv: %s\n", meeting.active ? "JA" : "NEIN");

        updateDisplay(meeting, next);
    }

    // Kurzes Schlafen um CPU-Last zu reduzieren
    delay(10000);  // 10 Sekunden warten, dann wieder prüfen
}
