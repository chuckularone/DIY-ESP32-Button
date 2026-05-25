/*
 * DIYButton - Self-contained ESP32 firmware
 * Captive portal first-boot config + Home Assistant MQTT button/light
 *
 * Hardware:
 *   GPIO33 - Button (INPUT_PULLUP, active low)
 *   GPIO23 - LED
 *   ESP32-WROOM-DA (esp32dev, Arduino core 3.3.8)
 *
 * Boot flow:
 *   First boot (no NVS config) → AP "DIY-Button-Setup-xxxxxxxx" + captive portal
 *   Normal boot (config present) → WiFi + MQTT + HA auto-discovery
 *   Hold GPIO33 5s → wipe NVS → reboot to AP mode
 */

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <esp_system.h>

#include "config.h"
#include "portal.h"
#include "mqtt_ha.h"
#include "base64_util.h"

// ── GPIO ─────────────────────────────────────────────────────────────────────
#define PIN_BUTTON  10
#define PIN_LED     9

// ── Globals ───────────────────────────────────────────────────────────────────
Preferences prefs;
WebServer   server(80);
DNSServer   dns;

DeviceConfig cfg;           // populated by loadConfig() or portal
bool         ledState = false;

// ── Forward declarations ───────────────────────────────────────────────────────
void startAPMode();
void startNormalMode();
void handleOTA();
void wipeAndReboot();

// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n[DIYButton] Booting…");

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  // IMPORTANT: Set WiFi mode before reading MAC (avoids boot panic on core 3.3.8)
  WiFi.mode(WIFI_AP);
  delay(100);

  if (loadConfig(prefs, cfg)) {
    Serial.println("[DIYButton] Config found → normal mode");
    startNormalMode();
  } else {
    Serial.println("[DIYButton] No config → AP/portal mode");
    startAPMode();
  }
}

// =============================================================================
void loop() {
  static bool configured = isConfigured(prefs);

  // ── Reset hold detection (works in both modes) ──────────────────────────
  static unsigned long btnHoldStart = 0;
  static bool          btnWasHeld   = false;

  if (digitalRead(PIN_BUTTON) == LOW) {
    if (btnHoldStart == 0) btnHoldStart = millis();
    if (!btnWasHeld && (millis() - btnHoldStart >= 5000)) {
      btnWasHeld = true;
      Serial.println("[DIYButton] 5s hold → wiping config");
      digitalWrite(PIN_LED, HIGH); delay(200);
      digitalWrite(PIN_LED, LOW);  delay(200);
      digitalWrite(PIN_LED, HIGH); delay(200);
      digitalWrite(PIN_LED, LOW);
      wipeAndReboot();
    }
  } else {
    btnHoldStart = 0;
    btnWasHeld   = false;
  }

  if (configured) {
    loopNormal();   // defined in mqtt_ha.cpp
  } else {
    loopPortal(server, dns);  // defined in portal.cpp
  }
}

// =============================================================================
void startAPMode() {
  String mac = WiFi.macAddress();
  // Compute deterministic 8-char suffix: first 8 chars of hex MD5(MAC)
  // We use a simple djb2 hash for brevity (no mbedtls MD5 needed for cosmetics)
  uint32_t h = 5381;
  for (char c : mac) h = ((h << 5) + h) ^ (uint8_t)c;
  char suffix[9];
  snprintf(suffix, sizeof(suffix), "%08x", h);

  String apName = String("DIY-Button-Setup-") + suffix;
  Serial.printf("[AP] SSID: %s\n", apName.c_str());

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName.c_str());
  delay(500);

  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

  // DNS wildcard → all hostnames resolve to 192.168.4.1
  dns.start(53, "*", apIP);

  setupPortal(server, prefs, cfg);
  server.begin();
  Serial.println("[AP] Portal ready at http://192.168.4.1");
}

// =============================================================================
void startNormalMode() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid, cfg.wifiPass);

  Serial.printf("[WiFi] Connecting to %s", cfg.ssid);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) {
    delay(500); Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Failed to connect — rebooting in 5s");
    delay(5000);
    ESP.restart();
  }
  Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());

  // OTA
  ArduinoOTA.setHostname(cfg.deviceName);
  ArduinoOTA.setPassword(cfg.otaPass);
  ArduinoOTA.onStart([]() { Serial.println("[OTA] Start"); });
  ArduinoOTA.onEnd([]()   { Serial.println("[OTA] End");   });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("[OTA] Error[%u]\n", e);
  });
  ArduinoOTA.begin();
  Serial.println("[OTA] Ready");

  setupMQTT(cfg, PIN_BUTTON, PIN_LED);
}

// =============================================================================
void wipeAndReboot() {
  prefs.begin("buttonconfig", false);
  prefs.clear();
  prefs.end();
  delay(500);
  ESP.restart();
}
