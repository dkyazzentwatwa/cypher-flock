# Repository Guidelines

Cypher Flock is a compact Arduino-based ESP32 WiFi detector for passive 2.4 GHz monitoring, paired with a Flask/Socket.IO web dashboard. This guide covers how to work with both the firmware and the host-side API.

## Project Structure

```
flock-you/
├── flock-you.ino                       # Unified Arduino firmware entrypoint
├── src/
│   ├── FlockYouCore.h                  # Firmware implementation
│   ├── Config.h                        # Board profile selection + shared defaults
│   └── profiles/                       # ESP32 DevKit / ESP32-S3 / Cypherbox / Waveshare AMOLED pin profiles
├── api/
│   ├── flockyou.py                     # Flask + Socket.IO dashboard (main entry)
│   ├── requirements.txt                 # Python deps
│   └── templates/index.html             # Web dashboard frontend
├── datasets/                            # Research notes, target lists, OUI CSVs
├── img/                                 # Hardware and UI photos
└── partitions.csv                      # 16 MB custom layout for Waveshare AMOLED builds
```

## Firmware Build & Flash

Use `arduino-cli` for all firmware work.

**ESP32 DevKit:**
```bash
arduino-cli core install esp32:esp32
arduino-cli lib install "Adafruit SSD1306" "Adafruit GFX Library" "Adafruit NeoPixel" "U8g2_for_Adafruit_GFX" "NimBLE-Arduino" "TinyGPSPlus"
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

**Waveshare ESP32-S3-Touch-AMOLED-1.8:**
```bash
arduino-cli lib install "GFX Library for Arduino" "Arduino_DriveBus" "ESP32_IO_Expander" "XPowersLib"
FQBN='esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,USBMode=default,CDCOnBoot=cdc,PartitionScheme=custom'
PORT='/dev/cu.usbmodemXXXX'
BUILD_DIR='/tmp/flock-build-waveshare-amoled'
arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD_DIR" \
  --build-property "build.extra_flags=-DESP32 -DBOARD_PROFILE=ESP32_WAVESHARE_AMOLED_18" .
python3 - <<'PY'
import serial, time
port = '/dev/cu.usbmodemXXXX'
ser = serial.Serial(port, 1200)
ser.dtr = False
ser.rts = True
time.sleep(0.2)
ser.close()
PY
arduino-cli upload -p "$PORT" --fqbn "$FQBN" --input-dir "$BUILD_DIR" .
```

The root `partitions.csv` is a 16 MB custom layout for the Waveshare AMOLED profile. Cypherbox should keep using the built-in `PartitionScheme=no_ota` command above.

Cypherbox uses an onboard WS2812 RGB LED on GPIO 26. Keep its running heartbeat as a soft green pulse every 5 seconds and detection feedback as a red pulse.

Waveshare AMOLED uses SH8601 QSPI display pins `4/5/6/7/11/12`, FT3168 touch and AXP2101 PMIC on I2C `SDA=15` / `SCL=14`, touch interrupt `21`, BOOT on GPIO0, and 1-bit SD_MMC `CLK=2`, `CMD=1`, `D0=3`. Preserve the detector UI and JSON serial contract; do not port the voice-bot companion shell into this repo. For this profile, short BOOT clicks cycle channel hopping mode and long BOOT press toggles stealth. Keep SD_MMC non-formatting by default and expose mount/write errors through the `storage` serial command.

**Serial monitor:**
```bash
arduino-cli monitor -p /dev/cu.usbserial-XXXX -c baudrate=115200
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
