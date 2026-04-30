# Cypher Flock v2 — Modular Refactor Plan

## Goals
- **One codebase**: collapse `cypher_flock_esp32s3.ino` and `cypher_flock_esp32_devkit.ino` into a single `FlockYou.ino` sketch with board profiles selected at compile time.
- **BLE + Raven detection**: add ESP32-BLE-Arduino scanning with confidence scoring and Raven service-UUID fingerprinting.
- **6–7 OLED screens**: expand the display with live feed, activity chart, proximity bar, GPS view, and session stats.
- **Dual storage**: LittleFS on both boards; MicroSD on boards that support it (SPI SD card on ESP32 DevKit, SDIO on ESP32-S3 where applicable). Access via embedded AP webserver.
- **AP webserver**: onboard ESP32 serves a lightweight file browser over its own AP — download logs and CSVs directly from the device.
- **Python API tests**: add pytest coverage for the Flask dashboard.

---

## Directory Structure

```
flock-you/
├── FlockYou.ino                   # Main sketch entrypoint (thin — includes all modules)
├── src/
│   ├── Config.h / .cpp            # Board profile macros + compile-time guards
│   │   ├── profiles/
│   │   │   ├── ESP32_DevKit.h     # DevKit pinouts, buzzer/pins, SPI SD, WiFi + BLE
│   │   │   └── ESP32_S3.h         # S3 pinouts, buzzer/pins, LittleFS-only, WiFi + BLE
│   ├── Storage.h / .cpp            # LittleFS session + SD CSV logging
│   ├── WifiSniffer.h / .cpp       # Promiscuous WiFi monitor + channel hopping
│   ├── BleScanner.h / .cpp        # BLE scanner + Raven service-UUID detection
│   ├── DetectionTable.h / .cpp    # OUI ring buffer, confidence scoring, deduplication
│   ├── AlertQueue.h / .cpp        # ISR-safe alert queue (IRAM)
│   ├── Audio.h / .cpp             # Buzzer chirps, heartbeat beeps, alarm escalation
│   ├── Display.h / .cpp           # SSD1306 OLED — 6–7 screens
│   ├── Buttons.h / .cpp           # Three-button debounce + long-press
│   ├── WebServer.h / .cpp         # ESPAsyncWebServer — AP mode + file browser
│   └── JsonEmit.h / .cpp          # Flask-compatible JSON line emitter
├── api/
│   ├── flockyou.py
│   ├── requirements.txt
│   ├── templates/index.html
│   └── tests/                     # NEW — pytest suite
│       ├── __init__.py
│       ├── test_detections.py
│       ├── test_export.py
│       └── test_gps.py
├── datasets/
├── img/
├── partitions.csv
├── AGENTS.md
└── README.md
```

---

## Module Breakdown

### Config.h / profiles/*.h
- Define `BOARD_PROFILE` macro via arduino-cli `--build-property build.extra_flags`.
- Profile headers declare: pin constants, storage backend (LittleFS / SD), BLE availability, OLED geometry, buzzer pin, LED pin, AP default SSID/password.
- No runtime chip detection — selection is fully compile-time.

### Storage.cpp
- **LittleFS**: session persistence (session.json with CRC32 envelope, every 60 s) — works on both boards.
- **SD CSV logging** (DevKit SPI SD, S3 SDIO if available): `FlockLog_XXX.csv` with columns: `Uptime_ms, Date_Time, Channel, Capture_Type, Protocol, RSSI, MAC_Address, Device_Name, TX_Power, Detection_Method, Confidence, Confidence_Label, Extra_Data, Latitude, Longitude, Speed_MPH, Heading_Deg, Altitude_M`.
- SD writes buffered (10-entry flush) to avoid blocking the scanner loop.

### WifiSniffer.cpp
- Current logic moved from `.ino`: promiscuous callback in IRAM, OUI checks on addr2/addr1/addr3, wildcard-probe signature, channel hopping (adaptive dwell: 500 ms on 1/6/11, 200 ms on others).
- Adaptive channel dwell from flock-detection.

### BleScanner.cpp
- Uses ESP32-BLE-Arduino (`BLEDevice.h`, `BLEScan.h`).
- Coexists with WiFi on Core 0 via ESP32 WiFi/BLE coexistence.
- Detection methods: MAC OUI, BLE device name, manufacturer company ID (0x09C8/XUNTONG), Penguin number, Raven service UUIDs (GainSec dataset), address type analysis.
- Confidence scoring: each method contributes weighted points. Alarm thresholds: MEDIUM ≥40%, HIGH ≥70%, CERTAIN ≥85%.

### RavenDetector
- Embedded in BleScanner.cpp as a header of known Raven BLE service UUIDs.
- Extracts firmware version from advertised UUIDs (1.1.x, 1.2.x, 1.3.x).
- Tagged in detection log via `Detection_Method` field.

### DetectionTable.cpp
- MAC ring buffer with 5-minute re-detection window (fresh GPS coords on re-detect).
- RSSI trend tracking: rise-peak-fall pattern earns a confidence bonus.
- Confidence score aggregation across all active detection methods.
- `fyAddDetection()` returns confidence label + chirp-worthy flag.

