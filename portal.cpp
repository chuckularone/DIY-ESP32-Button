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

// ── HTML template ─────────────────────────────────────────────────────────────
static const char PORTAL_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DIY Button Setup</title>
<style>
  body{font-family:sans-serif;background:#1a1a2e;color:#eee;margin:0;padding:16px}
  h1{color:#e94560;margin-bottom:4px}
  h2{color:#6ec6ff;font-size:1em;margin:20px 0 4px}
  p.sub{color:#aaa;font-size:.9em;margin-top:0}
  .card{background:#16213e;border-radius:8px;padding:20px;max-width:460px;margin:0 auto}
  label{display:block;margin-top:12px;font-size:.85em;color:#aaa}
  input,select{width:100%;box-sizing:border-box;padding:8px;border-radius:4px;
        border:1px solid #333;background:#0f3460;color:#eee;font-size:1em;margin-top:4px}
  .row{display:flex;gap:8px}
  .row input{flex:1}
  .btn-row{display:flex;align-items:center;gap:8px;margin-top:10px}
  .btn-row label{margin:0;min-width:68px;font-size:.85em;color:#aaa}
  .btn-row input{flex:1;margin:0}
  .btn-group{background:#0a1628;border-radius:6px;padding:10px 12px;margin-top:8px}
  .hidden{display:none}
  button{margin-top:20px;width:100%;padding:10px;background:#e94560;color:#fff;
         border:none;border-radius:4px;font-size:1em;cursor:pointer}
  button:hover{background:#c73652}
  .key{background:#0a1628;border-radius:4px;padding:8px;font-size:.8em;
       word-break:break-all;color:#6ec6ff;margin-top:8px}
  .note{font-size:.75em;color:#888;margin-top:4px}
  .err{color:#ff6b6b;font-size:.85em;margin-top:8px}
  hr{border:none;border-top:1px solid #2a2a4e;margin:18px 0}
</style></head><body>
<div class="card">
  <h1>&#128306; DIY Button</h1>
  <p class="sub">First-boot configuration</p>
  %ERROR%
  <form method="POST" action="/save">

    <label>Device Name (no spaces)</label>
    <input name="deviceName" placeholder="garage-button" value="%DEVNAME%" required
           pattern="[A-Za-z0-9_-]+" title="Letters, numbers, hyphen, underscore only">

    <hr>
    <label>WiFi SSID</label>
    <input name="ssid" placeholder="MyNetwork" value="%SSID%" required>

    <label>WiFi Password</label>
    <input name="wifiPass" type="password" placeholder="(leave blank if open)">

    <hr>
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

    <hr>
    <h2>&#128065; Button Configuration</h2>

    <label>Number of Buttons</label>
    <select name="buttonCount" id="btnCount" onchange="updateBtnRows()">
      <option value="1" %SEL1%>1 button</option>
      <option value="2" %SEL2%>2 buttons</option>
      <option value="3" %SEL3%>3 buttons</option>
      <option value="4" %SEL4%>4 buttons</option>
      <option value="5" %SEL5%>5 buttons</option>
    </select>

    <div class="btn-group">
      <div class="btn-row" id="brow1">
        <label>Button 1 GPIO</label>
        <input name="btnPin0" type="number" min="0" max="39" value="%BPIN0%">
      </div>
      <div class="btn-row" id="brow2">
        <label>Button 2 GPIO</label>
        <input name="btnPin1" type="number" min="0" max="39" value="%BPIN1%">
      </div>
      <div class="btn-row" id="brow3">
        <label>Button 3 GPIO</label>
        <input name="btnPin2" type="number" min="0" max="39" value="%BPIN2%">
      </div>
      <div class="btn-row" id="brow4">
        <label>Button 4 GPIO</label>
        <input name="btnPin3" type="number" min="0" max="39" value="%BPIN3%">
      </div>
      <div class="btn-row" id="brow5">
        <label>Button 5 GPIO</label>
        <input name="btnPin4" type="number" min="0" max="39" value="%BPIN4%">
      </div>
    </div>
    <p class="note">LED is fixed on GPIO9. Button 1 is also the 5-second factory-reset hold.</p>

    <hr>
    <label>Generated API Key <span class="note">(auto-generated, save for reference)</span></label>
    <div class="key">%APIKEY%</div>
    <input name="apiKey" type="hidden" value="%APIKEY%">

    <button type="submit">Save &amp; Reboot</button>
  </form>
</div>
<script>
function updateBtnRows() {
  var n = parseInt(document.getElementById('btnCount').value);
  for (var i = 1; i <= 5; i++) {
    var row = document.getElementById('brow' + i);
    row.style.display = (i <= n) ? 'flex' : 'none';
  }
}
updateBtnRows();
</script>
</body></html>
)rawhtml";

// ── Module-level state ────────────────────────────────────────────────────────
static Preferences*  _prefs = nullptr;
static DeviceConfig* _cfg   = nullptr;
static String        _apiKey;

static void sendCaptiveRedirect(WebServer& server) {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

static void sendConfigPage(WebServer& server, const String& error = "") {
  String page = FPSTR(PORTAL_HTML);
  page.replace("%ERROR%", error.isEmpty() ? "" :
    "<div class='err'>&#9888; " + error + "</div>");
  page.replace("%DEVNAME%",  _cfg->deviceName);
  page.replace("%SSID%",     _cfg->ssid);
  page.replace("%MQTTHOST%", _cfg->mqttHost);
  page.replace("%MQTTPORT%", String(_cfg->mqttPort ? _cfg->mqttPort : 1883));
  page.replace("%MQTTUSER%", _cfg->mqttUser);
  page.replace("%APIKEY%",   _apiKey);

  // Button count selector
  int bc = _cfg->buttonCount ? _cfg->buttonCount : 1;
  for (int i = 1; i <= MAX_BUTTONS; i++) {
    String tok = "%SEL" + String(i) + "%";
    page.replace(tok, (i == bc) ? "selected" : "");
  }

  // GPIO pin values
  char key[8];
  for (int i = 0; i < MAX_BUTTONS; i++) {
    snprintf(key, sizeof(key), "%%BPIN%d%%", i);
    int pinVal = (_cfg->buttonPins[i] != 0 || i == 0)
                 ? _cfg->buttonPins[i]
                 : DEFAULT_BUTTON_PINS[i];
    page.replace(key, String(pinVal));
  }

  server.send(200, "text/html", page);
}

// ── setupPortal ───────────────────────────────────────────────────────────────
void setupPortal(WebServer& server, Preferences& prefs, DeviceConfig& cfg) {
  _prefs = &prefs;
  _cfg   = &cfg;

  _apiKey = generateApiKey();
  strlcpy(cfg.apiKey, _apiKey.c_str(), sizeof(cfg.apiKey));

  if (cfg.mqttPort == 0) cfg.mqttPort = 1883;
  if (cfg.buttonCount < 1 || cfg.buttonCount > MAX_BUTTONS) cfg.buttonCount = 1;

  // Fill in default pin values if not set
  for (int i = 0; i < MAX_BUTTONS; i++) {
    if (cfg.buttonPins[i] == 0 && i > 0)
      cfg.buttonPins[i] = DEFAULT_BUTTON_PINS[i];
  }
  // Button 0 default is GPIO10 (may legitimately be 0 only for buttons 4/5)
  if (cfg.buttonPins[0] == 0) cfg.buttonPins[0] = DEFAULT_BUTTON_PINS[0];

  server.on("/", HTTP_GET, [&server]() {
    sendConfigPage(server);
  });

  server.on("/save", HTTP_POST, [&server]() {
    String devName     = server.arg("deviceName");
    String ssid        = server.arg("ssid");
    String wifiPass    = server.arg("wifiPass");
    String mqttHost    = server.arg("mqttHost");
    String mqttPort    = server.arg("mqttPort");
    String mqttUser    = server.arg("mqttUser");
    String mqttPass    = server.arg("mqttPass");
    String otaPass     = server.arg("otaPass");
    String apiKey      = server.arg("apiKey");
    String btnCountStr = server.arg("buttonCount");

    if (devName.isEmpty() || ssid.isEmpty() || mqttHost.isEmpty()) {
      sendConfigPage(server, "Device name, SSID, and MQTT host are required.");
      return;
    }
    for (char c : devName) {
      if (!isAlphaNumeric(c) && c != '-' && c != '_') {
        sendConfigPage(server, "Device name: letters, numbers, hyphen, underscore only.");
        return;
      }
    }

    int btnCount = btnCountStr.isEmpty() ? 1 : btnCountStr.toInt();
    if (btnCount < 1 || btnCount > MAX_BUTTONS) btnCount = 1;

    strlcpy(_cfg->deviceName, devName.c_str(),  sizeof(_cfg->deviceName));
    strlcpy(_cfg->ssid,       ssid.c_str(),     sizeof(_cfg->ssid));
    strlcpy(_cfg->wifiPass,   wifiPass.c_str(), sizeof(_cfg->wifiPass));
    strlcpy(_cfg->mqttHost,   mqttHost.c_str(), sizeof(_cfg->mqttHost));
    _cfg->mqttPort = mqttPort.isEmpty() ? 1883 : (uint16_t)mqttPort.toInt();
    strlcpy(_cfg->mqttUser,   mqttUser.c_str(), sizeof(_cfg->mqttUser));
    strlcpy(_cfg->mqttPass,   mqttPass.c_str(), sizeof(_cfg->mqttPass));
    strlcpy(_cfg->otaPass,    otaPass.c_str(),  sizeof(_cfg->otaPass));
    strlcpy(_cfg->apiKey,     apiKey.c_str(),   sizeof(_cfg->apiKey));
    _cfg->buttonCount = (uint8_t)btnCount;

    char argName[10];
    for (int i = 0; i < MAX_BUTTONS; i++) {
      snprintf(argName, sizeof(argName), "btnPin%d", i);
      String val = server.arg(argName);
      _cfg->buttonPins[i] = val.isEmpty() ? DEFAULT_BUTTON_PINS[i] : val.toInt();
    }

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

  // Captive portal probe URLs — each handler is a separate registration
  // (shared lambda with captured reference can go stale on some core 3.x builds)
  server.on("/hotspot-detect.html",       HTTP_GET, [&server]() { sendCaptiveRedirect(server); });
  server.on("/generate_204",              HTTP_GET, [&server]() { sendCaptiveRedirect(server); });
  server.on("/connecttest.txt",           HTTP_GET, [&server]() { sendCaptiveRedirect(server); });
  server.on("/ncsi.txt",                  HTTP_GET, [&server]() { sendCaptiveRedirect(server); });
  server.on("/redirect",                  HTTP_GET, [&server]() { sendCaptiveRedirect(server); });
  server.on("/fwlink",                    HTTP_GET, [&server]() { sendCaptiveRedirect(server); });
  server.on("/library/test/success.html", HTTP_GET, [&server]() { sendCaptiveRedirect(server); });
  server.onNotFound([&server]() { sendCaptiveRedirect(server); });
}

// ── loopPortal ────────────────────────────────────────────────────────────────
void loopPortal(WebServer& server, DNSServer& dns) {
  dns.processNextRequest();
  server.handleClient();
}
