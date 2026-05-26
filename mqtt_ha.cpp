/*
 * mqtt_ha.cpp — MQTT + Home Assistant MQTT auto-discovery
 *
 * Entities published:
 *   binary_sensor  → one per configured button (Button 1 … Button N)
 *   light          → LED on/off control
 *
 * Topic layout (all under homeassistant/<component>/<nodeId>_<suffix>/):
 *   config         → auto-discovery JSON (retained)
 *   state          → current state
 *   command        → HA → device commands (light only)
 *   availability   → online/offline (LWT)
 *
 * Device info block is included so HA groups all entities under one device.
 */

#include "mqtt_ha.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>

// ── Constants ──────────────────────────────────────────────────────────────────
#define MQTT_KEEPALIVE_SEC  15
#define RECONNECT_INTERVAL  5000   // ms between reconnect attempts

// ── Module state ───────────────────────────────────────────────────────────────
static WiFiClient    wifiClient;
static PubSubClient  mqtt(wifiClient);

static DeviceConfig  _cfg;
static int           _pinLed;

// Derived strings (built in setupMQTT)
static String nodeId;           // e.g. "diy_button_a1b2c3d4"
static String availTopic;       // single availability topic for the whole device
static String ledStateTopic;
static String ledCmdTopic;

// Per-button state topics (up to MAX_BUTTONS)
static String btnStateTopic[MAX_BUTTONS];

// Runtime state
static bool          ledState              = false;
static bool          lastButtonRaw[MAX_BUTTONS];
static unsigned long lastReconnect         = 0;

// ── Forward decls ──────────────────────────────────────────────────────────────
static void publishDiscovery();
static void publishLedState();
static bool reconnect();
static void onMessage(char* topic, byte* payload, unsigned int len);

// ── helpers ────────────────────────────────────────────────────────────────────
static String deviceJson() {
  String mac = WiFi.macAddress();
  return
    "\"device\":{"
      "\"identifiers\":[\"" + nodeId + "\"],"
      "\"name\":\"" + String(_cfg.deviceName) + "\","
      "\"model\":\"DIY Button (ESP32)\","
      "\"manufacturer\":\"DIY\","
      "\"sw_version\":\"1.1.0\","
      "\"connections\":[[\"mac\",\"" + mac + "\"]]"
    "}";
}

// ── setupMQTT ──────────────────────────────────────────────────────────────────
void setupMQTT(const DeviceConfig& cfg, int pinLed) {
  _cfg    = cfg;
  _pinLed = pinLed;

  // Initialise per-button state
  for (int i = 0; i < MAX_BUTTONS; i++) lastButtonRaw[i] = HIGH;

  // Build nodeId from MAC (strip colons, lower-case)
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toLowerCase();
  nodeId = String(cfg.deviceName) + "_" + mac;

  // Single availability topic shared by all entities
  availTopic    = "homeassistant/status/" + nodeId + "/availability";
  ledStateTopic = "homeassistant/light/"  + nodeId + "/state";
  ledCmdTopic   = "homeassistant/light/"  + nodeId + "/set";

  // Per-button state topics
  for (int i = 0; i < cfg.buttonCount; i++) {
    btnStateTopic[i] = "homeassistant/binary_sensor/" + nodeId
                       + "_btn" + String(i + 1) + "/state";
  }

  // Configure PubSubClient
  mqtt.setServer(_cfg.mqttHost, _cfg.mqttPort);
  mqtt.setCallback(onMessage);
  mqtt.setKeepAlive(MQTT_KEEPALIVE_SEC);
  mqtt.setBufferSize(1024);

  Serial.printf("[MQTT] Broker: %s:%u\n", _cfg.mqttHost, _cfg.mqttPort);
  Serial.printf("[MQTT] nodeId: %s\n", nodeId.c_str());
  Serial.printf("[MQTT] Buttons: %d\n", cfg.buttonCount);

  reconnect();  // first connect attempt
}

