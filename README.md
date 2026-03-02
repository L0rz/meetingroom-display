# Meetingraum-Display

E-Paper Meetingraum-Statusanzeige auf Basis des **Elecrow CrowPanel 4.2"**  
(ESP32-S3 · SSD1683 · 400×300px · SPI)

## Features

- 🟢 **FREI** / 🔴 **BELEGT** – großes Status-Banner
- Aktuelles Meeting: Titel + Zeitraum (HH:MM – HH:MM)
- Nächstes Meeting: Titel + Startzeit
- Datum + Uhrzeit oben rechts (NTP-synchronisiert)
- Update alle **5 Minuten** via Home Assistant REST API

## Voraussetzungen

- [PlatformIO](https://platformio.org/) (VSCode Extension oder CLI)
- ESP32-S3 Treiber (Board-Support Package wird automatisch geladen)
- Home Assistant mit:
  - `calendar.test2` Entity (state: `on`/`off`, attributes: `message`, `start_time`, `end_time`)
  - `input_text.meetingraum_next_title`
  - `input_text.meetingraum_next_start`

## Setup

### 1. Secrets anlegen

```bash
cp include/secrets.h.template include/secrets.h
# Dann WIFI_SSID und WIFI_PASSWORD eintragen
```

Die `include/secrets.h` ist in `.gitignore` – nie committen!

### 2. Kompilieren & Flashen

```bash
# Im Projektverzeichnis:
pio run --target upload

# Oder in VSCode: PlatformIO → Upload
```

### 3. Seriellen Monitor öffnen

```bash
pio device monitor
# 115200 Baud
```

## Hardware-Pinout

| Funktion | GPIO |
|----------|------|
| SPI CLK  | 12   |
| SPI MOSI | 11   |
| CS       | 45   |
| DC       | 46   |
| RST      | 47   |
| BUSY     | 48   |

## Display-Klasse anpassen

Falls das Display nicht initialisiert wird, liegt es ggf. an der falschen Klasse.  
In `src/main.cpp` die Zeile mit `GxEPD2_420_GDEY042T81` durch eine Alternative ersetzen:

```cpp
// Alternativen (in GxEPD2 Library nachschauen):
GxEPD2_BW<GxEPD2_420_GDEY042T81, ...>    // SSD1683 (Standard)
GxEPD2_BW<GxEPD2_420_GYE042A87, ...>     // Andere 4.2"
GxEPD2_BW<GxEPD2_420, ...>               // Ältere Version
```

Und den passenden Header oben mit einbinden:
```cpp
#include <epd/GxEPD2_420_GDEY042T81.h>
```

## Zeitzone anpassen

In `src/main.cpp`:
```cpp
#define TZ_OFFSET_SEC   3600    // UTC+1 (Winterzeit)
#define DST_OFFSET_SEC  3600    // +1h Sommerzeit
```

Für automatische DST-Erkennung (POSIX TZ-String):
```cpp
setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
tzset();
```

## Projektstruktur

```
meetingroom-display/
├── platformio.ini          # Board + Bibliotheken
├── include/
│   └── secrets.h           # WiFi-Credentials (nicht committen!)
├── src/
│   └── main.cpp            # Hauptprogramm
├── .gitignore
└── README.md
```
