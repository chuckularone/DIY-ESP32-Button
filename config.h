#pragma once
/*
 * config.h — DeviceConfig struct + NVS helpers
 * NVS namespace: "buttonconfig"
 * Keys: deviceName, ssid, wifiPass, mqttHost, mqttPort, mqttUser, mqttPass,
 *       otaPass, apiKey (display-only generated key)
 */

#include <Preferences.h>
#include <Arduino.h>

#define NVS_NS "buttonconfig"

struct DeviceConfig {
  char deviceName[64];   // e.g. "garage-button"
  char ssid[64];
  char wifiPass[64];
  char mqttHost[64];     // IP or hostname
  uint16_t mqttPort;     // default 1883
  char mqttUser[64];
  char mqttPass[64];
  char otaPass[64];
  char apiKey[64];       // base64 random key, generated on ESP32, display only
};

// Returns true if config exists and is populated
inline bool loadConfig(Preferences& prefs, DeviceConfig& cfg) {
  if (!prefs.begin(NVS_NS, true)) return false;  // read-only
  bool ok = prefs.isKey("ssid");
  if (ok) {
    strlcpy(cfg.deviceName, prefs.getString("deviceName", "diy-button").c_str(), sizeof(cfg.deviceName));
    strlcpy(cfg.ssid,       prefs.getString("ssid",       "").c_str(),           sizeof(cfg.ssid));
    strlcpy(cfg.wifiPass,   prefs.getString("wifiPass",   "").c_str(),           sizeof(cfg.wifiPass));
    strlcpy(cfg.mqttHost,   prefs.getString("mqttHost",   "").c_str(),           sizeof(cfg.mqttHost));
    cfg.mqttPort = (uint16_t)prefs.getUInt("mqttPort", 1883);
    strlcpy(cfg.mqttUser,   prefs.getString("mqttUser",   "").c_str(),           sizeof(cfg.mqttUser));
    strlcpy(cfg.mqttPass,   prefs.getString("mqttPass",   "").c_str(),           sizeof(cfg.mqttPass));
    strlcpy(cfg.otaPass,    prefs.getString("otaPass",    "").c_str(),           sizeof(cfg.otaPass));
    strlcpy(cfg.apiKey,     prefs.getString("apiKey",     "").c_str(),           sizeof(cfg.apiKey));
    ok = (strlen(cfg.ssid) > 0 && strlen(cfg.mqttHost) > 0);
  }
  prefs.end();
  return ok;
}

inline void saveConfig(Preferences& prefs, const DeviceConfig& cfg) {
  prefs.begin(NVS_NS, false);  // read-write
  prefs.putString("deviceName", cfg.deviceName);
  prefs.putString("ssid",       cfg.ssid);
  prefs.putString("wifiPass",   cfg.wifiPass);
  prefs.putString("mqttHost",   cfg.mqttHost);
  prefs.putUInt(  "mqttPort",   cfg.mqttPort);
  prefs.putString("mqttUser",   cfg.mqttUser);
  prefs.putString("mqttPass",   cfg.mqttPass);
  prefs.putString("otaPass",    cfg.otaPass);
  prefs.putString("apiKey",     cfg.apiKey);
  prefs.end();
}

// Lightweight check — read-only, just tests for the 'ssid' key
inline bool isConfigured(Preferences& prefs) {
  static int8_t cached = -1;
  if (cached >= 0) return (bool)cached;
  if (!prefs.begin(NVS_NS, true)) { cached = 0; return false; }
  bool ok = prefs.isKey("ssid");
  if (ok) ok = (prefs.getString("ssid", "").length() > 0 &&
                prefs.getString("mqttHost", "").length() > 0);
  prefs.end();
  cached = (int8_t)ok;
  return ok;
}