// ── loopNormal ─────────────────────────────────────────────────────────────────
void loopNormal() {
  ArduinoOTA.handle();

  // WiFi watchdog
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Lost connection — rebooting");
    delay(3000);
    ESP.restart();
  }

  // MQTT reconnect
  if (!mqtt.connected()) {
    unsigned long now = millis();
    if (now - lastReconnect >= RECONNECT_INTERVAL) {
      lastReconnect = now;
      reconnect();
    }
  }
  mqtt.loop();

  // Per-button edge detection
  for (int i = 0; i < _cfg.buttonCount; i++) {
    bool currentRaw = (digitalRead(_cfg.buttonPins[i]) == LOW);  // active low
    if (currentRaw && !lastButtonRaw[i]) {
      Serial.printf("[BTN%d] Pressed (GPIO%d)\n", i + 1, _cfg.buttonPins[i]);
      if (mqtt.connected()) {
        mqtt.publish(btnStateTopic[i].c_str(), "ON",  false);
        delay(100);
        mqtt.publish(btnStateTopic[i].c_str(), "OFF", false);
      }
    }
    lastButtonRaw[i] = currentRaw;
  }

  // LED sync
  digitalWrite(_pinLed, ledState ? HIGH : LOW);
}

// ── MQTT message callback ─────────────────────────────────────────────────────
static void onMessage(char* topic, byte* payload, unsigned int len) {
  String t(topic);
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];

  Serial.printf("[MQTT] ← %s : %s\n", topic, msg.c_str());

  if (t == ledCmdTopic) {
    msg.toUpperCase();
    if      (msg == "ON")  ledState = true;
    else if (msg == "OFF") ledState = false;
    publishLedState();
  }
}

// ── reconnect ─────────────────────────────────────────────────────────────────
static bool reconnect() {
  String clientId = nodeId + "_" + String(millis());

  Serial.printf("[MQTT] Connecting as %s…\n", clientId.c_str());

  bool ok;
  if (strlen(_cfg.mqttUser) > 0) {
    ok = mqtt.connect(clientId.c_str(),
                      _cfg.mqttUser, _cfg.mqttPass,
                      availTopic.c_str(), 1, true, "offline");
  } else {
    ok = mqtt.connect(clientId.c_str(),
                      nullptr, nullptr,
                      availTopic.c_str(), 1, true, "offline");
  }

  if (!ok) {
    Serial.printf("[MQTT] Failed, rc=%d\n", mqtt.state());
    return false;
  }

  Serial.println("[MQTT] Connected");

  mqtt.publish(availTopic.c_str(), "online", true);
  mqtt.subscribe(ledCmdTopic.c_str());

  publishDiscovery();
  publishLedState();

  // Publish all button initial states
  for (int i = 0; i < _cfg.buttonCount; i++) {
    mqtt.publish(btnStateTopic[i].c_str(), "OFF", false);
  }

  return true;
}

// ── publishDiscovery ──────────────────────────────────────────────────────────
static void publishDiscovery() {
  // One binary_sensor discovery per button
  for (int i = 0; i < _cfg.buttonCount; i++) {
    String suffix    = "_btn" + String(i + 1);
    String cfgTopic  = "homeassistant/binary_sensor/" + nodeId + suffix + "/config";
    String btnName   = "Button " + String(i + 1);
    String payload =
      "{"
        "\"name\":\"" + btnName + "\","
        "\"unique_id\":\"" + nodeId + suffix + "\","
        "\"platform\":\"mqtt\","
        "\"state_topic\":\"" + btnStateTopic[i] + "\","
        "\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\","
        "\"availability_topic\":\"" + availTopic + "\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        + deviceJson() +
      "}";
    bool ok = mqtt.publish(cfgTopic.c_str(), payload.c_str(), true);
    Serial.printf("[MQTT] Discovery btn%d: %s\n", i + 1, ok ? "OK" : "FAILED");
  }

  // Light (LED) discovery
  {
    String cfgTopic = "homeassistant/light/" + nodeId + "/config";
    String payload =
      "{"
        "\"name\":\"LED\","
        "\"unique_id\":\"" + nodeId + "_led\","
        "\"platform\":\"mqtt\","
        "\"state_topic\":\"" + ledStateTopic + "\","
        "\"command_topic\":\"" + ledCmdTopic + "\","
        "\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\","
        "\"availability_topic\":\"" + availTopic + "\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        + deviceJson() +
      "}";
    bool ok = mqtt.publish(cfgTopic.c_str(), payload.c_str(), true);
    Serial.printf("[MQTT] Discovery light: %s\n", ok ? "OK" : "FAILED");
  }
}

// ── publishLedState ───────────────────────────────────────────────────────────
static void publishLedState() {
  mqtt.publish(ledStateTopic.c_str(), ledState ? "ON" : "OFF", true);
}
