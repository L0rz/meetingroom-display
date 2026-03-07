#ifndef CONFIG_PORTAL_H
#define CONFIG_PORTAL_H

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>

// ══════════════════════════════════════════════════════════════════
//  Runtime Config — loaded from NVS, fallback to config.h defaults
// ══════════════════════════════════════════════════════════════════

struct RuntimeConfig {
  // WiFi
  String wifiSsid;
  String wifiPass;

  // MQTT
  String mqttHost;
  uint16_t mqttPort;
  String mqttUser;
  String mqttPass;
  String mqttClientId;
  String mqttPrefix;

  // Home Assistant
  String haHost;
  uint16_t haPort;
  String haToken;
  String haCalendarEntity;
  String haNextTitle;
  String haNextStart;

  // Display
  String roomName;
  String companyName;

  // Timing (ms)
  unsigned long httpIntervalMs;
  unsigned long minDrawMs;
  unsigned long mqttSettleMs;
  unsigned long fullRefreshMs;

  // NTP / Timezone
  String ntpServer;
  String timezone;
};

extern RuntimeConfig cfg;
extern bool apMode;

// ── Functions ────────────────────────────────────────────────────
void configLoadFromNVS();
void configSetDefaults();
bool configStartWiFi(unsigned long timeoutMs = 15000);
void configStartAP();
void configSetupRoutes(WebServer& server);
void configLoopDNS();

#endif // CONFIG_PORTAL_H
