# promiscuis-flock-you

WiFi-side companion to [flock-you](https://github.com/colonelpanichacks/flock-you). Passively detects Flock Safety cameras, Raven gunshot detectors, and related surveillance infrastructure by sniffing 2.4 GHz management and data frames in promiscuous mode. Emits Flask-compatible JSON over USB for live dashboard ingestion, stores detections on-device in SPIFFS so it works standalone too.

---

## Credits

All OUI research, the promiscuous-mode detection strategy, and the original firmware this is modded from: **ØяĐöØцяöЪöяцฐ @NitekryDPaul**. The core discovery — that Flock stations with randomised transmitter addresses still show up as the *destination* of probe responses and data frames during their burst-sleep duty cycle — is his. The 30-OUI target list below is his research. This fork adds Flask-app integration and on-device persistence on top of his work.

---

## What it does

- Sets the WiFi radio to `WIFI_MODE_NULL` and enables promiscuous sniffing
- Hops channels (default 1 / 6 / 11, 350 ms dwell)
- Inspects every 802.11 management and data frame
- Flags any frame whose `addr2` (transmitter) **or** `addr1` (receiver) matches a known Flock / Raven / SoundThinking OUI
- Skips multicast (`addr1` broadcast frames) and randomised MACs (locally-administered bit set) before matching
- Beeps the buzzer and flashes the onboard LED on every new detection
- Emits one JSON line per detection over USB CDC in the exact schema the Flask dashboard expects
- Stores detections in SPIFFS with an atomic CRC-checked envelope, so nothing is lost across power cycles

Runs with or without USB attached. No AP, no web server — the radio stays dedicated to sniffing, channel hopping is preserved.

---

## Why `addr1` matters

Most WiFi sniffers only check the transmitter address (`addr2`). Flock infrastructure often goes long windows without transmitting — it sleeps, wakes in bursts, uploads, sleeps again. During that silence it may still show up on the air as the **destination** of probe responses or data frames from nearby APs.

Checking `addr1` (receiver) picks those up. It requires a multicast guard (`addr1` is `ff:ff:ff:ff:ff:ff` in beacons and broadcasts) and a randomised-MAC guard, both of which are done at the top of the match function.

This is the key insight from @NitekryDPaul's research.

---

## Hardware

- **Board**: Seeed Studio XIAO ESP32-S3
- **Buzzer**: piezo on GPIO3
- **LED**: onboard user LED on GPIO21 (active low)
- **Serial mirror**: TX-only on GPIO43 at 115200 baud (for attaching a second logger or a secondary device)

---

## OUI target list

All lowercase, colon-separated. From @NitekryDPaul's research:

```
70:c9:4e   3c:91:80   d8:f3:bc   80:30:49   b8:35:32
14:5a:fc   74:4c:a1   08:3a:88   9c:2f:9d   c0:35:32
94:08:53   e4:aa:ea   f4:6a:dd   f8:a2:d6   24:b2:b9
00:f4:8d   d0:39:57   e8:d0:fc   e0:4f:43   b8:1e:a4
70:08:94   58:8e:81   ec:1b:bd   3c:71:bf   58:00:e3
90:35:ea   5c:93:a2   64:6e:69   48:27:ea   a4:cf:12
```

Pre-compiled into a byte table in `setup()` so the matcher stays entirely in IRAM with no flash-resident lookups during callback execution.

---

## Architecture

```
  [2.4GHz air]
       │
       ▼
  wifiSniffer()           ← IRAM promiscuous callback (WiFi task)
       │                    fast match, no Serial, no malloc
       ▼
  alertQueue[32]          ← lock-free ring buffer (ISR-safe mux)
       │
       ▼
  drainAlertQueue()       ← loop() context
       │
       ├─► fyAddDetection()      ← always, every hit
       │        │
       │        ▼
       │   fyDet[200]            ← unique-by-MAC table
       │        │
       │        ▼
       │   autosaveTick()        ← every 60s when dirty
       │        │
       │        ▼
       │   fySaveSession()       ← atomic CRC-envelope write to SPIFFS
       │
       ├─► shouldSuppressDuplicate()   ← 5s per-MAC cooldown
       │
       └─► emitDetectionJSON()   ← USB CDC line for Flask
            buzzerBeep() + ledFlash()
```

The split between callback and loop is deliberate: the WiFi task has hard real-time constraints and can't call `Serial.print` or `malloc` safely. The callback writes only to the ring buffer; `loop()` does all the heavy work.

---

## SPIFFS wire format

File layout on flash (atomic, crash-safe):

```
Line 1: {"v":1,"count":N,"bytes":B,"crc":"0xXXXXXXXX"}
Line 2: [{"mac":"...","method":"...","rssi":...,...},...]
```

Save procedure:

1. Compute CRC32 + byte count over the serialised payload
2. Write envelope header + payload to `/session.tmp`
3. Re-read and re-validate `/session.tmp` (CRC check)
4. Remove `/session.json`
5. Atomic rename `/session.tmp` → `/session.json` (copy+delete fallback)

Boot recovery:

1. If `/session.json` validates, promote it to `/prev_session.json`
2. Otherwise try `/session.tmp` (interrupted save)
3. Delete both working files, start with an empty live table
4. `/prev_session.json` stays around for inspection

CRC32 uses the standard `0xEDB88320` polynomial so the same file can be verified on a host with any off-the-shelf CRC tool.

---

## Flask dashboard integration

This firmware emits the same JSON schema as the BLE `flock-you` firmware, so the Flask app (`api/flockyou.py` in [colonelpanichacks/flock-you](https://github.com/colonelpanichacks/flock-you)) ingests it with no changes.

Per-detection JSON line:

```json
{"event":"detection","detection_method":"wifi_oui_addr2","protocol":"wifi_2_4ghz","mac_address":"aa:bb:cc:dd:ee:ff","oui":"aa:bb:cc","device_name":"","rssi":-62,"channel":6,"frequency":2437,"ssid":""}
```

`detection_method` values:

- `wifi_oui_addr2` — transmitter-address OUI match
- `wifi_oui_addr1` — receiver-address OUI match (the @NitekryDPaul technique)
- `wifi_oui_addr3` — BSSID-address OUI match (management frames only, disabled by default)
- `wifi_ssid` — SSID keyword match (disabled by default)

### GPS wardriving

No AP, no on-device GPS — GPS is handled Flask-side. Plug a USB NMEA puck into the host running Flask, or open the Flask dashboard in a phone browser and let it post browser-geolocation updates. Flask does a temporal match between the detection timestamp and the GPS timeline, and exports JSON / CSV / KML for Google Earth.

### Running Flask

```bash
cd flock-you/api
pip install -r requirements.txt
python flockyou.py
```

Open `http://localhost:5000`, connect your serial port from the UI, and detections start showing up live.

---

## Build and flash

PlatformIO config for the XIAO ESP32-S3:

```ini
[env:xiao_esp32s3]
platform = espressif32@^6.3.0
board = seeed_xiao_esp32s3
framework = arduino
monitor_speed = 115200
upload_speed = 921600

build_flags =
    -DCORE_DEBUG_LEVEL=0
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DBOARD_HAS_PSRAM

board_build.arduino.memory_type = qio_opi
board_build.partitions = partitions.csv
board_build.filesystem = spiffs
```

`partitions.csv` is included — 1.9 MB SPIFFS partition, 6 MB app.

No extra libraries needed. `SPIFFS.h` ships with Arduino-ESP32 core.

---

## Config cheatsheet (top of `main.cpp`)

| Define | Default | Notes |
|---|---|---|
| `CHANNEL_MODE` | `CHANNEL_MODE_CUSTOM` | `CUSTOM` (1/6/11), `FULL_HOP` (1-11), or `SINGLE` |
| `CHANNEL_DWELL_MS` | 350 | Time on each channel before hop |
| `RSSI_MIN` | -95 | Drop frames weaker than this |
| `ALERT_COOLDOWN_MS` | 5000 | Per-MAC serial-emit rate limit |
| `CHECK_ADDR1` | 1 | The @NitekryDPaul receiver-side technique |
| `CHECK_ADDR3` | 0 | BSSID fallback (mgmt frames only) |
| `ENABLE_SSID_MATCH` | 0 | Substring match against `target_ssid_keywords[]` |
| `PROCESS_MGMT_FRAMES` | 1 | Beacons, probe req/resp, etc. |
| `PROCESS_DATA_FRAMES` | 1 | Data frames (where addr1 catch shines) |
| `MAX_DETECTIONS` | 200 | On-device table cap |
| `AUTOSAVE_INTERVAL_MS` | 60000 | SPIFFS save cadence |
| `LED_PIN` | 21 | Onboard user LED |
| `BUZZER_PIN` | 3 | Piezo |

---

## Standalone vs connected operation

**Without USB:** device boots, beeps, starts scanning, stores every unique detection to SPIFFS, flashes the onboard LED on each hit. Plug in later, the prior session is sitting in `/prev_session.json`.

**With USB + Flask running:** same thing, plus every detection streams live to the dashboard as a JSON line. Flask adds GPS (if configured) and deduplicates across MAC, building the wardriving map as you move.

Both modes work simultaneously — the SPIFFS write path doesn't care if a host is listening.

---

## Legal / intended use

Passive reception of publicly-broadcast 802.11 frames and public BLE advertisements. Privacy research, surveillance auditing, education. The device does not transmit and does not attempt to authenticate to any network.