### AlertQueue.h
- ISR-safe circular buffer (IRAM), populated from WiFi callback and BLE scan callback.
- Consumed by `drainAlertQueue()` in `loop()`.

### Audio.cpp
- Alarm escalation: 1 beep MEDIUM (1000 Hz), 3 beeps HIGH (1200 Hz), 5 rapid CERTAIN (1500 Hz).
- New-MAC chirp: two ascending notes.
- Heartbeat beep while target is in range (3 s window).
- 60-second cooldown between alarms.

### Display.cpp
- 7 screens cycled with Up/Down:
  1. Scanner status + channel
  2. Detection stats (Flock WiFi / Flock BLE / Raven — session + lifetime)
  3. Last capture detail + confidence
  4. Live signal feed (rolling log)
  5. GPS coordinates
  6. Activity bar chart (dets/sec over last 25 s)
  7. Signal proximity bar + RSSI distance label
- Stealth mode: long-press Select kills display + buzzer while scanning continues.

### WebServer.cpp
- Uses `ESPAsyncWebServer` on the ESP32's own AP (default SSID `CypherFlock`, password `flockpass`).
- Serves a simple HTML file browser listing all files in LittleFS and on SD.
- Endpoints:
  - `GET /` — file browser UI
  - `GET /files` — JSON list of available files with sizes
  - `GET /download/<filename>` — serve file (CSV, JSON, session log)
  - `GET /status` — JSON: uptime, detection counts, GPS lock, free heap
  - `POST /reset` — reset session counters
- AP configuration (SSID, password, channel) set via board profile constants.

### JsonEmit.cpp
- Emits Flask-compatible one-JSON-per-line format over USB serial.
- Includes: event, detection_method, protocol, mac_address, oui, device_name, rssi, channel, frequency, ssid, confidence, confidence_label, gps fields.
- BLE detections use `detection_method` tags: `ble_mac`, `ble_name`, `ble_mfg_0x09C8`, `ble_penguin_num`, `ble_raven_uuid`, `ble_static_addr`, `ble_pub_addr`.

---

## Build Commands

```bash
# DevKit
arduino-cli compile \
  --fqbn esp32:esp32:esp32 \
  --build-property "build.extra_flags=-DESP32 -DBOARD_PROFILE=ESP32_DEVKIT" \
  ./FlockYou.ino

arduino-cli upload \
  --fqbn esp32:esp32:esp32 \
  --build-property "build.extra_flags=-DESP32 -DBOARD_PROFILE=ESP32_DEVKIT" \
  -p /dev/cu.usbserial-XXXX \
  ./FlockYou.ino

# ESP32-S3
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3 \
  --build-property "build.extra_flags=-DESP32 -DBOARD_PROFILE=ESP32_S3" \
  ./FlockYou.ino

arduino-cli upload \
  --fqbn esp32:esp32:esp32s3 \
  --build-property "build.extra_flags=-DESP32 -DBOARD_PROFILE=ESP32_S3" \
  -p /dev/cu.usbserial-XXXX \
  ./FlockYou.ino
```

---

## Python API Tests

```
api/tests/
├── __init__.py
├── test_detections.py    # POST /api/detections, GET /api/detections, filter params, CSV/KML export
├── test_gps.py           # GPS port listing, connect/disconnect, NMEA parsing, GPS-tagged detections
└── conftest.py           # Flask test client fixture, mock serial fixture
```

---

## Implementation Order

1. Create `src/` directory and extract `Config.h` + profile headers (`ESP32_DevKit.h`, `ESP32_S3.h`).
2. Create `FlockYou.ino` as a thin wrapper that includes all `src/` modules.
3. Move `WifiSniffer.cpp`, `AlertQueue.h`, `DetectionTable.cpp` from existing `.ino`.
4. Add `BleScanner.cpp` + Raven UUIDs.
5. Add confidence scoring into `DetectionTable.cpp`.
6. Add `Storage.cpp` with LittleFS + SD support.
7. Add `Audio.cpp` with alarm escalation.
8. Expand `Display.cpp` to 7 screens.
9. Add `WebServer.cpp` with AP + file browser.
10. Add `JsonEmit.cpp`.
11. Verify both profiles compile (`arduino-cli compile` for ESP32 and ESP32-S3).
12. Flash both boards and verify on hardware.
13. Add pytest suite for Flask API.
14. Update `AGENTS.md` and `README.md`.
15. Delete the old `.ino` files and subfolder.

---

## Assumptions & Defaults

| Decision | Choice |
|---|---|
| BLE library | ESP32-BLE-Arduino (stable, familiar) |
| Board selection | Compile-time via `--build-property` (clean, no runtime cost) |
| AP mode | AP-only (no STA fallback in v1 — standalone operation) |
| SD logging | DevKit: SPI SD; S3: LittleFS only (no SDIO in this hardware config) |
| Confidence scoring | Weighted points, alarm at 40/70/85% |
| AP defaults | SSID `CypherFlock`, password `flockpass` (configurable in profile) |
