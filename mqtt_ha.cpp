/*
 * mqtt_ha.cpp — MQTT + Home Assistant MQTT auto-discovery
 *
 * Entities published:
 *   binary_sensor  → button press events
 *   light          → LED on/off control
 *
 * Topic layout (all under homeassistant/<component>/<nodeId>/):
 *   config         → auto-discovery JSON (retained)
 *   state          → current state
 *   command        → HA → device commands (light only)
 *   availability   → online/offline (LWT)
 *
 * Device info block is included so HA groups both entities under one device.
 */

#include "mqtt_ha.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>

// ── Constants ─────────────────────────────────────────────────────────────────
#define MQTT_KEEPALIVE_SEC  15
#define RECONNECT_INTERVAL  5000   // ms between reconnect attempts

// ── Module state ──────────────────────────────────────────────────────────────
static WiFiClient    wifiClient;
static PubSubClient  mqtt(wifiClient);

static DeviceConfig  _cfg;
static int           _pinButton;
static int           _pinLed;

// Derived strings (built in setupMQTT)
static String nodeId;          // e.g. "diy_button_a1b2c3d4"
static String availTopic;      // homeassistant/binary_sensor/<nodeId>/availability
static String btnStateTopic;   // homeassistant/binary_sensor/<nodeId>/state
static String ledStateTopic;   // homeassistant/light/<nodeId>/state
static String ledCmdTopic;     // homeassistant/light/<nodeId>/set

// State
static bool   ledState       = false;
static bool   lastButtonRaw  = HIGH;
static bool   buttonPressed  = false;    // edge detect
static unsigned long lastReconnect = 0;

// ── Forward decls ─────────────────────────────────────────────────────────────
static void publishDiscovery();
static void publishLedState();
static bool reconnect();
static void onMessage(char* topic, byte* payload, unsigned int len);

// ── helpers ───────────────────────────────────────────────────────────────────
static String deviceJson() {
  // Shared device block referenced in both discovery payloads
  String mac = WiFi.macAddress();
  return
    "\"device\":{"
      "\"identifiers\":[\"" + nodeId + "\"],"
      "\"name\":\"" + String(_cfg.deviceName) + "\","
      "\"model\":\"DIY Button (ESP32-WROOM-DA)\","
      "\"manufacturer\":\"DIY\","
      "\"sw_version\":\"1.0.0\","
      "\"connections\":[[\"mac\",\"" + mac + "\"]]"
    "}";
}

// ── setupMQTT ─────────────────────────────────────────────────────────────────
void setupMQTT(const DeviceConfig& cfg, int pinButton, int pinLed) {
  _cfg       = cfg;
  _pinButton = pinButton;
  _pinLed    = pinLed;

  // Build nodeId from MAC (strip colons, lower-case)
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toLowerCase();
  nodeId = String(cfg.deviceName) + "_" + mac;

  // Build topics
  availTopic    = "homeassistant/binary_sensor/" + nodeId + "/availability";
  btnStateTopic = "homeassistant/binary_sensor/" + nodeId + "/state";
  ledStateTopic = "homeassistant/light/"          + nodeId + "/state";
  ledCmdTopic   = "homeassistant/light/"          + nodeId + "/set";

  // Configure PubSubClient
  mqtt.setServer(_cfg.mqttHost, _cfg.mqttPort);
  mqtt.setCallback(onMessage);
  mqtt.setKeepAlive(MQTT_KEEPALIVE_SEC);
  mqtt.setBufferSize(512);   // discovery payloads can be large

  Serial.printf("[MQTT] Broker: %s:%u\n", _cfg.mqttHost, _cfg.mqttPort);
  Serial.printf("[MQTT] nodeId: %s\n", nodeId.c_str());

  reconnect();  // first connect attempt
}

// ── loopNormal ────────────────────────────────────────────────────────────────
void loopNormal() {
  ArduinoOTA.handle();

  // ── WiFi watchdog ────────────────────────────────────────────────────────
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Lost connection — rebooting");
    delay(3000);
    ESP.restart();
  }

  // ── MQTT reconnect ────────────────────────────────────────────────────────
  if (!mqtt.connected()) {
    unsigned long now = millis();
    if (now - lastReconnect >= RECONNECT_INTERVAL) {
      lastReconnect = now;
      reconnect();
    }
  }
  mqtt.loop();

  // ── Button edge detection ─────────────────────────────────────────────────
  bool currentRaw = (digitalRead(_pinButton) == LOW);  // active low
  if (currentRaw && !lastButtonRaw) {
    // Falling edge → button pressed
    Serial.println("[BTN] Pressed");
    if (mqtt.connected()) {
      mqtt.publish(btnStateTopic.c_str(), "ON", false);
      // Immediately follow with OFF so HA sees a "trigger" event
      // (binary_sensor OFF after a brief pulse → button entity works correctly)
      delay(100);
      mqtt.publish(btnStateTopic.c_str(), "OFF", false);
    }
  }
  lastButtonRaw = currentRaw;

  // ── LED sync ──────────────────────────────────────────────────────────────
  digitalWrite(_pinLed, ledState ? HIGH : LOW);
}

// ── MQTT message callback ────────────────────────────────────────────────────
static void onMessage(char* topic, byte* payload, unsigned int len) {
  String t(topic);
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];

  Serial.printf("[MQTT] ← %s : %s\n", topic, msg.c_str());

  if (t == ledCmdTopic) {
    msg.toUpperCase();
    if (msg == "ON") {
      ledState = true;
    } else if (msg == "OFF") {
      ledState = false;
    }
    publishLedState();
  }
}

// ── reconnect ────────────────────────────────────────────────────────────────
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

  // Publish availability
  mqtt.publish(availTopic.c_str(), "online", true);

  // Subscribe to LED command
  mqtt.subscribe(ledCmdTopic.c_str());

  // Publish auto-discovery + initial states
  publishDiscovery();
  publishLedState();
  mqtt.publish(btnStateTopic.c_str(), "OFF", false);

  return true;
}

// ── publishDiscovery ──────────────────────────────────────────────────────────
static void publishDiscovery() {
  // ── Binary sensor (button) ─────────────────────────────────────────────
  {
    String cfgTopic = "homeassistant/binary_sensor/" + nodeId + "/config";
    String payload =
      "{"
        "\"name\":\"Button\","
        "\"unique_id\":\"" + nodeId + "_btn\","
        "\"platform\":\"mqtt\","
        "\"state_topic\":\"" + btnStateTopic + "\","
        "\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\","
        "\"availability_topic\":\"" + availTopic + "\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"device_class\":\"motion\","
        + deviceJson() +
      "}";
    mqtt.publish(cfgTopic.c_str(), payload.c_str(), true);
    Serial.println("[MQTT] Published binary_sensor discovery");
  }

  // ── Light (LED) ────────────────────────────────────────────────────────
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
    mqtt.publish(cfgTopic.c_str(), payload.c_str(), true);
    Serial.println("[MQTT] Published light discovery");
  }
}

// ── publishLedState ───────────────────────────────────────────────────────────
static void publishLedState() {
  mqtt.publish(ledStateTopic.c_str(), ledState ? "ON" : "OFF", true);
}
