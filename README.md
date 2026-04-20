# Flock-You: Surveillance Device Detector

<img src="flock.png" alt="Flock You" width="300px">

**Standalone BLE surveillance device detector with web dashboard, GPS wardriving, and session persistence.**

Available as part of the OUI-SPY project at [colonelpanic.tech](https://colonelpanic.tech)

---

## Overview

Flock-You detects Flock Safety surveillance cameras, Raven gunshot detectors, and related monitoring hardware using BLE-only heuristics. It runs a WiFi access point with a live web dashboard on your phone, tags detections with GPS from your phone's browser, and exports everything as JSON, CSV, or KML for Google Earth.

No WiFi sniffing — the radio is dedicated to serving the dashboard AP while BLE scans continuously in the background via ESP32 coexistence.

---

## What's on this branch (`promiscious`)

This branch adds a **WiFi sibling** to the BLE detector in a new `promiscuis-flock-you/` subdirectory. Same hardware class (XIAO ESP32-S3), same Flask dashboard, complementary RF coverage.

| | BLE detector (`src/main.cpp`) | WiFi promiscuous detector (`promiscuis-flock-you/main.cpp`) |
|---|---|---|
| Radio | 2.4 GHz BLE scan | 2.4 GHz 802.11 promiscuous sniff |
| Targets | Flock / Raven BLE fingerprints | Flock Safety WiFi infrastructure OUIs |
| Dashboard | Hosts own AP + web UI at `192.168.4.1` | No AP — emits Flask JSON only |
| GPS | Phone geolocation via on-device AP | Flask-side (USB NMEA / browser) |
| Persistence | SPIFFS session file | SPIFFS session file (same envelope+CRC format) |
| Coverage | BLE-advertising Flock gear | Flock infrastructure seen on air, including stations silent on the transmitter-side due to burst-sleep duty cycles |

Both firmwares emit the same Flask-compatible JSON schema over USB, so `api/flockyou.py` ingests them interchangeably. Run one, the other, or both in parallel on the same host — you get a merged detection map.

### WiFi firmware highlights

- **Promiscuous-mode sniff** on channels 1 / 6 / 11 with 350 ms dwell (configurable)
- **`addr1` + `addr2` matching** — the receiver-side check catches Flock stations that are silent on the transmitter side during their burst-sleep windows
- **Randomised-MAC and multicast guards** applied before OUI match to eliminate false positives
- **30-OUI target list** for Flock Safety infrastructure
- **SPIFFS persistence** with atomic CRC-envelope writes, `/prev_session.json` promotion on boot
- **Onboard LED flash + buzzer beep** per detection
- **Boot melody** — first 6 notes of SMB World 1-2 underground
- **USB-optional** — standalone operation with non-blocking Serial TX

See [`promiscuis-flock-you/README.md`](promiscuis-flock-you/README.md) for the full walkthrough.

### Research credit

All WiFi promiscuous research — the 30-OUI target list and the addr1-receiver detection technique — is the work of **ØяĐöØцяöЪöяцฐ / @NitekryDPaul**. The firmware on this branch is a mod of his original promiscuous-mode firmware with added SPIFFS persistence and Flask-dashboard integration. Full attribution and methodology in [`datasets/NitekryDPaul_wifi_ouis.md`](datasets/NitekryDPaul_wifi_ouis.md).

---

## Detection Methods

All detection is BLE-based:

| Method | Description |
|--------|-------------|
| **MAC prefix** | 20 known Flock Safety OUI prefixes (FS Ext Battery, Flock WiFi modules) |
| **BLE device name** | Case-insensitive substring match: `FS Ext Battery`, `Penguin`, `Flock`, `Pigvision` |
| **Manufacturer ID** | `0x09C8` (XUNTONG) — catches devices with no broadcast name. *From [wgreenberg/flock-you](https://github.com/wgreenberg/flock-you)* |
| **Raven service UUID** | Identifies Raven gunshot detectors by BLE GATT service UUIDs |
| **Raven FW estimation** | Determines firmware version (1.1.x / 1.2.x / 1.3.x) from advertised service patterns |

---

## Features

- **WiFi AP**: `flockyou` / password `flockyou123`
- **Web dashboard** at `192.168.4.1` — live detection feed, pattern database, export tools
- **GPS wardriving** — phone GPS via browser Geolocation API tags every detection with coordinates
- **Session persistence** — detections auto-save to flash (SPIFFS) every 60 seconds
- **Prior session tab** — previous session survives reboot and is viewable in the PREV tab
- **Export formats**: JSON, CSV, and KML (Google Earth) — current and prior sessions
- **Serial output** — Flask-compatible JSON over serial for live desktop ingestion
- **200 unique device storage** with FreeRTOS mutex thread safety
- **Crow call boot sounds** — modulated descending frequency sweeps with warble texture
- **Detection alerts** — ascending chirps + descending caw on new device detection
- **Heartbeat** — soft double coo every 10s while a device stays in range

---

## Enabling GPS (Android Chrome)

The dashboard uses your phone's GPS to geotag detections. Because it's served over HTTP, Chrome requires a one-time flag change:

1. Open a new Chrome tab and go to `chrome://flags`
2. Search for **"Insecure origins treated as secure"**
3. Add `http://192.168.4.1` to the text field
4. Set the flag to **Enabled**
5. Tap **Relaunch**

After relaunching, connect to the `flockyou` AP, open `192.168.4.1`, and tap the **GPS** card in the stats bar to grant location permission.

> **Note:** iOS Safari does not support Geolocation over HTTP. GPS wardriving requires Android with Chrome.

---

## Hardware

**Board:** Seeed Studio XIAO ESP32-S3

| Pin | Function |
|-----|----------|
| GPIO 3 | Piezo buzzer |
| GPIO 21 | LED (optional) |

---

## Building & Flashing

Requires [PlatformIO](https://platformio.org/).

```bash
cd flock-you
pio run                     # build
pio run -t upload           # flash
pio device monitor          # serial output
```

**Dependencies** (managed by PlatformIO):

- `NimBLE-Arduino` — BLE scanning
- `ESP Async WebServer` + `AsyncTCP` — web dashboard
- `ArduinoJson` — JSON serialization
- `SPIFFS` — session persistence to flash

---

## Flask Companion App

The `api/` folder contains a Flask web application for desktop analysis of detection data.

```bash
cd api
pip install -r requirements.txt
python flockyou.py
```

Open `http://localhost:5000` for the desktop dashboard.

**Import support:** JSON, CSV, and KML files exported from the ESP32 can be imported directly into the Flask app. Live serial ingestion is also supported — connect the ESP32 via USB and select the serial port in the Flask UI.

---

## Raven Gunshot Detector Detection

Flock-You identifies SoundThinking/ShotSpotter Raven devices through BLE service UUID fingerprinting:

| Service | UUID | Description |
|---------|------|-------------|
| Device Info | `0000180a-...` | Serial, model, firmware |
| GPS | `00003100-...` | Real-time coordinates |
| Power | `00003200-...` | Battery & solar status |
| Network | `00003300-...` | LTE/WiFi connectivity |
| Upload | `00003400-...` | Data transmission metrics |
| Error | `00003500-...` | Diagnostics & error logs |
| Health (legacy) | `00001809-...` | Firmware 1.1.x |
| Location (legacy) | `00001819-...` | Firmware 1.1.x |

Firmware version is estimated automatically from which service UUIDs are advertised.

---

## Acknowledgments

- **ØяĐöØцяöЪöяцฐ (@NitekryDPaul)** — **WiFi promiscuous detection research**: 30-OUI Flock Safety target list and the addr1-receiver detection technique that form the `promiscuis-flock-you` firmware on this branch. See `promiscuis-flock-you/` and `datasets/NitekryDPaul_wifi_ouis.md`. The WiFi firmware here is a mod of his original promiscuous-mode firmware.
- **Will Greenberg** ([@wgreenberg](https://github.com/wgreenberg)) — BLE manufacturer company ID detection (`0x09C8` XUNTONG) sourced from his [flock-you](https://github.com/wgreenberg/flock-you) fork
- **[DeFlock](https://deflock.me)** ([FoggedLens/deflock](https://github.com/FoggedLens/deflock)) — crowdsourced ALPR location data and detection methodologies. Datasets included in `datasets/`
- **[GainSec](https://github.com/GainSec)** — Raven BLE service UUID dataset (`raven_configurations.json`) enabling detection of SoundThinking/ShotSpotter acoustic surveillance devices

---

## OUI-SPY Firmware Ecosystem

Flock-You is part of the OUI-SPY firmware family:

| Firmware | Description | Board |
|----------|-------------|-------|
| **[OUI-SPY Unified](https://github.com/colonelpanichacks/oui-spy-unified-blue)** | Multi-mode BLE + WiFi detector | ESP32-S3 / ESP32-C5 |
| **[OUI-SPY Detector](https://github.com/colonelpanichacks/ouispy-detector)** | Targeted BLE scanner with OUI filtering | ESP32-S3 |
| **[OUI-SPY Foxhunter](https://github.com/colonelpanichacks/ouispy-foxhunter)** | RSSI-based proximity tracker | ESP32-S3 |
| **[Flock You](https://github.com/colonelpanichacks/flock-you)** | Flock Safety / Raven surveillance detection (this project) | ESP32-S3 |
| **[Sky-Spy](https://github.com/colonelpanichacks/Sky-Spy)** | Drone Remote ID detection | ESP32-S3 / ESP32-C5 |
| **[Remote-ID-Spoofer](https://github.com/colonelpanichacks/Remote-ID-Spoofer)** | WiFi Remote ID spoofer & simulator with swarm mode | ESP32-S3 |
| **[OUI-SPY UniPwn](https://github.com/colonelpanichacks/Oui-Spy-UniPwn)** | Unitree robot exploitation system | ESP32-S3 |

---

## Author

**colonelpanichacks**

**Oui-Spy devices available at [colonelpanic.tech](https://colonelpanic.tech)**

---

## Disclaimer

This tool is intended for security research, privacy auditing, and educational purposes. Detecting the presence of surveillance hardware in public spaces is legal in most jurisdictions. Always comply with local laws regarding wireless scanning and signal interception. The authors are not responsible for misuse.
