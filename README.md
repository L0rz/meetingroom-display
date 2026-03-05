# Meetingroom E-Paper Display

E-Paper Türschild für Meetingräume mit Home Assistant Integration.

![Display](docs/display.jpg)

## Hardware

- **Display:** Elecrow CrowPanel 4.2" E-Paper (400x300, SSD1683)
- **MCU:** ESP32-S3
- **SPI:** Bit-Banging (Hardware SPI funktioniert nicht auf CrowPanel)

## Features

- **Echtzeit-Status:** FREI / BELEGT mit Meeting-Details
- **MQTT:** Event-driven Updates (< 15s Reaktionszeit)
- **HTTP Fallback:** HA REST API Polling alle 3 Minuten
- **NTP:** Automatische Uhrzeit-Synchronisation
- **Anti-Ghosting:** Automatischer Full-Refresh alle 60 Minuten
- **Logo:** Konfigurierbares Firmenlogo im Header
- **Multi-Room:** MQTT Prefix konfigurierbar

## Setup

### 1. Konfiguration

```bash
cp src/config.h.example src/config.h
```

Passe `config.h` an deine Umgebung an:
- WiFi SSID + Passwort
- MQTT Broker Adresse + Credentials
- Home Assistant URL + Token
- Kalender-Entity
- Raumname + Firmenname

### 2. Logo (optional)

Eigenes Logo als C-Array in `src/helo_logo.h`:
- Format: 1-bit BW Bitmap
- Empfohlene Größe: 216x35px
- `LOGO_W`, `LOGO_H` Defines + `helo_logo[]` Array

Oder Logo deaktivieren: `#define SHOW_LOGO 0` in `config.h`

### 3. Build & Flash

```bash
pio run --target upload --upload-port /dev/ttyUSB0
```

## Home Assistant

### Kalender-Entity

Das Display liest den Status von einer HA Calendar Entity (z.B. `calendar.meetingraum`).

### Benötigte HA Helpers

```yaml
input_text:
  meetingraum_next_title:
    name: "Nächstes Meeting Titel"
    max: 255
  meetingraum_next_start:
    name: "Nächstes Meeting Start"
    max: 255
```

### Automationen

Zwei Automationen werden benötigt:
1. **Next Meeting Updater** — schreibt den nächsten Termin in die Input-Text Helpers
2. **MQTT Publisher** — publiziert Kalender-Status auf MQTT Topics

### MQTT Topics

| Topic | Beschreibung |
|-------|-------------|
| `meetingroom/occupied` | `true` / `false` |
| `meetingroom/current/title` | Aktueller Termin |
| `meetingroom/current/start` | Startzeit |
| `meetingroom/current/end` | Endzeit |
| `meetingroom/next/title` | Nächster Termin |
| `meetingroom/next/start` | Startzeit nächster Termin |

## Architektur

```
HA Calendar → Automation → MQTT Broker → ESP32-S3 → E-Paper
                  ↓
            Input Text Helpers → HTTP REST API → ESP32-S3 (Fallback)
```

## Pin-Belegung (CrowPanel 4.2")

| Pin | GPIO | Funktion |
|-----|------|----------|
| CLK | 12 | SPI Clock |
| MOSI | 11 | SPI Data |
| CS | 45 | Chip Select |
| DC | 46 | Data/Command |
| RST | 47 | Reset |
| BUSY | 48 | Busy Signal |
| PWR | 7 | Display Power |

## Lizenz

MIT
