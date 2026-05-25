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

## Build & Flash

### Arduino IDE 1.8.x
1. File → Preferences → Additional Boards Manager URLs, add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
2. Tools → Board → Boards Manager → search **esp32 by Espressif Systems** → Install
3. Tools → Board → ESP32 Arduino → **ESP32 Dev Module**
4. Sketch → Include Library → Manage Libraries → search **PubSubClient by Nick O'Leary** → Install
5. Open `DIYButton.ino`, select your port, Upload
6. To suppress jmdns noise in the console: launch Arduino from terminal as `arduino 2>/dev/null`

### PlatformIO
```bash
cd DIYButton
pio run -t upload          # USB first flash
pio device monitor         # serial monitor
```

---

## First Boot — Setup Portal

On first boot the device starts a WiFi AP named `DIY-Button-Setup-xxxxxxxx` (no password).
Connect to it from any phone or laptop — a captive portal pop-up should appear automatically.
If it doesn't, browse to `http://192.168.4.1`.

| Field | Description |
|-------|-------------|
| Device Name | Alphanumeric + `-_`, used as MQTT nodeId prefix and OTA hostname |
| WiFi SSID | Your 2.4 GHz network name |
| WiFi Password | Leave blank for open networks |
| MQTT Broker | IP address of your MQTT broker / Home Assistant host |
| MQTT Port | Default 1883 |
| MQTT Username | Optional — leave blank if broker has no auth |
| MQTT Password | Optional — leave blank if broker has no auth |
| OTA Password | Recommended; used for wireless firmware updates |
| Generated API Key | 32 random bytes (mbedtls CSPRNG), base64-encoded. Stored in NVS for reference. |

Click **Save & Reboot** — the device will connect to WiFi and register with Home Assistant automatically.

---

## Home Assistant Integration

### Prerequisites
- **MQTT integration** installed in HA (Settings → Devices & Services → Add Integration → MQTT)
- Pointed at the same broker the ESP32 uses
- **Enable discovery** checked (it is by default)

### Tested with
- Home Assistant OS 2026.5.4
- Mosquitto broker add-on (Settings → Apps → Mosquitto broker)
- MQTT integration using the "Use the official Mosquitto Mqtt Broker app" connection option

### Important notes from testing
- HA 2026.x requires `"platform":"mqtt"` in discovery payloads — older docs omit this field
- PubSubClient buffer must be **1024 bytes** — discovery payloads exceed 512 bytes and will silently fail if the buffer is too small
- If the MQTT integration shows `password_not_changed` in the password field, delete and re-add the integration using the Mosquitto automatic connection option rather than entering credentials manually

### What gets auto-discovered

After the device connects, HA automatically adds both entities under a single Device:

| Entity | Type | Description |
|--------|------|-------------|
| `binary_sensor.<deviceName>_button` | Binary Sensor | Pulses ON→OFF on each button press |
| `light.<deviceName>_led` | Light | Turn GPIO23 LED on/off from HA |

Find them at: Settings → Devices & Services → Devices → search your device name.

### MQTT Topic Layout

```
homeassistant/binary_sensor/<nodeId>/config        (retained, discovery)
homeassistant/binary_sensor/<nodeId>/state         (ON pulse → OFF)
homeassistant/binary_sensor/<nodeId>/availability  (online / offline LWT)

homeassistant/light/<nodeId>/config                (retained, discovery)
homeassistant/light/<nodeId>/state                 (ON / OFF, retained)
homeassistant/light/<nodeId>/set                   (HA → device command)
```

`<nodeId>` = `<deviceName>_<MAC-no-colons>` e.g. `Web-Button-01_007007e5b9ec`

---

## OTA Updates

After first boot, OTA is available via ArduinoOTA on port 3232.

**Arduino IDE:** Tools → Port → select the network port (device name), then upload normally.

**PlatformIO** — add to `[env:esp32dev]`:
```ini
upload_protocol = espota
upload_port     = Web-Button-01.local   ; or IP address
upload_flags    = --auth=YOUR_OTA_PASSWORD
```

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
- **PubSubClient buffer must be 1024** — `setBufferSize(1024)` in code + `MQTT_MAX_PACKET_SIZE=1024` in `platformio.ini`. Discovery payloads are ~570 bytes and silently fail if the buffer is too small.
- **`"platform":"mqtt"` required** — HA 2026.x will not process discovery payloads without this field.
- **NVS `isConfigured()`** is cached with a `static int8_t` so NVS isn't polled every loop iteration.
- **jmdns noise in Arduino IDE** — launch with `arduino 2>/dev/null` to suppress Java mDNS exception spam in the terminal.

---

## Why MQTT instead of ESPHome Native API?

The ESPHome native API uses a custom noise-protocol-encrypted protobuf framing that would require implementing ~500+ lines of low-level protocol code in Arduino with high risk of subtle incompatibilities. MQTT auto-discovery achieves identical HA integration (same entity types, same device grouping, same availability tracking) with a well-supported library and no protocol reverse-engineering.
