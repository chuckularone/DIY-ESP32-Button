# DIYButton — Self-contained ESP32 Firmware

Single-flash firmware for an ESP32-WROOM-DA that provides:
- **First-boot captive portal** — configure WiFi + MQTT + OTA via browser
- **Home Assistant auto-discovery** — button and LED appear automatically in HA via MQTT
- **Factory reset** — hold GPIO33 for 5 s to wipe config and re-enter setup mode

---

## Hardware

| Signal | GPIO | Notes |
|--------|------|-------|
| Button | 33 | INPUT_PULLUP, active low |
| LED    | 23 | Active high |

---

## Boot Flow

```
Power on
  └─ NVS "buttonconfig" has SSID + MQTT host?
        NO  → WiFi AP "DIY-Button-Setup-xxxxxxxx"
              Captive portal on 192.168.4.1
              DNS wildcard on :53
              User fills form → save → reboot
       YES  → WiFi STA → connect
              ArduinoOTA start
              MQTT connect → publish HA discovery
              Normal operation loop
```

### Factory Reset
Hold GPIO33 for **5 seconds** — LED flashes 3× → NVS wiped → reboot to AP mode.

---

## Setup Portal

Connect to the `DIY-Button-Setup-xxxxxxxx` AP (no password) from any device.
A captive portal pop-up should appear automatically; if not, browse to `http://192.168.4.1`.

Fields:

| Field | Description |
|-------|-------------|
| Device Name | Alphanumeric + `-_`, used as MQTT nodeId prefix and OTA hostname |
| WiFi SSID | Your 2.4 GHz network name |
| WiFi Password | Leave blank for open networks |
| MQTT Broker | IP or hostname of your MQTT broker |
| MQTT Port | Default 1883 |
| MQTT Username | Optional |
| MQTT Password | Optional |
| OTA Password | Recommended; used with ArduinoOTA and `platformio.ini` upload_flags |
| Generated API Key | 32 random bytes (mbedtls CSPRNG), base64-encoded. Save this as a reference token / shared secret. Not currently used for MQTT auth but available in NVS. |

---

## Home Assistant Integration

### Prerequisites
1. HA has the **MQTT integration** enabled and pointed at the same broker.
2. MQTT discovery is enabled (default: `true` in modern HA).

### What gets auto-discovered

After the device connects, HA will automatically add:

| Entity | Type | Description |
|--------|------|-------------|
| `binary_sensor.<deviceName>_button` | Binary Sensor | Pulses ON→OFF on each button press |
| `light.<deviceName>_led` | Light | Turn GPIO23 LED on/off from HA |

Both entities appear under a single **Device** card in HA (device name = your configured Device Name).

### MQTT Topic Layout

```
homeassistant/binary_sensor/<nodeId>/config        (retained, discovery)
homeassistant/binary_sensor/<nodeId>/state         (ON pulse → OFF)
homeassistant/binary_sensor/<nodeId>/availability  (online / offline LWT)

homeassistant/light/<nodeId>/config                (retained, discovery)
homeassistant/light/<nodeId>/state                 (ON / OFF, retained)
homeassistant/light/<nodeId>/set                   (HA → device command)
```

`<nodeId>` = `<deviceName>_<MAC-no-colons>` e.g. `garage-button_a4cf1234abcd`

---

## OTA Updates

After first boot, OTA is available via ArduinoOTA on port 3232 (mDNS name = device name).

**PlatformIO:**
```ini
; Add to [env:esp32dev] in platformio.ini:
upload_protocol = espota
upload_port     = garage-button.local   ; or IP address
upload_flags    = --auth=YOUR_OTA_PASSWORD
```

**Arduino IDE:** Sketch → Upload → select the network port.

---

## Build & Flash

### PlatformIO (recommended)
```bash
cd DIYButton
pio run -t upload          # USB first flash
pio device monitor         # serial monitor
```

### Arduino IDE
1. Install board: `esp32` by Espressif ≥ 3.3.8
2. Install library: **PubSubClient** by Nick O'Leary
3. Open `DIYButton.ino`
4. Board: *ESP32 Dev Module*, Upload Speed: 921600
5. Upload

---

## File Structure

```
DIYButton/
  DIYButton.ino   — main sketch (setup/loop, AP init, OTA, factory reset)
  config.h        — DeviceConfig struct, NVS load/save helpers
  base64_util.h   — inline base64 encoder + mbedtls key generator
  portal.h/.cpp   — captive portal web server
  mqtt_ha.h/.cpp  — MQTT client + HA auto-discovery
  platformio.ini  — PlatformIO build config
  README.md       — this file
```

---

## Known Gotchas

- **`WiFi.mode(WIFI_AP)` before `WiFi.macAddress()`** — required on core 3.3.8 to avoid boot panic.
- **Only one `.ino` file** — extra `.ino` files in the sketch folder cause redefinition errors.
- **PubSubClient buffer** — `setBufferSize(512)` + `MQTT_MAX_PACKET_SIZE=512` in `platformio.ini` prevents discovery payload truncation.
- **NVS `isConfigured()`** is cached with a `static int8_t` so NVS isn't polled every loop iteration.
- If HA doesn't pick up entities after boot, check Mosquitto logs for connection / topic errors. The discovery topics are retained so re-connecting the MQTT integration should pick them up.

---

## Why MQTT instead of ESPHome Native API?

The ESPHome native API uses a custom noise-protocol-encrypted protobuf framing that would require implementing ~500+ lines of low-level protocol code in Arduino with high risk of subtle incompatibilities. MQTT auto-discovery achieves identical HA integration (same entity types, same device grouping, same availability tracking) with a well-supported library and no protocol reverse-engineering.
