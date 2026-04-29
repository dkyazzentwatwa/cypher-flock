# Cypher Flock

<img src="img/cypher-flock1.JPG" alt="Cypher Flock hardware" width="100%">

Cypher Flock is a compact Arduino-based ESP32 WiFi detector for passive 2.4 GHz monitoring, built around a small OLED screen, three buttons, and a simple on-device workflow. It runs fully standalone on the board, and it can also stream detections over USB for a live dashboard on a computer.

This repo is now maintained as **Cypher Flock**.

## Gallery

| Hardware | Screen | UI |
|---|---|---|
| <img src="img/cypher-flock1.JPG" alt="Cypher Flock hardware photo" width="100%"> | <img src="img/cypher-flock2.JPG" alt="Cypher Flock screen photo" width="100%"> | <img src="img/cypher-flock3.JPG" alt="Cypher Flock UI photo" width="100%"> |

## What It Does

- Passively listens on 2.4 GHz WiFi
- Checks frames for known target OUIs and related signatures
- Saves detections locally in SPIFFS
- Emits one JSON line per hit over USB serial
- Shows live status on the SSD1306 display
- Uses three buttons for navigation and control

The current firmware is available in two Arduino sketches:

- [cypher_flock_esp32s3.ino](cypher_flock_esp32s3.ino)
- [cypher_flock_esp32_devkit/cypher_flock_esp32_devkit.ino](cypher_flock_esp32_devkit/cypher_flock_esp32_devkit.ino)

## Supported Boards

| Sketch | Target Board | Notes |
|---|---|---|
| `cypher_flock_esp32s3.ino` | ESP32-S3 DevKit | Uses the S3 wiring and S3-safe defaults |
| `cypher_flock_esp32_devkit/cypher_flock_esp32_devkit.ino` | ESP32 DevKit | Uses the normal ESP32 DevKit wiring |

## Hardware

### ESP32 DevKit wiring

| Part | Pin |
|---|---|
| SSD1306 SDA | GPIO 5 |
| SSD1306 SCL | GPIO 4 |
| Button Up | GPIO 34 |
| Button Down | GPIO 36 |
| Button Select | GPIO 39 |
| LED | GPIO 27 |
| Buzzer | Optional |

### ESP32-S3 sketch wiring

The ESP32-S3 version uses its own board-friendly defaults in the sketch file. If you are using that build, follow the pin constants at the top of [cypher_flock_esp32s3.ino](cypher_flock_esp32s3.ino).

## Button Behavior

- `Up` changes pages or increases the current menu value
- `Down` changes pages or decreases the current menu value
- `Select` opens and steps through the menu
- Long press on `Select` pauses or resumes sniffing

## Display

The OLED uses a 128x64 SSD1306 panel over I2C.

The splash screen now shows a framed `Cypher Flock` intro before the live detector view starts.

## Build

This project is Arduino-only. Use Arduino IDE or `arduino-cli`.

### Arduino CLI

For the ESP32 DevKit sketch:

```bash
arduino-cli core install esp32:esp32
arduino-cli lib install "Adafruit SSD1306" "Adafruit GFX Library" "FastLED"
arduino-cli compile --fqbn esp32:esp32:esp32 ./cypher_flock_esp32_devkit
arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/cu.usbserial-0001 ./cypher_flock_esp32_devkit
```

For the ESP32-S3 sketch:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3 ./cypher_flock_esp32s3.ino
arduino-cli upload --fqbn esp32:esp32:esp32s3 -p /dev/cu.usbserial-0001 ./cypher_flock_esp32s3.ino
```

## Serial Output

Each detection emits a single JSON object over USB serial. That keeps the board easy to pair with a host app or a terminal monitor.

Example:

```json
{"event":"detection","detection_method":"wifi_oui_addr2","protocol":"wifi_2_4ghz","mac_address":"aa:bb:cc:dd:ee:ff","oui":"aa:bb:cc","device_name":"","rssi":-62,"channel":6,"frequency":2437,"ssid":""}
```

## Files

| Path | Purpose |
|---|---|
| `cypher_flock_esp32s3.ino` | ESP32-S3 build |
| `cypher_flock_esp32_devkit/cypher_flock_esp32_devkit.ino` | ESP32 DevKit build |
| `api/flockyou.py` | Host-side Flask dashboard and serial ingester |
| `datasets/` | Research notes and target lists |
| `img/` | Project images |

## Acknowledgments

Cypher Flock builds on the open research and field work of others in the WiFi detection space. The target-list and signature work in this repo is credited in the code and datasets where it originated.

## License

If you want, I can add a real license file next. For now this repo is documented as a personal project fork under the Cypher Flock name.
