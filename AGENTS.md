# Repository Guidelines

Cypher Flock is a compact Arduino-based ESP32 WiFi detector for passive 2.4 GHz monitoring, paired with a Flask/Socket.IO web dashboard. This guide covers how to work with both the firmware and the host-side API.

## Project Structure

```
flock-you/
├── flock-you.ino                       # Unified Arduino firmware entrypoint
├── src/
│   ├── FlockYouCore.h                  # Firmware implementation
│   ├── Config.h                        # Board profile selection + shared defaults
│   └── profiles/                       # ESP32 DevKit / ESP32-S3 / Cypherbox pin profiles
├── api/
│   ├── flockyou.py                     # Flask + Socket.IO dashboard (main entry)
│   ├── requirements.txt                 # Python deps
│   └── templates/index.html             # Web dashboard frontend
├── datasets/                            # Research notes, target lists, OUI CSVs
├── img/                                 # Hardware and UI photos
└── partitions.csv                      # 4 MB-safe no-OTA layout for ESP32/Cypherbox
```

## Firmware Build & Flash

Use `arduino-cli` for all firmware work.

**ESP32 DevKit:**
```bash
arduino-cli core install esp32:esp32
arduino-cli lib install "Adafruit SSD1306" "Adafruit GFX Library" "U8g2_for_Adafruit_GFX" "NimBLE-Arduino" "TinyGPSPlus"
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app \
  --build-property "build.extra_flags=-DESP32 -DBOARD_PROFILE=ESP32_DEVKIT" .
arduino-cli upload --fqbn esp32:esp32:esp32:PartitionScheme=huge_app \
  --build-property "build.extra_flags=-DESP32 -DBOARD_PROFILE=ESP32_DEVKIT" \
  -p /dev/cu.usbserial-XXXX .
```

**ESP32-S3:**
```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:PartitionScheme=huge_app \
  --build-property "build.extra_flags=-DESP32 -DBOARD_PROFILE=ESP32_S3" .
arduino-cli upload --fqbn esp32:esp32:esp32s3:PartitionScheme=huge_app \
  --build-property "build.extra_flags=-DESP32 -DBOARD_PROFILE=ESP32_S3" \
  -p /dev/cu.usbserial-XXXX .
```

**Cypherbox:**
```bash
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=no_ota \
  --build-property "build.extra_flags=-DESP32 -DBOARD_PROFILE=ESP32_CYPHERBOX" .
arduino-cli upload --fqbn esp32:esp32:esp32:PartitionScheme=no_ota \
  --build-property "build.extra_flags=-DESP32 -DBOARD_PROFILE=ESP32_CYPHERBOX" \
  -p /dev/cu.usbserial-XXXX .
```

The root `partitions.csv` is a 4 MB-safe no-OTA layout with a 2 MB app slot and LittleFS storage. Use it for Cypherbox so the board does not receive an invalid partition table.

**Serial monitor:**
```bash
arduino-cli monitor -p /dev/cu.usbserial-XXXX --baud 115200
```

Port names vary per machine — detect with `arduino-cli board list` or `ls /dev/cu.*`.

## API Dashboard

```bash
cd api
pip install -r requirements.txt
python flockyou.py
# Open http://localhost:5000
```

The API expects JSON detection events from the ESP32 over USB serial or via HTTP POST to `/api/detections`. It also manages GPS dongle input, OUI database refresh, and real-time WebSocket updates.

## Coding Style

- **Firmware**: Arduino/C++ files — standard Arduino/C++ conventions, 4-space indent, pin constants in `src/profiles/*.h`.
- **Python (API)**: PEP 8, Flask + Socket.IO patterns, no heavy frameworks.
- **Naming**: descriptive constants (`LED_PIN`, `USE_BUZZER`), snake_case for Python vars.
- No auto-formatters on firmware files; preserve readability for portability.

## Testing

- Firmware: manually verify on target hardware — compile success is not sufficient for timing/UI/serial behavior.
- API: run the Flask dev server and test endpoints with a connected board or mock JSON payload.
- GPS: test with an actual NMEA GPS dongle for location-tagging flows.

## Commit & PR Conventions

- Commits should describe what changed and why (e.g., `firmware: add 31st OUI from DeFlockJoplin research`).
- PRs should reference the issue or context and include a photo/video of hardware behavior when applicable.
- Do not commit large binary blobs, compiled firmware binaries, or the `exports/` directory — those are gitignored.
