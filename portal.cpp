/*
 * portal.cpp — Captive portal implementation
 *
 * Handles:
 *   GET  /           → config form
 *   POST /save       → validate + save + reboot
 *   GET  /hotspot-detect.html, /generate_204, /connecttest.txt,
 *        /redirect, /ncsi.txt, /fwlink, /library/test/success.html
 *        → 302 redirect to http://192.168.4.1/ (triggers captive portal pop-up)
 */

#include "portal.h"
#include "config.h"
#include "base64_util.h"
#include <Arduino.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// ── Minimal inline CSS + HTML ─────────────────────────────────────────────────
static const char PORTAL_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DIY Button Setup</title>
<style>
  body{font-family:sans-serif;background:#1a1a2e;color:#eee;margin:0;padding:16px}
  h1{color:#e94560;margin-bottom:4px}
  p.sub{color:#aaa;font-size:.9em;margin-top:0}
  .card{background:#16213e;border-radius:8px;padding:20px;max-width:420px;margin:0 auto}
  label{display:block;margin-top:12px;font-size:.85em;color:#aaa}
  input{width:100%;box-sizing:border-box;padding:8px;border-radius:4px;
        border:1px solid #333;background:#0f3460;color:#eee;font-size:1em;margin-top:4px}
  .row{display:flex;gap:8px}
  .row input{flex:1}
  button{margin-top:20px;width:100%;padding:10px;background:#e94560;color:#fff;
         border:none;border-radius:4px;font-size:1em;cursor:pointer}
  button:hover{background:#c73652}
  .key{background:#0a1628;border-radius:4px;padding:8px;font-size:.8em;
       word-break:break-all;color:#6ec6ff;margin-top:8px}
  .note{font-size:.75em;color:#888;margin-top:4px}
  .err{color:#ff6b6b;font-size:.85em;margin-top:8px}
</style></head><body>
<div class="card">
  <h1>&#128306; DIY Button</h1>
  <p class="sub">First-boot configuration</p>
  %ERROR%
  <form method="POST" action="/save">
    <label>Device Name (no spaces)</label>
    <input name="deviceName" placeholder="garage-button" value="%DEVNAME%" required
           pattern="[A-Za-z0-9_-]+" title="Letters, numbers, hyphen, underscore only">

    <label>WiFi SSID</label>
    <input name="ssid" placeholder="MyNetwork" value="%SSID%" required>

    <label>WiFi Password</label>
    <input name="wifiPass" type="password" placeholder="(leave blank if open)">

    <label>MQTT Broker Host / IP</label>
    <input name="mqttHost" placeholder="192.168.1.100" value="%MQTTHOST%" required>

    <label>MQTT Port</label>
    <input name="mqttPort" type="number" min="1" max="65535"
           placeholder="1883" value="%MQTTPORT%">

    <label>MQTT Username <span class="note">(optional)</span></label>
    <input name="mqttUser" placeholder="" value="%MQTTUSER%">

    <label>MQTT Password <span class="note">(optional)</span></label>
    <input name="mqttPass" type="password" placeholder="">

    <label>OTA Password</label>
    <input name="otaPass" type="password" placeholder="(recommended)" value="">

    <label>Generated API Key <span class="note">(auto-generated, save for reference)</span></label>
    <div class="key">%APIKEY%</div>
    <input name="apiKey" type="hidden" value="%APIKEY%">

    <button type="submit">Save &amp; Reboot</button>
  </form>
</div>
</body></html>
)rawhtml";

// ── Module-level state ─────────────────────────────────────────────────────────
static Preferences* _prefs  = nullptr;
static DeviceConfig* _cfg   = nullptr;
static String        _apiKey;

// ── Helper: send captive-portal redirect ──────────────────────────────────────
static void sendCaptiveRedirect(WebServer& server) {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

// ── Helper: build and send the config form ────────────────────────────────────
static void sendConfigPage(WebServer& server, const String& error = "") {
  String page = FPSTR(PORTAL_HTML);
  page.replace("%ERROR%", error.isEmpty() ? "" :
    "<div class='err'>&#9888; " + error + "</div>");
  page.replace("%DEVNAME%", _cfg->deviceName);
  page.replace("%SSID%",    _cfg->ssid);
  page.replace("%MQTTHOST%",_cfg->mqttHost);
  page.replace("%MQTTPORT%", String(_cfg->mqttPort ? _cfg->mqttPort : 1883));
  page.replace("%MQTTUSER%",_cfg->mqttUser);
  page.replace("%APIKEY%",  _apiKey);
  server.send(200, "text/html", page);
}

// ── setupPortal ───────────────────────────────────────────────────────────────
void setupPortal(WebServer& server, Preferences& prefs, DeviceConfig& cfg) {
  _prefs = &prefs;
  _cfg   = &cfg;

  // Generate API key once per AP session (stays constant until save)
  _apiKey = generateApiKey();
  strlcpy(cfg.apiKey, _apiKey.c_str(), sizeof(cfg.apiKey));

  // Default port
  if (cfg.mqttPort == 0) cfg.mqttPort = 1883;

  // ── Main config page ──────────────────────────────────────────────────────
  server.on("/", HTTP_GET, [&server]() {
    sendConfigPage(server);
  });

  // ── Save handler ──────────────────────────────────────────────────────────
  server.on("/save", HTTP_POST, [&server]() {
    String devName  = server.arg("deviceName");
    String ssid     = server.arg("ssid");
    String wifiPass = server.arg("wifiPass");
    String mqttHost = server.arg("mqttHost");
    String mqttPort = server.arg("mqttPort");
    String mqttUser = server.arg("mqttUser");
    String mqttPass = server.arg("mqttPass");
    String otaPass  = server.arg("otaPass");
    String apiKey   = server.arg("apiKey");

    // Basic validation
    if (devName.isEmpty() || ssid.isEmpty() || mqttHost.isEmpty()) {
      sendConfigPage(server, "Device name, SSID, and MQTT host are required.");
      return;
    }
    // Sanitise device name — alphanumeric, hyphen, underscore only
    for (char c : devName) {
      if (!isAlphaNumeric(c) && c != '-' && c != '_') {
        sendConfigPage(server, "Device name: letters, numbers, hyphen, underscore only.");
        return;
      }
    }

    strlcpy(_cfg->deviceName, devName.c_str(), sizeof(_cfg->deviceName));
    strlcpy(_cfg->ssid,       ssid.c_str(),    sizeof(_cfg->ssid));
    strlcpy(_cfg->wifiPass,   wifiPass.c_str(),sizeof(_cfg->wifiPass));
    strlcpy(_cfg->mqttHost,   mqttHost.c_str(),sizeof(_cfg->mqttHost));
    _cfg->mqttPort = mqttPort.isEmpty() ? 1883 : (uint16_t)mqttPort.toInt();
    strlcpy(_cfg->mqttUser,   mqttUser.c_str(),sizeof(_cfg->mqttUser));
    strlcpy(_cfg->mqttPass,   mqttPass.c_str(),sizeof(_cfg->mqttPass));
    strlcpy(_cfg->otaPass,    otaPass.c_str(), sizeof(_cfg->otaPass));
    strlcpy(_cfg->apiKey,     apiKey.c_str(),  sizeof(_cfg->apiKey));

    saveConfig(*_prefs, *_cfg);

    server.send(200, "text/html",
      "<html><body style='font-family:sans-serif;background:#1a1a2e;color:#eee;"
      "display:flex;justify-content:center;align-items:center;height:100vh;margin:0'>"
      "<div style='text-align:center'>"
      "<h2 style='color:#6dca8a'>&#10003; Saved!</h2>"
      "<p>Device will reboot and connect to your network.</p>"
      "<p style='color:#aaa;font-size:.85em'>You can close this window.</p>"
      "</div></body></html>");

    delay(1500);
    ESP.restart();
  });

  // ── Captive portal probe URLs ─────────────────────────────────────────────
  // iOS / macOS
  server.on("/hotspot-detect.html", HTTP_GET, [&server]() {
    sendCaptiveRedirect(server);
  });
  // Android (AOSP)
  server.on("/generate_204", HTTP_GET, [&server]() {
    sendCaptiveRedirect(server);
  });
  // Windows (NCSI)
  server.on("/connecttest.txt", HTTP_GET, [&server]() {
    sendCaptiveRedirect(server);
  });
  server.on("/ncsi.txt", HTTP_GET, [&server]() {
    sendCaptiveRedirect(server);
  });
  server.on("/redirect", HTTP_GET, [&server]() {
    sendCaptiveRedirect(server);
  });
  server.on("/fwlink", HTTP_GET, [&server]() {
    sendCaptiveRedirect(server);
  });
  // Firefox
  server.on("/library/test/success.html", HTTP_GET, [&server]() {
    sendCaptiveRedirect(server);
  });

  // Catch-all for any unmatched path → redirect to config
  server.onNotFound([&server]() {
    sendCaptiveRedirect(server);
  });
}

// ── loopPortal ────────────────────────────────────────────────────────────────
void loopPortal(WebServer& server, DNSServer& dns) {
  dns.processNextRequest();
  server.handleClient();
}
