#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include "esp_wifi.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <string>
#include "Config.h"
#if USE_CARDPUTER_DISPLAY
#include <CypherPuterReturn.h>
#endif

static void fyReturnToLauncher(uint32_t delayMs) {
#if USE_CARDPUTER_DISPLAY
  cypherPuterReturnToLauncher(delayMs);
#else
  delay(delayMs);
  ESP.restart();
#endif
}

#ifndef USB_SERIAL_WAIT_MS
#define USB_SERIAL_WAIT_MS 3000
#endif
#include <LittleFS.h>
#include <FS.h>
#if ENABLE_SD_LOGGING
#include <SPI.h>
#include <SD.h>
#if USE_SD_MMC
#include <SD_MMC.h>
#endif
#endif
#if ENABLE_GPS
#include <TinyGPSPlus.h>
#endif
#include <Wire.h>
#include <Adafruit_GFX.h>
#if USE_CARDPUTER_DISPLAY
#include <M5Cardputer.h>
#endif
#if USE_AMOLED_DISPLAY
#include <Adafruit_XCA9554.h>
#include <Arduino_GFX_Library.h>
#include <Arduino_DriveBus_Library.h>
#include <memory>
#endif
#if ENABLE_POWER_STATUS
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>
#endif
#if !USE_AMOLED_DISPLAY && !USE_CARDPUTER_DISPLAY
#include <Adafruit_SSD1306.h>
#include <U8g2_for_Adafruit_GFX.h>
#endif
#if USE_RGB_LED
#include <Adafruit_NeoPixel.h>
#endif

#define SPIFFS LittleFS

// ============================================================
// CONFIG
// ============================================================

static const uint8_t customChannels[]  = {1, 6, 11};
static const size_t  customChannelCount = sizeof(customChannels) / sizeof(customChannels[0]);

static const uint8_t fullHopChannels[] = {1,2,3,4,5,6,7,8,9,10,11,12,13};
static const size_t  fullHopChannelCount = sizeof(fullHopChannels) / sizeof(fullHopChannels[0]);

typedef struct FYDetection FYDetection;
typedef struct ButtonState ButtonState;

static const char* target_ssid_keywords[] = {
  "flock", "FS Ext Battery", "FS_", "Penguin", "Pigvision", "FlockOS", "flocksafety"
};
static const size_t SSID_KEYWORD_COUNT = sizeof(target_ssid_keywords) / sizeof(target_ssid_keywords[0]);

// ============================================================
// TARGET OUI LIST  (all lowercase, colons only)
// ============================================================

static const char* target_ouis[] = {
  "70:c9:4e", "3c:91:80", "d8:f3:bc", "80:30:49", "b8:35:32",
  "14:5a:fc", "74:4c:a1", "08:3a:88", "9c:2f:9d", "c0:35:32",
  "94:08:53", "e4:aa:ea", "f4:6a:dd", "f8:a2:d6", "24:b2:b9",
  "00:f4:8d", "d0:39:57", "e8:d0:fc", "e0:4f:43", "b8:1e:a4",
  "70:08:94", "58:8e:81", "ec:1b:bd", "3c:71:bf", "58:00:e3",
  "90:35:ea", "5c:93:a2", "64:6e:69", "48:27:ea", "a4:cf:12",
  // Contributed by Michael / DeFlockJoplin — discovered via wildcard-probe
  // + OUI signature during field testing. The 12th camera in his drive-test
  // used this prefix and wasn't in @NitekryDPaul's original 30.
  "82:6b:f2"

};
static const size_t OUI_COUNT = sizeof(target_ouis) / sizeof(target_ouis[0]);

// Pre-compiled byte table — populated once in setup(), never touched again.
// Keeps matchOuiRaw entirely in IRAM with no flash-resident function calls.
static uint8_t oui_bytes[OUI_COUNT][3];

// ============================================================
// ALERT QUEUE  (callback → loop, avoids Serial in WiFi task)
// ============================================================

#define ALERT_QUEUE_SIZE 12

typedef enum : uint8_t {
  ALERT_OUI_ADDR2       = 0,
  ALERT_OUI_ADDR1       = 1,
  ALERT_OUI_ADDR3       = 2,
  ALERT_SSID            = 3,
  // Probe Request + wildcard SSID (tag 0, length 0) from a known-OUI addr2.
  // Tight signature from Michael / DeFlockJoplin field research:
  //   https://github.com/DeflockJoplin/flock-you
  ALERT_WILDCARD_PROBE  = 4,
  ALERT_BLE_FLOCK       = 5,
  ALERT_BLE_RAVEN       = 6,
} AlertType;

typedef struct {
  AlertType type;
  uint8_t   mac[6];
  int8_t    rssi;
  uint8_t   channel;
  int16_t   txPower;
  uint8_t   confidence;
  char      ssid[33];     // populated for SSID hits
  char      deviceName[33];
  char      frameKind[12];
  char      methods[48];
  char      extra[48];
} AlertEntry;

static volatile AlertEntry alertQueue[ALERT_QUEUE_SIZE];
static volatile size_t alertHead = 0;  // written by callback
static volatile size_t alertTail = 0;  // read by loop()
static portMUX_TYPE    queueMux  = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t alertQueueDrops = 0;

static void IRAM_ATTR enqueueAlert(AlertType type, const uint8_t* mac, int8_t rssi,
                                    uint8_t ch, const char* ssid, const char* kind) {
  portENTER_CRITICAL_ISR(&queueMux);
  size_t next = (alertHead + 1) % ALERT_QUEUE_SIZE;
  if (next == alertTail) {                         // drop if full — loop() is behind
    alertQueueDrops++;
    portEXIT_CRITICAL_ISR(&queueMux);
    return;
  }

  AlertEntry* e = (AlertEntry*)&alertQueue[alertHead];
  e->type    = type;
  e->rssi    = rssi;
  e->channel = ch;
  e->txPower = 0;
  e->confidence = 0;
  memcpy((void*)e->mac, mac, 6);

  if (ssid)  { strncpy((char*)e->ssid,      ssid, 32); ((char*)e->ssid)[32] = '\0'; }
  else        { ((char*)e->ssid)[0] = '\0'; }

  if (kind)  { strncpy((char*)e->frameKind, kind, 11); ((char*)e->frameKind)[11] = '\0'; }
  else        { ((char*)e->frameKind)[0] = '\0'; }
  ((char*)e->deviceName)[0] = '\0';
  ((char*)e->methods)[0] = '\0';
  ((char*)e->extra)[0] = '\0';

  alertHead = next;
  portEXIT_CRITICAL_ISR(&queueMux);
}

static void enqueueDetectionEvent(AlertType type, const uint8_t* mac, int8_t rssi,
                                  uint8_t ch, const char* ssid, const char* kind,
                                  const char* name, const char* methods,
                                  const char* extra, int txPower, uint8_t confidence) {
  portENTER_CRITICAL(&queueMux);
  size_t next = (alertHead + 1) % ALERT_QUEUE_SIZE;
  if (next == alertTail) {
    alertQueueDrops++;
    portEXIT_CRITICAL(&queueMux);
    return;
  }

  AlertEntry* e = (AlertEntry*)&alertQueue[alertHead];
  e->type = type;
  e->rssi = rssi;
  e->channel = ch;
  e->txPower = txPower;
  e->confidence = confidence;
  memcpy((void*)e->mac, mac, 6);
  if (ssid) { strncpy((char*)e->ssid, ssid, 32); ((char*)e->ssid)[32] = '\0'; }
  else { ((char*)e->ssid)[0] = '\0'; }
  if (kind) { strncpy((char*)e->frameKind, kind, 11); ((char*)e->frameKind)[11] = '\0'; }
  else { ((char*)e->frameKind)[0] = '\0'; }
  if (name) { strncpy((char*)e->deviceName, name, 32); ((char*)e->deviceName)[32] = '\0'; }
  else { ((char*)e->deviceName)[0] = '\0'; }
  if (methods) { strncpy((char*)e->methods, methods, 47); ((char*)e->methods)[47] = '\0'; }
  else { ((char*)e->methods)[0] = '\0'; }
  if (extra) { strncpy((char*)e->extra, extra, 47); ((char*)e->extra)[47] = '\0'; }
  else { ((char*)e->extra)[0] = '\0'; }
  alertHead = next;
  portEXIT_CRITICAL(&queueMux);
}

// ============================================================
// DETECTION TABLE  (on-device storage, persisted to SPIFFS)
// ============================================================
//
// Single-threaded: only touched from loop() — drainAlertQueue() adds, and
// fySaveSession() reads. No mutex needed. The WiFi-task callback never
// touches this table; it only writes to the lock-free alert ring buffer.

typedef struct FYDetection {
  char     mac[18];
  char     method[48];
  char     protocol[16];
  char     captureType[16];
  char     deviceName[33];
  char     extra[48];
  int8_t   rssi;
  uint8_t  channel;
  int16_t  txPower;
  uint8_t  confidence;
  char     confidenceLabel[8];
  uint32_t firstSeen;      // millis() at first hit
  uint32_t lastSeen;       // millis() at latest hit
  uint16_t count;
  char     ssid[33];       // "" unless an SSID hit populated it
} FYDetection;

static FYDetection fyDet[MAX_DETECTIONS];
static int           fyDetCount       = 0;
static bool          fySpiffsReady    = false;
static bool          fyDirty          = false;
static unsigned long fyLastSaveAt     = 0;
static int           fyLastSaveCount  = 0;

// ============================================================
// STATE
// ============================================================

static uint8_t  currentChannel = 1;
static uint8_t  runtimeChannelMode = CHANNEL_MODE;
static uint8_t  runtimeSingleChannel = SINGLE_CHANNEL;
static size_t   customChannelIndex = 0;
static size_t   fullHopIndex = 0;
static unsigned long lastHop = 0;
static unsigned long lastHeartbeat = 0;
static volatile bool sniffingStopped = false;
static volatile bool sniffingPaused = false;
static bool stealthMode = false;
static bool sdReady = false;
static bool apReady = false;
static String currentLogFile = "/FlockLog_001.csv";
#if ENABLE_SD_LOGGING
static char sdStatusLine[48] = "not initialized";
static uint8_t sdCardTypeValue = CARD_NONE;
static uint64_t sdCardSizeBytes = 0;
#endif
static NimBLEScan* bleScan = nullptr;
static unsigned long lastBleScanAt = 0;
static WebServer webServer(AP_WEB_SERVER_PORT);

#if ENABLE_GPS
static TinyGPSPlus gps;
static HardwareSerial SerialGPS(1);
#endif

static unsigned long sessionStartMs = 0;
static unsigned long lastLifetimeSaveAt = 0;
static uint32_t sessionWifi = 0;
static uint32_t sessionBle = 0;
static uint32_t sessionRaven = 0;
static uint32_t lifetimeWifi = 0;
static uint32_t lifetimeBle = 0;
static uint32_t lifetimeRaven = 0;
static uint32_t lifetimeSeconds = 0;
static uint32_t activityBuckets[25] = {0};
static unsigned long lastActivityBucketAt = 0;
static uint8_t activityBucketIndex = 0;

// Dedupe table (small circular, avoids single-slot eviction bug).
// This is the *serial-rate-limit* dedup — it suppresses beep + emit within
// ALERT_COOLDOWN_MS of a prior hit on the same MAC. The detection table
// (above) still counts every hit regardless of this suppression.
#define DEDUPE_SLOTS 8
static struct {
  char mac[18];
  unsigned long ts;
} dedupeTable[DEDUPE_SLOTS];
static size_t dedupeIdx = 0;

// LED one-shot pulse timer
static volatile unsigned long ledOffAt = 0;
#if USE_RGB_LED
static Adafruit_NeoPixel rgbLed(RGB_LED_COUNT, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);
static unsigned long rgbPulseStartedAt = 0;
static unsigned long rgbPulseDurationMs = 0;
static unsigned long rgbLastHeartbeatAt = 0;
static uint8_t rgbPulseR = 0;
static uint8_t rgbPulseG = 0;
static uint8_t rgbPulseB = 0;
static bool rgbPulseActive = false;
#endif

// Heartbeat audio state: last time any target was seen, last time the
// heartbeat beep-pair was played. When nothing has been seen for
// HB_DEVICE_ACTIVE_MS the heartbeat stops until the next new detection.
static unsigned long fyLastTargetSeen  = 0;
static unsigned long fyLastHeartbeatAt = 0;

static char uiLastMac[18] = "";
static char uiLastMethod[24] = "";
static char uiLastName[33] = "";
static char uiLastProtocol[16] = "";
static char uiLastConfidenceLabel[8] = "LOW";
static int8_t uiLastRssi = 0;
static uint8_t uiLastChannel = 0;
static uint8_t uiLastConfidence = 0;
static char uiLiveFeed[5][48] = {{0}};
static bool uiDisplayReady = false;
static bool uiTouchReady = false;
static bool uiBuzzerMuted = false;
static unsigned long uiLastRefreshAt = 0;
static uint8_t uiPage = 0;
static bool uiMenuMode = false;
static uint8_t uiMenuIndex = 0;
static const uint8_t uiMenuItems = 3;
static unsigned long uiStatusUntilMs = 0;
static char uiStatusLine[32] = "";

static const char* uiMenuLabel() {
  if (uiMenuIndex == 0) return "channel mode";
  if (uiMenuIndex == 1) return "buzzer";
  return "launcher";
}

static const char* uiMenuShortLabel() {
  if (uiMenuIndex == 0) return "CH";
  if (uiMenuIndex == 1) return "BUZZ";
  return "HOME";
}

#if USE_AMOLED_DISPLAY
static bool amoledNeedsFullRedraw = true;
static uint8_t amoledLastPage = 255;
static bool amoledLastMenuMode = false;
static uint8_t amoledLastMenuIndex = 255;
static bool amoledLastStealthMode = false;
static Arduino_DataBus* amoledBus = nullptr;
static Arduino_SH8601* amoled = nullptr;
static Adafruit_XCA9554 amoledExpander;
static bool amoledExpanderReady = false;
static std::shared_ptr<Arduino_IIC_DriveBus> touchBus;
static std::unique_ptr<Arduino_FT3x68> touchDevice;
static bool touchActive = false;
static bool touchHoldSent = false;
static unsigned long touchDownAt = 0;
static unsigned long touchLastPollAt = 0;
static int16_t touchDownX = -1;
static int16_t touchDownY = -1;
static int16_t touchLastX = -1;
static int16_t touchLastY = -1;
#elif USE_CARDPUTER_DISPLAY
static bool cardputerUiDirty = true;
#else
static Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, OLED_RESET);
static U8G2_FOR_ADAFRUIT_GFX uiFonts;
#endif

#if ENABLE_POWER_STATUS
static XPowersPMU powerPmu;
static bool powerReady = false;
static bool powerBatteryPresent = false;
static bool powerCharging = false;
static bool powerVbusPresent = false;
static int powerBatteryPercent = -1;
static uint16_t powerBatteryMv = 0;
static uint16_t powerVbusMv = 0;
static uint16_t powerSystemMv = 0;
static unsigned long powerLastReadAt = 0;
#endif

static char serialCmdBuf[96] = "";
static uint8_t serialCmdLen = 0;
static bool serialSawCr = false;

typedef struct ButtonState {
  int8_t pin;
  bool pressed;
  bool raw;
  unsigned long changedAt;
  unsigned long pressedAt;
} ButtonState;

static ButtonState btnUp = { BTN_UP_PIN, false, false, 0, 0 };
static ButtonState btnDown = { BTN_DOWN_PIN, false, false, 0, 0 };
static ButtonState btnSelect = { BTN_SELECT_PIN, false, false, 0, 0 };

// ============================================================
// 802.11 HEADER
// ============================================================

typedef struct __attribute__((packed)) {
  uint16_t frame_ctrl;
  uint16_t duration;
  uint8_t  addr1[6];
  uint8_t  addr2[6];
  uint8_t  addr3[6];
  uint16_t seq_ctrl;
} wifi_ieee80211_mac_hdr_t;

// ============================================================
// HELPERS
// ============================================================

// Dual-output: prints to both Serial (USB) and Serial1 (GPIO43)
static char _dualBuf[384];

static void dualPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void dualPrintf(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(_dualBuf, sizeof(_dualBuf), fmt, args);
  va_end(args);
  if (n > 0) {
    Serial.write(_dualBuf, n);
#if MIRROR_SERIAL
    Serial1.write(_dualBuf, n);
#endif
  }
}

static void dualPrintln(const char* str) {
  Serial.println(str);
#if MIRROR_SERIAL
  Serial1.println(str);
#endif
}

static inline void ledSet(bool on) {
#if USE_LED
#if USE_RGB_LED
  rgbLed.setPixelColor(0, on ? rgbLed.Color(RGB_RED_R, RGB_RED_G, RGB_RED_B) : 0);
  rgbLed.show();
#else
#if LED_ACTIVE_HIGH
  digitalWrite(LED_PIN, on ? HIGH : LOW);
#else
  digitalWrite(LED_PIN, on ? LOW  : HIGH);
#endif
#endif
#endif
}

#if USE_RGB_LED
static void rgbSetColor(uint8_t r, uint8_t g, uint8_t b) {
  rgbLed.setPixelColor(0, rgbLed.Color(r, g, b));
  rgbLed.show();
}

static void rgbStartPulse(uint8_t r, uint8_t g, uint8_t b, unsigned ms) {
  rgbPulseR = r;
  rgbPulseG = g;
  rgbPulseB = b;
  rgbPulseDurationMs = ms;
  rgbPulseStartedAt = millis();
  rgbPulseActive = true;
}
#endif

static void ledFlash(unsigned ms) {
#if USE_LED
#if USE_RGB_LED
  rgbStartPulse(RGB_RED_R, RGB_RED_G, RGB_RED_B, ms);
  rgbSetColor(RGB_RED_R, RGB_RED_G, RGB_RED_B);
  ledOffAt = millis() + ms;
  if (ledOffAt == 0) ledOffAt = 1;  // avoid the "off" sentinel
#else
  ledSet(true);
  ledOffAt = millis() + ms;
  if (ledOffAt == 0) ledOffAt = 1;  // avoid the "off" sentinel
#endif
#endif
}

static void ledBootPulse(unsigned ms) {
#if USE_LED
#if USE_RGB_LED
  rgbStartPulse(RGB_GREEN_R, RGB_GREEN_G, RGB_GREEN_B, ms);
  rgbSetColor(RGB_GREEN_R, RGB_GREEN_G, RGB_GREEN_B);
#else
  ledFlash(ms);
#endif
#endif
}

static void ledTick() {
#if USE_LED
#if USE_RGB_LED
  unsigned long now = millis();
  if (rgbPulseActive) {
    unsigned long elapsed = now - rgbPulseStartedAt;
    if (elapsed >= rgbPulseDurationMs) {
      rgbPulseActive = false;
      ledOffAt = 0;
      rgbSetColor(0, 0, 0);
    } else {
      unsigned long half = rgbPulseDurationMs / 2;
      if (half == 0) half = 1;
      unsigned long phase = (elapsed <= half) ? elapsed : (rgbPulseDurationMs - elapsed);
      uint8_t level = (uint8_t)((phase * 255UL) / half);
      rgbSetColor((uint8_t)((rgbPulseR * level) / 255),
                  (uint8_t)((rgbPulseG * level) / 255),
                  (uint8_t)((rgbPulseB * level) / 255));
    }
    return;
  }

  if (now - rgbLastHeartbeatAt >= RGB_HEARTBEAT_MS) {
    rgbLastHeartbeatAt = now;
    rgbStartPulse(RGB_GREEN_R, RGB_GREEN_G, RGB_GREEN_B, RGB_PULSE_MS);
  }
#else
  if (ledOffAt && (long)(millis() - ledOffAt) >= 0) {
    ledSet(false);
    ledOffAt = 0;
  }
#endif
#endif
}

static void buzzerBeep(unsigned int ms) {
#if USE_BUZZER
  if (uiBuzzerMuted || stealthMode) return;
  digitalWrite(BUZZER_PIN, HIGH); delay(ms); digitalWrite(BUZZER_PIN, LOW);
#endif
}

// Two fast ascending beeps — played on the FIRST sighting of a MAC.
static void newDetectChirp() {
#if USE_BUZZER
  if (uiBuzzerMuted || stealthMode) return;
  tone(BUZZER_PIN, NEW_CHIRP_LO_HZ); delay(NEW_CHIRP_NOTE_MS); noTone(BUZZER_PIN);
  delay(NEW_CHIRP_GAP_MS);
  tone(BUZZER_PIN, NEW_CHIRP_HI_HZ); delay(NEW_CHIRP_NOTE_MS); noTone(BUZZER_PIN);
#endif
}

// Two monotone beeps — periodic heartbeat while at least one target is still
// in range (last seen within HB_DEVICE_ACTIVE_MS).
static void heartbeatBeep() {
#if USE_BUZZER
  if (uiBuzzerMuted || stealthMode) return;
  tone(BUZZER_PIN, HB_BEEP_HZ); delay(HB_BEEP_NOTE_MS); noTone(BUZZER_PIN);
  delay(HB_BEEP_GAP_MS);
  tone(BUZZER_PIN, HB_BEEP_HZ); delay(HB_BEEP_NOTE_MS); noTone(BUZZER_PIN);
#endif
}

static void alarmEscalation(uint8_t confidence) {
#if USE_BUZZER
  if (uiBuzzerMuted || stealthMode) return;
  uint8_t beeps = confidence >= CONFIDENCE_CERTAIN ? 5 : (confidence >= CONFIDENCE_HIGH ? 3 : 1);
  uint16_t freq = confidence >= CONFIDENCE_CERTAIN ? 1500 : (confidence >= CONFIDENCE_HIGH ? 1200 : 1000);
  for (uint8_t i = 0; i < beeps; i++) {
    tone(BUZZER_PIN, freq);
    delay(confidence >= CONFIDENCE_CERTAIN ? 70 : 120);
    noTone(BUZZER_PIN);
    delay(55);
  }
#endif
}
static void startupBeep() {
#if USE_BUZZER
  if (uiBuzzerMuted || stealthMode) return;
  // First 6 notes of SMB World 1-2 (underground). Koji Kondo's descending
  // pattern: C5 → C4 → A4 → A3 → G#4 → G#3 (alternating-octave pairs).
  static const uint16_t notes[6] = { 523, 262, 440, 220, 415, 208 };
  for (int i = 0; i < 6; i++) {
    tone(BUZZER_PIN, notes[i]);
    delay((i == 5) ? 160 : 95);
    noTone(BUZZER_PIN);
    if (i < 5) delay(22);
  }
#endif
}

static void macToStr(const uint8_t* mac, char* buf, size_t len) {
  snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
static void ouiFromMac(const uint8_t* mac, char* buf, size_t len) {
  snprintf(buf, len, "%02x:%02x:%02x", mac[0], mac[1], mac[2]);
}

static void precompileOuis() {
  for (size_t i = 0; i < OUI_COUNT; i++) {
    const char* o  = target_ouis[i];
    oui_bytes[i][0] = (uint8_t)strtol(o,     nullptr, 16);
    oui_bytes[i][1] = (uint8_t)strtol(o + 3, nullptr, 16);
    oui_bytes[i][2] = (uint8_t)strtol(o + 6, nullptr, 16);
  }
}

// Bit 0 of byte 0 set = multicast/broadcast — never a real device transmitter or receiver
// we care about. Guards addr1 checks against 01:xx, 33:33:xx, ff:ff:ff:ff:ff:ff etc.
static inline bool IRAM_ATTR isMulticast(const uint8_t* mac) {
  return mac[0] & 0x01;
}

static bool IRAM_ATTR matchOuiRaw(const uint8_t* mac) {
  // Locally-administered (randomised) MACs have bit 1 of byte 0 set.
  // Fixed infrastructure devices never use them — skip immediately.
  if (mac[0] & 0x02) return false;

  for (size_t i = 0; i < OUI_COUNT; i++) {
    if (mac[0] == oui_bytes[i][0] &&
        mac[1] == oui_bytes[i][1] &&
        mac[2] == oui_bytes[i][2]) return true;
  }
  return false;
}

static char* strcasestr_local(const char* haystack, const char* needle) {
  if (!*needle) return (char*)haystack;
  for (; *haystack; ++haystack) {
    const char* h = haystack; const char* n = needle;
    while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { ++h; ++n; }
    if (!*n) return (char*)haystack;
  }
  return nullptr;
}
static bool matchSsidKeyword(const char* ssid) {
  for (size_t i = 0; i < SSID_KEYWORD_COUNT; i++)
    if (strcasestr_local(ssid, target_ssid_keywords[i])) return true;
  return false;
}

static const char* channelModeName() {
  switch (runtimeChannelMode) {
    case CHANNEL_MODE_FULL_HOP: return "FULL_HOP";
    case CHANNEL_MODE_CUSTOM:   return "CUSTOM";
    case CHANNEL_MODE_SINGLE:   return "SINGLE";
    default:                    return "UNKNOWN";
  }
}

static inline uint16_t channelFreqMhz(uint8_t ch) {
  return (ch >= 1 && ch <= 14) ? (uint16_t)(2407 + 5 * ch) : 0;
}

static const char* confidenceLabel(uint8_t score) {
  if (score >= CONFIDENCE_CERTAIN) return "CERTAIN";
  if (score >= CONFIDENCE_HIGH) return "HIGH";
  if (score >= CONFIDENCE_ALARM_THRESHOLD) return "MEDIUM";
  return "LOW";
}

static bool isFlockSsidFormat(const char* ssid) {
  if (!ssid) return false;
  if (strncasecmp(ssid, "Flock-", 6) != 0) return false;
  const char* suffix = ssid + 6;
  int len = strlen(suffix);
  if (len < 2 || len > 12) return false;
  for (int i = 0; i < len; i++) {
    if (!isxdigit((unsigned char)suffix[i])) return false;
  }
  return true;
}

static uint16_t dwellForChannel(uint8_t ch) {
  return (ch == 1 || ch == 6 || ch == 11) ? CHANNEL_DWELL_PRIMARY_MS : CHANNEL_DWELL_SECONDARY_MS;
}

#define RSSI_TRACK_MAX_DEVICES 16
#define RSSI_TRACK_SAMPLES 5
#define RSSI_TRACK_EXPIRY_MS 15000

typedef struct {
  char mac[18];
  int8_t samples[RSSI_TRACK_SAMPLES];
  uint8_t sampleCount;
  unsigned long lastSeen;
  bool scored;
} RssiTrack;

static RssiTrack rssiTracks[RSSI_TRACK_MAX_DEVICES];
static uint8_t rssiTrackCount = 0;

static void rssiTrackUpdate(const char* mac, int8_t rssi) {
  unsigned long now = millis();
  for (uint8_t i = 0; i < rssiTrackCount; i++) {
    if (strcasecmp(rssiTracks[i].mac, mac) == 0) {
      if (rssiTracks[i].sampleCount < RSSI_TRACK_SAMPLES) {
        rssiTracks[i].samples[rssiTracks[i].sampleCount++] = rssi;
      } else {
        for (uint8_t j = 0; j < RSSI_TRACK_SAMPLES - 1; j++) {
          rssiTracks[i].samples[j] = rssiTracks[i].samples[j + 1];
        }
        rssiTracks[i].samples[RSSI_TRACK_SAMPLES - 1] = rssi;
      }
      rssiTracks[i].lastSeen = now;
      return;
    }
  }

  uint8_t idx = rssiTrackCount;
  if (idx >= RSSI_TRACK_MAX_DEVICES) {
    idx = 0;
    for (uint8_t i = 1; i < RSSI_TRACK_MAX_DEVICES; i++) {
      if (rssiTracks[i].lastSeen < rssiTracks[idx].lastSeen) idx = i;
    }
  } else {
    rssiTrackCount++;
  }
  strlcpy(rssiTracks[idx].mac, mac, sizeof(rssiTracks[idx].mac));
  rssiTracks[idx].samples[0] = rssi;
  rssiTracks[idx].sampleCount = 1;
  rssiTracks[idx].lastSeen = now;
  rssiTracks[idx].scored = false;
}

static bool rssiTrackStationaryBonus(const char* mac) {
  for (uint8_t i = 0; i < rssiTrackCount; i++) {
    if (strcasecmp(rssiTracks[i].mac, mac) == 0 && rssiTracks[i].sampleCount >= 3 && !rssiTracks[i].scored) {
      uint8_t peak = 0;
      for (uint8_t j = 1; j < rssiTracks[i].sampleCount; j++) {
        if (rssiTracks[i].samples[j] > rssiTracks[i].samples[peak]) peak = j;
      }
      int range = rssiTracks[i].samples[peak] - min(rssiTracks[i].samples[0], rssiTracks[i].samples[rssiTracks[i].sampleCount - 1]);
      if (peak > 0 && peak < rssiTracks[i].sampleCount - 1 && range >= 6) {
        rssiTracks[i].scored = true;
        return true;
      }
      return false;
    }
  }
  return false;
}

static void rssiTrackExpire() {
  unsigned long now = millis();
  for (int i = rssiTrackCount - 1; i >= 0; i--) {
    if ((now - rssiTracks[i].lastSeen) > RSSI_TRACK_EXPIRY_MS) {
      for (uint8_t j = i; j + 1 < rssiTrackCount; j++) rssiTracks[j] = rssiTracks[j + 1];
      rssiTrackCount--;
    }
  }
}

static bool shouldSuppressDuplicate(const char* macStr) {
  unsigned long now = millis();
  for (size_t i = 0; i < DEDUPE_SLOTS; i++) {
    if (strcmp(dedupeTable[i].mac, macStr) == 0) {
      if ((now - dedupeTable[i].ts) < ALERT_COOLDOWN_MS) return true;
      dedupeTable[i].ts = now;
      return false;
    }
  }
  // Not found — insert into next slot
  strlcpy(dedupeTable[dedupeIdx].mac, macStr, 18);
  dedupeTable[dedupeIdx].ts = now;
  dedupeIdx = (dedupeIdx + 1) % DEDUPE_SLOTS;
  return false;
}

static void stopSniffing(const char* reason) {
  if (sniffingStopped) return;
  sniffingStopped = true;
  esp_wifi_set_promiscuous(false);
  dualPrintf("[cypher-flock] sniffing stopped: %s\n", reason);
}

static void setSniffPaused(bool paused) {
  if (sniffingStopped) return;
  if (sniffingPaused == paused) return;
  sniffingPaused = paused;
  esp_wifi_set_promiscuous(paused ? false : true);
  dualPrintf("[cypher-flock] sniffing %s\n", paused ? "paused" : "resumed");
  strlcpy(uiStatusLine, paused ? "sniffing paused" : "sniffing resumed", sizeof(uiStatusLine));
  uiStatusUntilMs = millis() + 1500;
}

static void applyInitialChannel() {
  if (runtimeChannelMode == CHANNEL_MODE_SINGLE) {
    currentChannel = runtimeSingleChannel;
  } else if (runtimeChannelMode == CHANNEL_MODE_CUSTOM) {
    customChannelIndex = 0;
    currentChannel = customChannels[0];
  } else {
    fullHopIndex = 0;
    currentChannel = fullHopChannels[0];
  }
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
  lastHop = millis();  // start dwell timer precisely when channel is first set
}

static void updateChannelMode() {
  if (sniffingStopped || sniffingPaused) return;
  if (runtimeChannelMode == CHANNEL_MODE_SINGLE) {
    if (currentChannel != runtimeSingleChannel) {
      currentChannel = runtimeSingleChannel;
      esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    }
    return;
  }
  if (millis() - lastHop < dwellForChannel(currentChannel)) return;
  if (runtimeChannelMode == CHANNEL_MODE_CUSTOM) {
    customChannelIndex = (customChannelIndex + 1) % customChannelCount;
    currentChannel = customChannels[customChannelIndex];
  } else {
    fullHopIndex = (fullHopIndex + 1) % fullHopChannelCount;
    currentChannel = fullHopChannels[fullHopIndex];
  }
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
  lastHop = millis();
}

static void printHeartbeat() {
  if (millis() - lastHeartbeat >= HEARTBEAT_MS) {
    dualPrintf("[cypher-flock] scanning (ch=%u mode=%s det=%d)\n",
                  currentChannel, channelModeName(), fyDetCount);
    lastHeartbeat = millis();
  }
}

// ============================================================
// DETECTION TABLE OPS
// ============================================================

static const char* alertTypeToMethod(AlertType t) {
  switch (t) {
    case ALERT_OUI_ADDR2:      return "oui_addr2";
    case ALERT_OUI_ADDR1:      return "oui_addr1";
    case ALERT_OUI_ADDR3:      return "oui_addr3";
    case ALERT_SSID:           return "ssid";
    case ALERT_WILDCARD_PROBE: return "wildcard_probe";
    case ALERT_BLE_FLOCK:      return "ble_flock";
    case ALERT_BLE_RAVEN:      return "ble_raven";
    default:                   return "unknown";
  }
}

// Returns index of entry (new or updated), or -1 if table is full.
// Returns index, and sets *outChirpWorthy = true when the caller should fire
// the ascending new-discovery chirp. Chirp-worthy means either (a) MAC is
// brand new to this session, or (b) MAC is known but hasn't been seen in
// REDISCOVER_MS — i.e. it left RF range and came back.
static int fyAddDetection(const char* mac, const char* method,
                          const char* protocol, const char* captureType,
                          const char* deviceName, const char* extra,
                          int8_t rssi, uint8_t ch, int16_t txPower,
                          uint8_t confidence, const char* label, const char* ssid,
                          bool* outChirpWorthy) {
  uint32_t now = millis();
  for (int i = 0; i < fyDetCount; i++) {
    if (strcasecmp(fyDet[i].mac, mac) == 0) {
      bool rediscover = (now - fyDet[i].lastSeen) > REDISCOVER_MS;
      if (fyDet[i].count < 0xFFFF) fyDet[i].count++;
      fyDet[i].lastSeen = now;
      fyDet[i].rssi     = rssi;
      fyDet[i].channel  = ch;
      fyDet[i].txPower  = txPower;
      if (confidence >= fyDet[i].confidence) {
        fyDet[i].confidence = confidence;
        strlcpy(fyDet[i].confidenceLabel, label ? label : confidenceLabel(confidence), sizeof(fyDet[i].confidenceLabel));
      }
      if (method && method[0]) strlcpy(fyDet[i].method, method, sizeof(fyDet[i].method));
      if (protocol && protocol[0]) strlcpy(fyDet[i].protocol, protocol, sizeof(fyDet[i].protocol));
      if (captureType && captureType[0]) strlcpy(fyDet[i].captureType, captureType, sizeof(fyDet[i].captureType));
      if (deviceName && deviceName[0]) strlcpy(fyDet[i].deviceName, deviceName, sizeof(fyDet[i].deviceName));
      if (extra && extra[0]) strlcpy(fyDet[i].extra, extra, sizeof(fyDet[i].extra));
      if (ssid && ssid[0] && !fyDet[i].ssid[0]) {
        strlcpy(fyDet[i].ssid, ssid, sizeof(fyDet[i].ssid));
      }
      fyDirty = true;
      if (outChirpWorthy) *outChirpWorthy = rediscover;
      return i;
    }
  }
  if (fyDetCount >= MAX_DETECTIONS) {
    if (outChirpWorthy) *outChirpWorthy = false;
    return -1;
  }
  FYDetection& d = fyDet[fyDetCount];
  strlcpy(d.mac,    mac,                       sizeof(d.mac));
  strlcpy(d.method, method ? method : "",      sizeof(d.method));
  strlcpy(d.protocol, protocol ? protocol : "", sizeof(d.protocol));
  strlcpy(d.captureType, captureType ? captureType : "", sizeof(d.captureType));
  strlcpy(d.deviceName, deviceName ? deviceName : "", sizeof(d.deviceName));
  strlcpy(d.extra, extra ? extra : "", sizeof(d.extra));
  d.rssi      = rssi;
  d.channel   = ch;
  d.txPower   = txPower;
  d.confidence = confidence;
  strlcpy(d.confidenceLabel, label ? label : confidenceLabel(confidence), sizeof(d.confidenceLabel));
  d.firstSeen = now;
  d.lastSeen  = now;
  d.count     = 1;
  if (ssid && ssid[0]) strlcpy(d.ssid, ssid, sizeof(d.ssid));
  else                 d.ssid[0] = '\0';
  fyDetCount++;
  fyDirty = true;
  if (outChirpWorthy) *outChirpWorthy = true;
  return fyDetCount - 1;
}

// ============================================================
// JSON ESCAPE  — only needed for SSIDs (user-controlled bytes)
// ============================================================

static size_t jsonEscape(char* dst, size_t cap, const char* src) {
  size_t o = 0;
  if (cap == 0) return 0;
  for (size_t i = 0; src[i]; i++) {
    char c = src[i];
    if (c == '"' || c == '\\') {
      if (o + 2 >= cap) break;
      dst[o++] = '\\'; dst[o++] = c;
    } else if ((unsigned char)c < 0x20) {
      if (o + 6 >= cap) break;
      int n = snprintf(dst + o, cap - o, "\\u%04x", (unsigned)(unsigned char)c);
      if (n <= 0 || (size_t)n >= cap - o) break;
      o += (size_t)n;
    } else {
      if (o + 1 >= cap) break;
      dst[o++] = c;
    }
  }
  dst[o] = '\0';
  return o;
}

// ============================================================
// CRC32  (zlib / SPIFFS-tool compatible polynomial 0xEDB88320)
// ============================================================

static uint32_t fyCRC32Update(uint32_t crc, const uint8_t* data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320u & -(int32_t)(crc & 1));
  }
  return ~crc;
}

// ============================================================
// SPIFFS SESSION PERSISTENCE  — bulletproof envelope format
// ============================================================
//
// Wire format on disk:
//   Line 1: {"v":1,"count":N,"bytes":B,"crc":"0xXXXXXXXX"}\n
//   Line 2+: [{"mac":...},...]     (exactly B bytes, CRC32 == X)
//
// Atomic write procedure:
//   1. Compute payload size + CRC (pass 1)
//   2. Write envelope + payload to /session.tmp (pass 2)
//   3. Re-validate /session.tmp from disk
//   4. Remove /session.json, rename tmp → main (with copy+delete fallback)
//
// Boot-time recovery:
//   - Try /session.json. If missing or CRC-invalid, try /session.tmp.
//   - Copy whichever validates to /prev_session.json, then delete both.

static size_t fySerializeDet(const FYDetection& d, char* dst, size_t cap) {
  char ssidEsc[sizeof(d.ssid) * 6 + 1];
  char nameEsc[sizeof(d.deviceName) * 6 + 1];
  char extraEsc[sizeof(d.extra) * 6 + 1];
  jsonEscape(ssidEsc, sizeof(ssidEsc), d.ssid);
  jsonEscape(nameEsc, sizeof(nameEsc), d.deviceName);
  jsonEscape(extraEsc, sizeof(extraEsc), d.extra);
  int n = snprintf(dst, cap,
      "{\"mac\":\"%s\",\"method\":\"%s\",\"protocol\":\"%s\",\"capture\":\"%s\","
      "\"name\":\"%s\",\"rssi\":%d,\"channel\":%u,\"tx_power\":%d,"
      "\"confidence\":%u,\"label\":\"%s\",\"first\":%lu,\"last\":%lu,"
      "\"count\":%u,\"ssid\":\"%s\",\"extra\":\"%s\"}",
      d.mac, d.method, d.protocol, d.captureType, nameEsc, d.rssi, (unsigned)d.channel,
      (int)d.txPower, (unsigned)d.confidence, d.confidenceLabel,
      (unsigned long)d.firstSeen, (unsigned long)d.lastSeen, (unsigned)d.count,
      ssidEsc, extraEsc);
  return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

static uint32_t fyComputePayloadCRC(size_t& outBytes) {
  char line[384];
  uint32_t crc = 0;
  outBytes = 0;
  crc = fyCRC32Update(crc, (const uint8_t*)"[", 1); outBytes += 1;
  for (int i = 0; i < fyDetCount; i++) {
    if (i > 0) { crc = fyCRC32Update(crc, (const uint8_t*)",", 1); outBytes += 1; }
    size_t n = fySerializeDet(fyDet[i], line, sizeof(line));
    if (n == 0) continue;
    crc = fyCRC32Update(crc, (const uint8_t*)line, n);
    outBytes += n;
  }
  crc = fyCRC32Update(crc, (const uint8_t*)"]", 1); outBytes += 1;
  return crc;
}

// Minimal envelope parser: pulls bytes + crc fields by substring search.
// Robust to field reordering; rejects anything without both required keys.
static bool fyParseEnvelope(const char* hdr, size_t& outBytes, uint32_t& outCrc) {
  const char* b = strstr(hdr, "\"bytes\":");
  const char* c = strstr(hdr, "\"crc\":\"0x");
  if (!b || !c) return false;
  b += 8;
  long long bv = 0;
  if (sscanf(b, "%lld", &bv) != 1 || bv < 0) return false;
  c += 9;
  unsigned cv = 0;
  if (sscanf(c, "%x", &cv) != 1) return false;
  outBytes = (size_t)bv;
  outCrc   = (uint32_t)cv;
  return true;
}

static bool fyValidateSessionFile(const char* path) {
  if (!SPIFFS.exists(path)) return false;
  File f = SPIFFS.open(path, "r");
  if (!f) return false;

  String hdr = f.readStringUntil('\n');
  if (hdr.length() < 10 || hdr[0] != '{') { f.close(); return false; }

  size_t   expectedBytes = 0;
  uint32_t expectedCRC   = 0;
  if (!fyParseEnvelope(hdr.c_str(), expectedBytes, expectedCRC)) {
    f.close(); return false;
  }

  size_t bodyOffset = hdr.length() + 1;
  size_t fileSize   = f.size();
  if (fileSize < bodyOffset + expectedBytes) { f.close(); return false; }
  if ((fileSize - bodyOffset) != expectedBytes) { f.close(); return false; }

  uint8_t buf[256];
  uint32_t crc = 0;
  size_t remaining = expectedBytes;
  while (remaining > 0) {
    int n = f.read(buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
    if (n <= 0) break;
    crc = fyCRC32Update(crc, buf, (size_t)n);
    remaining -= (size_t)n;
  }
  f.close();
  return (remaining == 0 && crc == expectedCRC);
}

static bool fySpiffsCopy(const char* src, const char* dst) {
  File s = SPIFFS.open(src, "r");
  if (!s) return false;
  File d = SPIFFS.open(dst, "w");
  if (!d) { s.close(); return false; }
  uint8_t buf[256];
  int n;
  bool ok = true;
  while ((n = s.read(buf, sizeof(buf))) > 0) {
    if (d.write(buf, (size_t)n) != (size_t)n) { ok = false; break; }
  }
  s.close();
  d.close();
  return ok;
}

static bool fyAtomicPromote(const char* src, const char* dst) {
  if (SPIFFS.rename(src, dst)) return true;
  if (!fySpiffsCopy(src, dst)) return false;
  SPIFFS.remove(src);
  return true;
}

static void fySaveSession() {
  if (!fySpiffsReady) return;
  if (!fyDirty && fyDetCount == fyLastSaveCount) return;

  size_t   payloadBytes = 0;
  uint32_t crc          = fyComputePayloadCRC(payloadBytes);
  int      savedCount   = fyDetCount;

  File f = SPIFFS.open(FY_SESSION_TMP, "w");
  if (!f) {
    dualPrintf("[cypher-flock] save failed: cannot open %s\n", FY_SESSION_TMP);
    return;
  }
  f.printf("{\"v\":1,\"count\":%d,\"bytes\":%u,\"crc\":\"0x%08lX\"}\n",
           savedCount, (unsigned)payloadBytes, (unsigned long)crc);

  char line[384];
  size_t wrote = 0;
  f.write((uint8_t*)"[", 1); wrote++;
  for (int i = 0; i < fyDetCount; i++) {
    if (i > 0) { f.write((uint8_t*)",", 1); wrote++; }
    size_t n = fySerializeDet(fyDet[i], line, sizeof(line));
    if (n == 0) continue;
    f.write((uint8_t*)line, n);
    wrote += n;
  }
  f.write((uint8_t*)"]", 1); wrote++;
  f.close();

  if (wrote != payloadBytes) {
    dualPrintf("[cypher-flock] save WARNING: wrote %u expected %u — aborting\n",
               (unsigned)wrote, (unsigned)payloadBytes);
    return;
  }

  if (!fyValidateSessionFile(FY_SESSION_TMP)) {
    dualPrintf("[cypher-flock] save verify FAILED — old session preserved\n");
    return;
  }

  SPIFFS.remove(FY_SESSION_FILE);
  if (!fyAtomicPromote(FY_SESSION_TMP, FY_SESSION_FILE)) {
    dualPrintf("[cypher-flock] promote FAILED — data in %s for recovery\n", FY_SESSION_TMP);
    return;
  }

  fyLastSaveAt    = millis();
  fyLastSaveCount = savedCount;
  fyDirty         = false;
  dualPrintf("[cypher-flock] session saved: %d det, %u bytes, crc=0x%08lX\n",
             savedCount, (unsigned)payloadBytes, (unsigned long)crc);
}

// Promote any valid session file from last boot into /prev_session.json, then
// start this boot with a fresh empty table. Preserves history across power cycles.
static void fyPromotePrevSession() {
  if (!fySpiffsReady) return;

  const char* source = nullptr;
  if      (fyValidateSessionFile(FY_SESSION_FILE)) source = FY_SESSION_FILE;
  else if (fyValidateSessionFile(FY_SESSION_TMP))  source = FY_SESSION_TMP;

  if (!source) {
    if (SPIFFS.exists(FY_SESSION_FILE)) SPIFFS.remove(FY_SESSION_FILE);
    if (SPIFFS.exists(FY_SESSION_TMP))  SPIFFS.remove(FY_SESSION_TMP);
    dualPrintln("[cypher-flock] no valid prior session to promote");
    return;
  }

  if (!fySpiffsCopy(source, FY_PREV_FILE)) {
    dualPrintf("[cypher-flock] failed to promote %s -> %s\n", source, FY_PREV_FILE);
    return;
  }
  if (SPIFFS.exists(FY_SESSION_FILE)) SPIFFS.remove(FY_SESSION_FILE);
  if (SPIFFS.exists(FY_SESSION_TMP))  SPIFFS.remove(FY_SESSION_TMP);

  File v = SPIFFS.open(FY_PREV_FILE, "r");
  size_t sz = v ? v.size() : 0;
  if (v) v.close();
  dualPrintf("[cypher-flock] prior session promoted from %s (%u bytes)\n",
             source, (unsigned)sz);
}

static void lifetimeLoad() {
  if (!fySpiffsReady || !SPIFFS.exists(FY_LIFETIME_FILE)) return;
  File f = SPIFFS.open(FY_LIFETIME_FILE, "r");
  if (!f) return;
  lifetimeWifi = f.readStringUntil('\n').toInt();
  lifetimeBle = f.readStringUntil('\n').toInt();
  lifetimeRaven = f.readStringUntil('\n').toInt();
  lifetimeSeconds = f.readStringUntil('\n').toInt();
  f.close();
  dualPrintf("[cypher-flock] lifetime restored: WiFi=%lu BLE=%lu Raven=%lu Time=%lu\n",
             (unsigned long)lifetimeWifi, (unsigned long)lifetimeBle,
             (unsigned long)lifetimeRaven, (unsigned long)lifetimeSeconds);
}

static void lifetimeSave() {
  if (!fySpiffsReady) return;
  File f = SPIFFS.open(FY_LIFETIME_FILE, "w");
  if (!f) return;
  lifetimeSeconds += (millis() - sessionStartMs) / 1000;
  sessionStartMs = millis();
  f.printf("%lu\n%lu\n%lu\n%lu\n",
           (unsigned long)(lifetimeWifi + sessionWifi),
           (unsigned long)(lifetimeBle + sessionBle),
           (unsigned long)(lifetimeRaven + sessionRaven),
           (unsigned long)lifetimeSeconds);
  f.close();
  lastLifetimeSaveAt = millis();
}

#if ENABLE_SD_LOGGING
static const char* sdCardTypeName(uint8_t cardType) {
  switch (cardType) {
    case CARD_MMC: return "MMC";
    case CARD_SD: return "SDSC";
    case CARD_SDHC: return "SDHC";
    default: return "NONE";
  }
}

static void sdSetStatus(const char* status) {
  strlcpy(sdStatusLine, status, sizeof(sdStatusLine));
}

#if USE_SD_MMC
static void sdMmcPreparePins() {
  pinMode(SDMMC_CMD_PIN, INPUT_PULLUP);
  pinMode(SDMMC_D0_PIN, INPUT_PULLUP);
  pinMode(SDMMC_CLK_PIN, INPUT_PULLUP);
}

#if USE_AMOLED_DISPLAY
static void sdMmcPrepareBoardPower() {
  if (!amoledExpanderReady) return;
  amoledExpander.pinMode(7, OUTPUT);
  amoledExpander.digitalWrite(7, HIGH);
  delay(200);
  dualPrintln("[cypher-flock] SD_MMC expander enable high");
}
#endif

static bool sdMmcMountAtFrequency(int frequency, const char* label) {
  sdMmcPreparePins();
  if (!SD_MMC.setPins(SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN)) {
    sdSetStatus("pin setup failed");
    dualPrintf("[cypher-flock] SD_MMC pin setup failed (clk=%d cmd=%d d0=%d)\n",
               SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN);
    return false;
  }
  if (SD_MMC.begin("/sdcard", true, false, frequency)) {
    snprintf(sdStatusLine, sizeof(sdStatusLine), "mounted %s", label);
    dualPrintf("[cypher-flock] SD_MMC mounted at %s\n", label);
    return true;
  }
  dualPrintf("[cypher-flock] SD_MMC mount failed at %s\n", label);
  SD_MMC.end();
  return false;
}
#endif
#endif

static void storageInit() {
#if ENABLE_SD_LOGGING
  sdReady = false;
  sdCardTypeValue = CARD_NONE;
  sdCardSizeBytes = 0;
  currentLogFile = "";
  sdSetStatus("not initialized");
#if USE_SD_MMC
#if USE_AMOLED_DISPLAY
  sdMmcPrepareBoardPower();
#endif
  if (!sdMmcMountAtFrequency(SDMMC_FREQ_HIGHSPEED, "40MHz") &&
      !sdMmcMountAtFrequency(SDMMC_FREQ_DEFAULT, "20MHz") &&
      !sdMmcMountAtFrequency(10000, "10MHz") &&
      !sdMmcMountAtFrequency(SDMMC_FREQ_PROBING, "400kHz")) {
    sdSetStatus("mount failed all freqs");
    dualPrintf("[cypher-flock] SD_MMC mount failed (clk=%d cmd=%d d0=%d)\n",
               SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN);
    return;
  }

  sdCardTypeValue = SD_MMC.cardType();
  if (sdCardTypeValue == CARD_NONE) {
    sdSetStatus("no card detected");
    dualPrintln("[cypher-flock] SD_MMC mounted but no card was detected");
    SD_MMC.end();
    return;
  }
  sdCardSizeBytes = SD_MMC.cardSize();

  for (int i = 1; i < 1000; i++) {
    char path[24];
    snprintf(path, sizeof(path), "/FlockLog_%03d.csv", i);
    if (!SD_MMC.exists(path)) {
      currentLogFile = path;
      File f = SD_MMC.open(path, FILE_WRITE);
      if (!f) {
        sdSetStatus("write probe failed");
        dualPrintf("[cypher-flock] SD_MMC write probe failed: %s\n", path);
        SD_MMC.end();
        return;
      }
      f.println("Uptime_ms,Date_Time,Channel,Capture_Type,Protocol,RSSI,MAC_Address,Device_Name,TX_Power,Detection_Method,Confidence,Confidence_Label,Extra_Data,Latitude,Longitude,Speed_MPH,Heading_Deg,Altitude_M");
      f.close();
      break;
    }
  }
  if (currentLogFile.length() == 0) {
    sdSetStatus("no log slot");
    dualPrintln("[cypher-flock] SD_MMC no available log slot");
    SD_MMC.end();
    return;
  }

  sdReady = true;
  sdSetStatus("ready");
  dualPrintf("[cypher-flock] SD_MMC logging ready: %s type=%s size=%lluMB\n",
             currentLogFile.c_str(), sdCardTypeName(sdCardTypeValue),
             (unsigned long long)(sdCardSizeBytes / (1024ULL * 1024ULL)));
#else
#if defined(SD_MOSI_PIN) && defined(SD_MISO_PIN) && defined(SD_SCK_PIN) && (SD_MOSI_PIN >= 0) && (SD_MISO_PIN >= 0) && (SD_SCK_PIN >= 0)
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
#endif
  if (SD.begin(SD_CS_PIN)) {
    sdReady = true;
    for (int i = 1; i < 1000; i++) {
      char path[24];
      snprintf(path, sizeof(path), "/FlockLog_%03d.csv", i);
      if (!SD.exists(path)) {
        currentLogFile = path;
        File f = SD.open(path, FILE_WRITE);
        if (f) {
          f.println("Uptime_ms,Date_Time,Channel,Capture_Type,Protocol,RSSI,MAC_Address,Device_Name,TX_Power,Detection_Method,Confidence,Confidence_Label,Extra_Data,Latitude,Longitude,Speed_MPH,Heading_Deg,Altitude_M");
          f.close();
        }
        break;
      }
    }
    sdSetStatus("ready");
    dualPrintf("[cypher-flock] SD logging ready: %s\n", currentLogFile.c_str());
  } else {
    sdSetStatus("mount failed");
    dualPrintln("[cypher-flock] SD init skipped/failed");
  }
#endif
#endif
}

static void storageLogDetection(const FYDetection& d) {
#if ENABLE_SD_LOGGING
  if (!sdReady) return;
#if USE_SD_MMC
  File f = SD_MMC.open(currentLogFile.c_str(), FILE_APPEND);
#else
  File f = SD.open(currentLogFile.c_str(), FILE_APPEND);
#endif
  if (!f) return;
  f.printf("%lu,%lu,%u,%s,%s,%d,%s,%s,%d,%s,%u,%s,%s,,,,,\n",
           (unsigned long)millis(), (unsigned long)millis(), (unsigned)d.channel,
           d.captureType, d.protocol, d.rssi, d.mac, d.deviceName, (int)d.txPower,
           d.method, (unsigned)d.confidence, d.confidenceLabel, d.extra);
  f.close();
#else
  (void)d;
#endif
}

// ============================================================
// FLASK-COMPATIBLE JSON EMISSION
// ============================================================
//
// The Flask app (flock-you/api/flockyou.py) reads one JSON object per line
// from the USB CDC serial port. It filters by presence of `detection_method`
// and extracts these fields:  mac_address, rssi, channel, frequency, ssid,
// device_name, gps.latitude, gps.longitude, gps.accuracy.
//
// The Flask app can add GPS from a host-side USB NMEA puck or browser
// geolocation. Cypherbox builds can also read on-device GPS via SerialGPS.

static void emitDetectionJSON(const char* mac, const char* method, const char* protocol,
                              const char* deviceName, int8_t rssi, uint8_t ch,
                              const char* ssid, uint8_t confidence, const char* label) {
  char ssidEsc[sizeof(((FYDetection*)0)->ssid) * 6 + 1];
  char nameEsc[sizeof(((FYDetection*)0)->deviceName) * 6 + 1];
  jsonEscape(ssidEsc, sizeof(ssidEsc), ssid ? ssid : "");
  jsonEscape(nameEsc, sizeof(nameEsc), deviceName ? deviceName : "");
  char oui[9];
  uint8_t mbytes[6] = {0};
  sscanf(mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
         &mbytes[0], &mbytes[1], &mbytes[2], &mbytes[3], &mbytes[4], &mbytes[5]);
  ouiFromMac(mbytes, oui, sizeof(oui));

  dualPrintf(
      "{\"event\":\"detection\","
      "\"detection_method\":\"%s\","
      "\"protocol\":\"wifi_2_4ghz\","
      "\"mac_address\":\"%s\","
      "\"oui\":\"%s\","
      "\"device_name\":\"%s\","
      "\"rssi\":%d,"
      "\"channel\":%u,"
      "\"frequency\":%u,"
      "\"ssid\":\"%s\","
      "\"confidence\":%u,"
      "\"confidence_label\":\"%s\"}\n",
      method, protocol, mac, oui, nameEsc, rssi,
      (unsigned)ch, (unsigned)channelFreqMhz(ch), ssidEsc,
      (unsigned)confidence, label ? label : confidenceLabel(confidence));
}

// ============================================================
// PROMISCUOUS CALLBACK  — keep it fast, no Serial, no malloc
// ============================================================

static bool IRAM_ATTR extractSsidFromMgmtBody(const uint8_t* body, int len,
                                     char* outSsid, size_t outLen) {
  if (!body || len <= 0 || !outSsid || outLen == 0) return false;
  while (len >= 2) {
    uint8_t id = body[0], elen = body[1];
    if ((int)elen + 2 > len) break;
    if (id == 0) {
      size_t n = (elen < (outLen - 1)) ? elen : (outLen - 1);
      memcpy(outSsid, body + 2, n);
      outSsid[n] = '\0';
      return true;
    }
    body += elen + 2; len -= elen + 2;
  }
  return false;
}

// Returns:
//   1  = wildcard SSID IE found (tag 0, length 0)  → Flock-style probe
//   0  = SSID IE found, non-zero length            → directed probe, not ours
//  -1  = no SSID IE found at all                   → caller should retry with
//                                                    FCS-stripped length, then bail
static int IRAM_ATTR isWildcardProbeIE(const uint8_t* body, int len) {
  if (!body || len < 2) return -1;
  while (len >= 2) {
    uint8_t id   = body[0];
    uint8_t elen = body[1];
    if ((int)elen + 2 > len) break;
    if (id == 0) return (elen == 0) ? 1 : 0;
    body += elen + 2;
    len  -= elen + 2;
  }
  return -1;
}

static void IRAM_ATTR wifiSniffer(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!buf || sniffingStopped || sniffingPaused) return;

#if PROCESS_MGMT_FRAMES && PROCESS_DATA_FRAMES
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
#elif PROCESS_MGMT_FRAMES
  if (type != WIFI_PKT_MGMT) return;
#elif PROCESS_DATA_FRAMES
  if (type != WIFI_PKT_DATA) return;
#else
  return;  // nothing configured to process
#endif

  wifi_promiscuous_pkt_t*      pkt = (wifi_promiscuous_pkt_t*)buf;
  if (pkt->rx_ctrl.sig_len < sizeof(wifi_ieee80211_mac_hdr_t)) return;
  wifi_ieee80211_mac_hdr_t*    hdr = (wifi_ieee80211_mac_hdr_t*)pkt->payload;
  int8_t rssi = pkt->rx_ctrl.rssi;

  if (rssi < RSSI_MIN) return;

  uint8_t ch = (uint8_t)pkt->rx_ctrl.channel;  // actual rx channel from driver

  // --- OUI check: addr2 (transmitter/source) ---
  //
  // For mgmt Probe Requests (type=0 subtype=4) from a matched OUI, tighten
  // to the DeFlockJoplin wildcard-probe signature: SSID IE (tag 0) length
  // must be zero. This reduces false positives dramatically (Michael's field
  // test: 11/12 true-positive with only 2 false-positives in Joplin).
  //
  // Non-probe frames from the same OUI still emit the broad ADDR2 alert.
  // See: https://github.com/DeflockJoplin/flock-you
  if (matchOuiRaw(hdr->addr2)) {
    bool emitted = false;
    if (type == WIFI_PKT_MGMT) {
      uint8_t fc0     = hdr->frame_ctrl & 0xFF;
      uint8_t ftype   = (fc0 >> 2) & 0x03;
      uint8_t subtype = (fc0 >> 4) & 0x0F;
      if (ftype == 0 && subtype == 4) {                        // Probe Request
        int sigLen  = (int)pkt->rx_ctrl.sig_len;
        int bodyLen = sigLen - (int)sizeof(wifi_ieee80211_mac_hdr_t);
        const uint8_t* body = pkt->payload + sizeof(wifi_ieee80211_mac_hdr_t);
        int r = (bodyLen > 0) ? isWildcardProbeIE(body, bodyLen) : -1;
        // FCS-trailer retry: only when the first parse found no SSID IE AT
        // ALL (-1). A found-but-nonzero (0) means legit directed probe; do
        // not retry — it would mis-classify.
        if (r == -1 && bodyLen > 4) r = isWildcardProbeIE(body, bodyLen - 4);
        if (r == 1) {
          enqueueAlert(ALERT_WILDCARD_PROBE, hdr->addr2, rssi, ch,
                       nullptr, "probe_req");
          emitted = true;
        }
      }
    }
    if (!emitted) {
      enqueueAlert(ALERT_OUI_ADDR2, hdr->addr2, rssi, ch, nullptr, "addr2");
    }
  }

#if CHECK_ADDR1
  // addr1 (receiver/destination): catches Flock STAs that appear only as the
  // dst of probe responses and data frames — never transmitting in the capture
  // window due to their burst-sleep duty cycle. Multicast guard is mandatory
  // here since addr1 is broadcast (ff:ff:ff:ff:ff:ff) in beacons/broadcasts.
  if (!isMulticast(hdr->addr1) && matchOuiRaw(hdr->addr1)) {
    enqueueAlert(ALERT_OUI_ADDR1, hdr->addr1, rssi, ch, nullptr, "addr1");
  }
#endif

#if CHECK_ADDR3
  // addr3 fallback: catches cases where addr2 is randomised but addr3
  // carries the real BSSID OUI (management frames only).
  if (type == WIFI_PKT_MGMT && matchOuiRaw(hdr->addr3)) {
    enqueueAlert(ALERT_OUI_ADDR3, hdr->addr3, rssi, ch, nullptr, "addr3");
  }
#endif

#if ENABLE_SSID_MATCH
  if (type == WIFI_PKT_MGMT) {
    uint8_t fc0     = hdr->frame_ctrl & 0xFF;
    uint8_t subtype = (fc0 >> 4) & 0x0F;
    uint8_t ftype   = (fc0 >> 2) & 0x03;

    if (ftype == 0) {
      int sigLen = pkt->rx_ctrl.sig_len - 4;  // strip 4-byte FCS
      if (sigLen < (int)sizeof(wifi_ieee80211_mac_hdr_t)) return;

      const uint8_t* mgmtBody    = nullptr;
      int            mgmtBodyLen = 0;
      const char*    frameKind   = nullptr;

      if (subtype == 8 || subtype == 5) {
        // Beacon / Probe Response: fixed params = 12 bytes after MAC hdr
        int off = sizeof(wifi_ieee80211_mac_hdr_t) + 12;
        if (sigLen > off) {
          frameKind   = (subtype == 8) ? "beacon" : "probe_resp";
          mgmtBody    = pkt->payload + off;
          mgmtBodyLen = sigLen - off;
        }
      } else if (subtype == 4) {
        // Probe Request: IEs follow directly after MAC hdr
        int off = sizeof(wifi_ieee80211_mac_hdr_t);
        if (sigLen > off) {
          frameKind   = "probe_req";
          mgmtBody    = pkt->payload + off;
          mgmtBodyLen = sigLen - off;
        }
      }

      if (mgmtBody && mgmtBodyLen > 0) {
        char ssid[33] = {0};
        if (extractSsidFromMgmtBody(mgmtBody, mgmtBodyLen, ssid, sizeof(ssid))) {
          if (matchSsidKeyword(ssid)) {
            enqueueAlert(ALERT_SSID, hdr->addr2, rssi, ch, ssid, frameKind);
          }
        }
      }
    }
  }
#endif
}

// ============================================================
// BLE SCANNER + RAVEN FINGERPRINTING
// ============================================================

static const char* bleNamePatterns[] = {
  "FS Ext Battery", "Penguin", "Flock", "Pigvision", "FlockCam", "FS-"
};
static const size_t BLE_NAME_PATTERN_COUNT = sizeof(bleNamePatterns) / sizeof(bleNamePatterns[0]);

static const char* ravenServiceUuids[] = {
  "0000180a-0000-1000-8000-00805f9b34fb",
  "00003100-0000-1000-8000-00805f9b34fb",
  "00003200-0000-1000-8000-00805f9b34fb",
  "00003300-0000-1000-8000-00805f9b34fb",
  "00003400-0000-1000-8000-00805f9b34fb",
  "00003500-0000-1000-8000-00805f9b34fb",
  "00001809-0000-1000-8000-00805f9b34fb",
  "00001819-0000-1000-8000-00805f9b34fb",
};
static const size_t RAVEN_UUID_COUNT = sizeof(ravenServiceUuids) / sizeof(ravenServiceUuids[0]);

static bool matchBleName(const char* name) {
  if (!name || !name[0]) return false;
  for (size_t i = 0; i < BLE_NAME_PATTERN_COUNT; i++) {
    if (strcasestr_local(name, bleNamePatterns[i])) return true;
  }
  return false;
}

static bool isPenguinNumericName(const char* name) {
  if (!name) return false;
  int len = strlen(name);
  if (len < 8 || len > 12) return false;
  for (int i = 0; i < len; i++) {
    if (!isdigit((unsigned char)name[i])) return false;
  }
  return true;
}

static bool hasTnSerial(const std::string& data) {
  if (data.length() < 10) return false;
  for (size_t i = 0; i + 1 < data.length(); i++) {
    if (data[i] == 'T' && data[i + 1] == 'N') return true;
  }
  return false;
}

static int countRavenUuids(const NimBLEAdvertisedDevice* device) {
  if (!device || !device->haveServiceUUID()) return 0;
  int matched = 0;
  int count = device->getServiceUUIDCount();
  for (int i = 0; i < count; i++) {
    std::string uuid = device->getServiceUUID(i).toString();
    for (size_t j = 0; j < RAVEN_UUID_COUNT; j++) {
      if (strcasecmp(uuid.c_str(), ravenServiceUuids[j]) == 0) {
        matched++;
        break;
      }
    }
  }
  return matched;
}

static const char* classifyRavenFirmware(const NimBLEAdvertisedDevice* device) {
  if (!device || !device->haveServiceUUID()) return "unknown";
  bool hasHealth = false, hasLocation = false, hasGps = false, hasPower = false;
  bool hasNetwork = false, hasUpload = false, hasError = false;
  int count = device->getServiceUUIDCount();
  for (int i = 0; i < count; i++) {
    std::string uuid = device->getServiceUUID(i).toString();
    if (strcasestr_local(uuid.c_str(), "00001809")) hasHealth = true;
    if (strcasestr_local(uuid.c_str(), "00001819")) hasLocation = true;
    if (strcasestr_local(uuid.c_str(), "00003100")) hasGps = true;
    if (strcasestr_local(uuid.c_str(), "00003200")) hasPower = true;
    if (strcasestr_local(uuid.c_str(), "00003300")) hasNetwork = true;
    if (strcasestr_local(uuid.c_str(), "00003400")) hasUpload = true;
    if (strcasestr_local(uuid.c_str(), "00003500")) hasError = true;
  }
  if (hasGps && hasPower && hasNetwork && hasUpload && hasError) return "1.3.x";
  if (hasGps && hasPower && hasNetwork) return "1.2.x";
  if (hasHealth || hasLocation) return "1.1.x";
  return "unknown";
}

static bool parseMacString(const char* s, uint8_t* out) {
  unsigned vals[6];
  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]) != 6) return false;
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)vals[i];
  return true;
}

class FlockBleCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    NimBLEAddress addr = advertisedDevice->getAddress();
    std::string addrText = addr.toString();
    uint8_t mac[6] = {0};
    if (!parseMacString(addrText.c_str(), mac)) return;

    char name[33] = "";
    if (advertisedDevice->haveName()) {
      std::string n = advertisedDevice->getName();
      strlcpy(name, n.c_str(), sizeof(name));
    }

    char methods[48] = "";
    char extra[48] = "";
    int methodCount = 0;
    uint8_t confidence = 0;
    bool macMatch = matchOuiRaw(mac);
    bool nameMatch = matchBleName(name);
    bool penguinNum = isPenguinNumericName(name);
    int ravenCount = countRavenUuids(advertisedDevice);
    bool raven = ravenCount > 0;

    if (macMatch) { confidence += CONF_MAC_PREFIX; strlcat(methods, "ble_mac ", sizeof(methods)); methodCount++; }
    if (nameMatch) { confidence += CONF_BLE_NAME; strlcat(methods, "ble_name ", sizeof(methods)); methodCount++; }
    if (penguinNum) { confidence += CONF_PENGUIN_NUM; strlcat(methods, "ble_penguin_num ", sizeof(methods)); methodCount++; }

    if (advertisedDevice->haveManufacturerData()) {
      std::string mfg = advertisedDevice->getManufacturerData();
      if (mfg.length() >= 2) {
        uint16_t company = (uint8_t)mfg[0] | ((uint8_t)mfg[1] << 8);
        if (company == 0x09C8) {
          confidence += CONF_MFG_ID;
          strlcat(methods, "ble_mfg_0x09C8 ", sizeof(methods));
          methodCount++;
        }
      }
      if (hasTnSerial(mfg)) {
        confidence += CONF_PENGUIN_SERIAL;
        strlcat(methods, "ble_tn_serial ", sizeof(methods));
        methodCount++;
      }
    }

    if (raven) {
      confidence += (ravenCount >= 3) ? CONF_RAVEN_MULTI_UUID : CONF_RAVEN_UUID;
      strlcat(methods, (ravenCount >= 3) ? "ble_raven_multi " : "ble_raven_uuid ", sizeof(methods));
      snprintf(extra, sizeof(extra), "raven_fw=%s uuid_count=%d", classifyRavenFirmware(advertisedDevice), ravenCount);
      methodCount++;
    }

    uint8_t addrType = addr.getType();
    if (addrType == 0) {
      confidence += CONF_BONUS_BLE_STATIC_ADDR;
      strlcat(methods, "ble_pub_addr ", sizeof(methods));
    } else if (mac[0] & 0xC0) {
      confidence += CONF_BONUS_BLE_STATIC_ADDR;
      strlcat(methods, "ble_static_addr ", sizeof(methods));
    }

    if (methodCount >= 2) confidence += CONF_BONUS_MULTI_METHOD;
    if (advertisedDevice->getRSSI() > -50) confidence += CONF_BONUS_STRONG_RSSI;

    char macStr[18];
    macToStr(mac, macStr, sizeof(macStr));
    rssiTrackUpdate(macStr, advertisedDevice->getRSSI());
    if (confidence >= CONFIDENCE_ALARM_THRESHOLD && rssiTrackStationaryBonus(macStr)) {
      confidence += CONF_BONUS_STATIONARY;
      strlcat(methods, "rssi_trend ", sizeof(methods));
    }
    if (confidence > 100) confidence = 100;
    if (confidence < CONFIDENCE_ALARM_THRESHOLD) return;

    int txPower = advertisedDevice->haveTXPower() ? advertisedDevice->getTXPower() : 0;
    enqueueDetectionEvent(raven ? ALERT_BLE_RAVEN : ALERT_BLE_FLOCK, mac, advertisedDevice->getRSSI(), 0,
                          nullptr, raven ? "raven_ble" : "flock_ble", name, methods, extra, txPower, confidence);
  }
};

static FlockBleCallbacks bleCallbacks;

static void bleInit() {
  NimBLEDevice::init("CypherFlock");
  bleScan = NimBLEDevice::getScan();
  bleScan->setScanCallbacks(&bleCallbacks, true);
  bleScan->setActiveScan(true);
  bleScan->setInterval(97);
  bleScan->setWindow(97);
  dualPrintln("[cypher-flock] BLE scanner ready");
}

static void bleTick() {
  if (!bleScan) return;
  if (millis() - lastBleScanAt < BLE_SCAN_INTERVAL_MS) return;
  if (!bleScan->isScanning()) {
    bleScan->start(BLE_SCAN_DURATION_S, false);
    bleScan->clearResults();
    lastBleScanAt = millis();
  }
}

// ============================================================
// DRAIN QUEUE — called from loop(), safe to Serial.print here
// ============================================================

static void drainAlertQueue() {
  while (true) {
    portENTER_CRITICAL(&queueMux);
    if (alertTail == alertHead) { portEXIT_CRITICAL(&queueMux); break; }
    AlertEntry e;
    memcpy(&e, (const void*)&alertQueue[alertTail], sizeof(AlertEntry));
    alertTail = (alertTail + 1) % ALERT_QUEUE_SIZE;
    portEXIT_CRITICAL(&queueMux);

    char macStr[18];
    macToStr(e.mac, macStr, sizeof(macStr));
    const bool isBle = (e.type == ALERT_BLE_FLOCK || e.type == ALERT_BLE_RAVEN);
    const bool isRaven = (e.type == ALERT_BLE_RAVEN);
    char method[48];
    if (e.methods[0]) strlcpy(method, e.methods, sizeof(method));
    else snprintf(method, sizeof(method), "wifi_%s", alertTypeToMethod(e.type));

    uint8_t confidence = e.confidence;
    if (!isBle) {
      bool ssidFmt = (e.type == ALERT_SSID) && isFlockSsidFormat(e.ssid);
      bool ssidGeneric = (e.type == ALERT_SSID) && matchSsidKeyword(e.ssid);
      bool macMethod = (e.type == ALERT_OUI_ADDR2 || e.type == ALERT_OUI_ADDR1 ||
                        e.type == ALERT_OUI_ADDR3 || e.type == ALERT_WILDCARD_PROBE);
      if (ssidFmt) confidence += CONF_SSID_FLOCK_FMT;
      else if (ssidGeneric) confidence += CONF_SSID_PATTERN;
      if (macMethod) confidence += CONF_MAC_PREFIX;
      if (e.type == ALERT_WILDCARD_PROBE) confidence += CONF_BONUS_MULTI_METHOD;
      if (e.rssi > -50) confidence += CONF_BONUS_STRONG_RSSI;
      rssiTrackUpdate(macStr, e.rssi);
      if (confidence >= CONFIDENCE_ALARM_THRESHOLD && rssiTrackStationaryBonus(macStr)) confidence += CONF_BONUS_STATIONARY;
      if (confidence > 100) confidence = 100;
    }

    const char* protocol = isBle ? "bluetooth_le" : "wifi_2_4ghz";
    const char* captureType = isRaven ? "RAVEN_BLE" : (isBle ? "FLOCK_BLE" : "FLOCK_WIFI");
    const char* label = confidenceLabel(confidence);

    strlcpy(uiLastMac, macStr, sizeof(uiLastMac));
    strlcpy(uiLastMethod, method, sizeof(uiLastMethod));
    strlcpy(uiLastProtocol, protocol, sizeof(uiLastProtocol));
    strlcpy(uiLastName, e.deviceName, sizeof(uiLastName));
    strlcpy(uiLastConfidenceLabel, label, sizeof(uiLastConfidenceLabel));
    uiLastRssi = e.rssi;
    uiLastChannel = e.channel;
    uiLastConfidence = confidence;

    // Always update the on-device detection table (survives reboot via SPIFFS).
    // chirpWorthy = true for brand-new MACs AND for MACs rediscovered after
    // REDISCOVER_MS of silence (drove away and came back).
    bool chirpWorthy = false;
    int idx = fyAddDetection(macStr, method, protocol, captureType, e.deviceName, e.extra,
                             e.rssi, e.channel, e.txPower, confidence, label,
                             (e.type == ALERT_SSID) ? e.ssid : nullptr,
                             &chirpWorthy);
    if (idx >= 0 && chirpWorthy) storageLogDetection(fyDet[idx]);

    // Refresh the global "still around" timer for the heartbeat tick.
    // Done unconditionally so a device counts as active even when serial is
    // rate-limited (still audible via heartbeat, just quieter on the wire).
    fyLastTargetSeen = millis();

    // Serial-rate-limit: suppress emit/beep/flash within ALERT_COOLDOWN_MS.
    if (shouldSuppressDuplicate(macStr)) continue;

    // Human-readable line (for serial terminal / mirror).
    char oui[9];
    ouiFromMac(e.mac, oui, sizeof(oui));
    if (isBle) {
      dualPrintf("[cypher-flock] DETECT-%s mac=%s name=\"%s\" rssi=%d confidence=%u %s methods=\"%s\" count=%d\n",
                 captureType, macStr, e.deviceName, e.rssi, (unsigned)confidence, label, method,
                 (idx >= 0) ? (int)fyDet[idx].count : 0);
    } else if (e.type == ALERT_SSID) {
      dualPrintf("[cypher-flock] DETECT-SSID type=%s mac=%s ssid=\"%s\" rssi=%d ch=%u count=%d\n",
                 e.frameKind, macStr, e.ssid, e.rssi, e.channel,
                 (idx >= 0) ? (int)fyDet[idx].count : 0);
    } else {
      dualPrintf("[cypher-flock] DETECT-OUI mac=%s oui=%s rssi=%d ch=%u addr=%s count=%d\n",
                 macStr, oui, e.rssi, e.channel,
                 e.frameKind[0] ? e.frameKind : "addr2",
                 (idx >= 0) ? (int)fyDet[idx].count : 0);
    }

    // Flask-compatible JSON line (parsed by api/flockyou.py over USB CDC).
    emitDetectionJSON(macStr, method, protocol, e.deviceName, e.rssi, e.channel,
                      (e.type == ALERT_SSID) ? e.ssid : "", confidence, label);

    if (isRaven) sessionRaven++;
    else if (isBle) sessionBle++;
    else sessionWifi++;
    snprintf(uiLiveFeed[4], sizeof(uiLiveFeed[4]), "%s", uiLiveFeed[3]);
    snprintf(uiLiveFeed[3], sizeof(uiLiveFeed[3]), "%s", uiLiveFeed[2]);
    snprintf(uiLiveFeed[2], sizeof(uiLiveFeed[2]), "%s", uiLiveFeed[1]);
    snprintf(uiLiveFeed[1], sizeof(uiLiveFeed[1]), "%s", uiLiveFeed[0]);
    snprintf(uiLiveFeed[0], sizeof(uiLiveFeed[0]), "%s %s %u%%", isBle ? "BLE" : "WiFi", macStr + 9, (unsigned)confidence);
    activityBuckets[activityBucketIndex]++;

    // Audio feedback:
    //   - NEW MAC  → two fast ascending beeps (clearly distinct sound)
    //   - REPEAT   → silent; the heartbeat tick covers continued presence
    // LED flashes on every emitted detection either way.
    if (chirpWorthy) {
      if (confidence >= CONFIDENCE_ALARM_THRESHOLD) alarmEscalation(confidence);
      else newDetectChirp();
      // Reset the heartbeat phase so the first follow-up beep lands
      // HB_BEEP_INTERVAL_MS after the initial chirp, not mid-window.
      fyLastHeartbeatAt = millis();
    }
    ledFlash(LED_FLASH_MS);

#if STOP_ON_OUI_HIT
    if (e.type != ALERT_SSID) stopSniffing("OUI hit");
#endif
#if STOP_ON_SSID_HIT
    if (e.type == ALERT_SSID) stopSniffing("SSID hit");
#endif
  }
}

// ============================================================
// AUTOSAVE
// ============================================================

static void autosaveTick() {
  if (!fySpiffsReady || !fyDirty) return;
  if (millis() - fyLastSaveAt < AUTOSAVE_INTERVAL_MS) return;
  fySaveSession();
}

static void activityTick() {
  unsigned long now = millis();
  if (now - lastActivityBucketAt >= 1000) {
    activityBucketIndex = (activityBucketIndex + 1) % 25;
    activityBuckets[activityBucketIndex] = 0;
    lastActivityBucketAt = now;
  }
}

static void lifetimeTick() {
  if (millis() - lastLifetimeSaveAt >= AUTOSAVE_INTERVAL_MS) lifetimeSave();
}

static void gpsTick() {
#if ENABLE_GPS
  while (SerialGPS.available()) gps.encode(SerialGPS.read());
#endif
}

// Heartbeat beep while at least one target was seen in the last
// HB_DEVICE_ACTIVE_MS. Fires HB_BEEP_INTERVAL_MS apart.
static void heartbeatTick() {
  if (fyLastTargetSeen == 0) return;                           // never seen one
  unsigned long now = millis();
  if (now - fyLastTargetSeen > HB_DEVICE_ACTIVE_MS) return;    // gone silent
  if (now - fyLastHeartbeatAt < HB_BEEP_INTERVAL_MS) return;   // too soon
  heartbeatBeep();
  fyLastHeartbeatAt = now;
}

static String jsonFileList(fs::FS& fs, const char* prefix) {
  String out = "";
  File root = fs.open("/");
  if (!root) return out;
  File file = root.openNextFile();
  while (file) {
    if (out.length()) out += ",";
    out += "{\"fs\":\"";
    out += prefix;
    out += "\",\"name\":\"";
    out += file.name();
    out += "\",\"size\":";
    out += String((unsigned long)file.size());
    out += "}";
    file = root.openNextFile();
  }
  return out;
}

#if ENABLE_POWER_STATUS
static void powerRead(bool force = false);
#endif

static void webInit() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL);
  webServer.on("/", HTTP_GET, []() {
    String html = "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>Cypher Flock</title></head><body><h1>Cypher Flock</h1><p>AP file browser</p><ul id='files'></ul><script>fetch('/files').then(r=>r.json()).then(d=>{files.innerHTML=d.files.map(f=>`<li>${f.fs}: <a href='/download/${f.fs}${f.name}'>${f.name}</a> (${f.size} bytes)</li>`).join('')})</script></body></html>";
    webServer.send(200, "text/html", html);
  });
  webServer.on("/files", HTTP_GET, []() {
    String body = "{\"files\":[";
    body += jsonFileList(SPIFFS, "littlefs");
#if ENABLE_SD_LOGGING
    if (sdReady) {
#if USE_SD_MMC
      String sd = jsonFileList(SD_MMC, "sd");
#else
      String sd = jsonFileList(SD, "sd");
#endif
      if (body[body.length() - 1] != '[' && sd.length()) body += ",";
      body += sd;
    }
#endif
    body += "]}";
    webServer.send(200, "application/json", body);
  });
  webServer.on("/status", HTTP_GET, []() {
    String body = "{";
    body += "\"uptime\":" + String((unsigned long)millis());
    body += ",\"detections\":" + String(fyDetCount);
    body += ",\"wifi\":" + String(sessionWifi);
    body += ",\"ble\":" + String(sessionBle);
    body += ",\"raven\":" + String(sessionRaven);
    body += ",\"gps_lock\":";
#if ENABLE_GPS
    body += gps.location.isValid() ? "true" : "false";
#else
    body += "false";
#endif
    body += ",\"free_heap\":" + String((unsigned long)ESP.getFreeHeap());
#if ENABLE_POWER_STATUS
    powerRead();
    body += ",\"power_ready\":";
    body += powerReady ? "true" : "false";
    body += ",\"battery_percent\":" + String(powerBatteryPercent);
    body += ",\"battery_charging\":";
    body += powerCharging ? "true" : "false";
    body += ",\"vbus_present\":";
    body += powerVbusPresent ? "true" : "false";
#endif
    body += "}";
    webServer.send(200, "application/json", body);
  });
  webServer.on("/reset", HTTP_POST, []() {
    fyDetCount = 0;
    fyDirty = true;
    sessionWifi = sessionBle = sessionRaven = 0;
    webServer.send(200, "application/json", "{\"status\":\"ok\"}");
  });
  webServer.onNotFound([]() {
    String uri = webServer.uri();
    if (!uri.startsWith("/download/")) {
      webServer.send(404, "application/json", "{\"error\":\"not found\"}");
      return;
    }
    String rest = uri.substring(strlen("/download/"));
    fs::FS* fsPtr = &SPIFFS;
    if (rest.startsWith("littlefs")) rest = rest.substring(strlen("littlefs"));
#if ENABLE_SD_LOGGING
    else if (rest.startsWith("sd")) {
      rest = rest.substring(strlen("sd"));
#if USE_SD_MMC
      fsPtr = &SD_MMC;
#else
      fsPtr = &SD;
#endif
    }
#endif
    if (!rest.startsWith("/")) rest = "/" + rest;
    if (!fsPtr->exists(rest)) {
      webServer.send(404, "application/json", "{\"error\":\"file not found\"}");
      return;
    }
    File file = fsPtr->open(rest, "r");
    webServer.streamFile(file, "application/octet-stream");
    file.close();
  });
  webServer.begin();
  apReady = true;
  dualPrintf("[cypher-flock] AP webserver ready: ssid=%s ip=%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

static void uiFitText(char* out, size_t outLen, const char* in, uint8_t maxChars) {
  if (!outLen) return;
  if (!in || !in[0]) {
    strlcpy(out, "-", outLen);
    return;
  }
  strlcpy(out, in, outLen);
  if (strlen(out) <= maxChars) return;
  if (maxChars < 2) {
    out[0] = '\0';
    return;
  }
  out[maxChars - 1] = '~';
  out[maxChars] = '\0';
}

#if USE_CARDPUTER_DISPLAY
static const uint16_t CARD_BLACK = 0x0000;
static const uint16_t CARD_WHITE = 0xFFFF;
static const uint16_t CARD_DIM = 0x9CD3;
static const uint16_t CARD_PANEL = 0x0841;
static const uint16_t CARD_ACCENT = 0x07FF;
static const uint16_t CARD_WARN = 0xFD20;
static const uint16_t CARD_DANGER = 0xF800;
static const uint16_t CARD_OK = 0x07E0;

static void cardText(int16_t x, int16_t y, const char* text, uint8_t size = 1,
                     uint16_t color = CARD_WHITE, uint16_t bg = CARD_BLACK) {
  M5Cardputer.Display.setTextSize(size);
  M5Cardputer.Display.setTextColor(color, bg);
  M5Cardputer.Display.setCursor(x, y);
  M5Cardputer.Display.print(text);
}

static void cardPrintf(int16_t x, int16_t y, uint8_t size, uint16_t color, const char* fmt, ...) {
  char buf[80];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  cardText(x, y, buf, size, color);
}

static void cardHeader(const char* title) {
  M5Cardputer.Display.fillRect(0, 0, M5Cardputer.Display.width(), 21, CARD_BLACK);
  M5Cardputer.Display.drawFastHLine(0, 20, M5Cardputer.Display.width(), CARD_PANEL);
  cardText(6, 5, title, 1, CARD_ACCENT);
  cardPrintf(208, 5, 1, CARD_DIM, "%u/7", (unsigned)(uiPage + 1));
}

static void cardFooter(unsigned long now) {
  int16_t y = M5Cardputer.Display.height() - 17;
  M5Cardputer.Display.fillRect(0, y, M5Cardputer.Display.width(), 17, CARD_BLACK);
  M5Cardputer.Display.drawFastHLine(0, y, M5Cardputer.Display.width(), CARD_PANEL);
  char line[42];
  if (uiMenuMode) {
    snprintf(line, sizeof(line), "MENU %s  arrows edit  Enter next", uiMenuShortLabel());
  } else if (uiStatusUntilMs > now) {
    uiFitText(line, sizeof(line), uiStatusLine, 38);
  } else {
    snprintf(line, sizeof(line), "%s Ch%u %s", sniffingPaused ? "PAUSED" : "SCAN", currentChannel, channelModeName());
  }
  cardText(6, y + 5, line, 1, CARD_DIM);
}

static void cardPill(int16_t x, int16_t y, const char* label, bool active) {
  int16_t w = (int16_t)(strlen(label) * 6 + 11);
  uint16_t color = active ? CARD_ACCENT : CARD_DIM;
  M5Cardputer.Display.drawRoundRect(x, y, w, 15, 3, color);
  if (active) M5Cardputer.Display.fillRoundRect(x + 2, y + 2, w - 4, 11, 2, CARD_PANEL);
  cardText(x + 5, y + 4, label, 1, color);
}

static void cardBar(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t pct, uint16_t color) {
  pct = constrain(pct, 0, 100);
  M5Cardputer.Display.drawRoundRect(x, y, w, h, 3, CARD_DIM);
  int16_t fill = (int16_t)((w - 4) * pct / 100);
  if (fill > 0) M5Cardputer.Display.fillRoundRect(x + 2, y + 2, fill, h - 4, 2, color);
}

static void cardMetric(int16_t x, int16_t y, const char* label, unsigned long value, uint16_t color) {
  cardText(x, y, label, 1, CARD_DIM);
  cardPrintf(x, y + 13, 2, color, "%lu", value);
}
#endif

#if USE_AMOLED_DISPLAY
static const uint16_t AMOLED_BLACK = 0x0000;
static const uint16_t AMOLED_WHITE = 0xFFFF;
static const uint16_t AMOLED_DIM = 0x7BEF;
static const uint16_t AMOLED_PANEL = 0x1082;
static const uint16_t AMOLED_ACCENT = 0x07FF;
static const uint16_t AMOLED_WARN = 0xFD20;
static const uint16_t AMOLED_DANGER = 0xF800;
static const uint16_t AMOLED_OK = 0x07E0;

static void amoledText(int16_t x, int16_t y, const char* text, uint8_t size = 2,
                       uint16_t color = AMOLED_WHITE, uint16_t bg = AMOLED_BLACK) {
  if (!amoled) return;
  amoled->setTextSize(size);
  amoled->setTextColor(color, bg);
  amoled->setCursor(x, y);
  amoled->print(text);
}

static void amoledPrintf(int16_t x, int16_t y, uint8_t size, uint16_t color, const char* fmt, ...) {
  char buf[72];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  amoledText(x, y, buf, size, color);
}

static void powerRead(bool force) {
#if ENABLE_POWER_STATUS
  unsigned long now = millis();
  if (!force && now - powerLastReadAt < 2500) return;
  powerLastReadAt = now;
  if (!powerReady) return;
  powerBatteryPresent = powerPmu.isBatteryConnect();
  powerCharging = powerPmu.isCharging();
  powerVbusPresent = powerPmu.isVbusIn();
  powerBatteryMv = powerPmu.getBattVoltage();
  powerVbusMv = powerPmu.getVbusVoltage();
  powerSystemMv = powerPmu.getSystemVoltage();
  powerBatteryPercent = powerBatteryPresent ? powerPmu.getBatteryPercent() : -1;
#else
  (void)force;
#endif
}

static void powerLabel(char* out, size_t outLen) {
  if (!outLen) return;
#if ENABLE_POWER_STATUS
  powerRead();
  if (!powerReady) strlcpy(out, "BAT?", outLen);
  else if (powerBatteryPresent && powerBatteryPercent >= 0) {
    snprintf(out, outLen, powerCharging ? "%d%% CHG" : "%d%%", powerBatteryPercent);
  } else if (powerVbusPresent) strlcpy(out, "USB", outLen);
  else strlcpy(out, "BAT?", outLen);
#else
  strlcpy(out, "", outLen);
#endif
}

static void amoledHeader(const char* title, bool fullRedraw) {
  if (fullRedraw) {
    amoled->fillRect(0, 0, AMOLED_W, 92, AMOLED_BLACK);
    amoled->drawFastHLine(14, 47, AMOLED_W - 28, AMOLED_DIM);
    int16_t titleX = (int16_t)((AMOLED_W - (int)strlen(title) * 18) / 2);
    amoledText(titleX < 14 ? 14 : titleX, 14, title, 3, AMOLED_WHITE);
  }
  char pwr[16];
  powerLabel(pwr, sizeof(pwr));
  amoled->fillRect(214, 52, 140, 30, AMOLED_BLACK);
  if (pwr[0]) {
    int16_t badgeW = (int16_t)strlen(pwr) * 12;
    int16_t badgeX = AMOLED_W - badgeW - 18;
    if (badgeX < 214) badgeX = 214;
    amoled->drawRoundRect(badgeX - 8, 52, badgeW + 16, 28, 6, AMOLED_OK);
    amoledText(badgeX, 59, pwr, 2, AMOLED_OK);
  }
}

static void amoledFooter(unsigned long now) {
  amoled->fillRect(0, AMOLED_H - 50, AMOLED_W, 50, AMOLED_BLACK);
  amoled->drawFastHLine(14, AMOLED_H - 48, AMOLED_W - 28, AMOLED_DIM);
  char line[44];
  if (uiMenuMode) {
    snprintf(line, sizeof(line), "MENU %s  swipe edits", uiMenuShortLabel());
  } else if (uiStatusUntilMs > now) {
    uiFitText(line, sizeof(line), uiStatusLine, 40);
  } else {
    snprintf(line, sizeof(line), "%s Ch%u %s", sniffingPaused ? "PAUSED" : "SCAN", currentChannel, channelModeName());
  }
  amoledText(18, AMOLED_H - 32, line, 2, AMOLED_DIM);
  amoledPrintf(AMOLED_W - 54, AMOLED_H - 32, 2, AMOLED_DIM, "%u/7", (unsigned)(uiPage + 1));
}

static void amoledPill(int16_t x, int16_t y, const char* label, bool active) {
  int16_t w = (int16_t)(strlen(label) * 12 + 18);
  uint16_t color = active ? AMOLED_ACCENT : AMOLED_DIM;
  amoled->drawRoundRect(x, y, w, 28, 6, color);
  if (active) amoled->fillRoundRect(x + 2, y + 2, w - 4, 24, 5, 0x0228);
  amoledText(x + 9, y + 7, label, 2, color);
}

static void amoledBar(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t pct, uint16_t color) {
  pct = constrain(pct, 0, 100);
  amoled->drawRoundRect(x, y, w, h, 4, AMOLED_DIM);
  int16_t fill = (int16_t)((w - 6) * pct / 100);
  if (fill > 0) amoled->fillRoundRect(x + 3, y + 3, fill, h - 6, 3, color);
}

static void amoledMetric(int16_t x, int16_t y, const char* label, unsigned long value, uint16_t color) {
  amoledText(x, y, label, 2, AMOLED_DIM);
  amoledPrintf(x, y + 28, 4, color, "%lu", value);
}

static void amoledRemapTouch(int32_t rawX, int32_t rawY, int16_t& outX, int16_t& outY) {
  int32_t x = rawY;
  int32_t y = (AMOLED_H - 1) - rawX;
  outX = (int16_t)constrain(x, 0, AMOLED_W - 1);
  outY = (int16_t)constrain(y, 0, AMOLED_H - 1);
}

static void touchInit() {
#if USE_TOUCH_INPUT
  touchBus = std::make_shared<Arduino_HWIIC>(TOUCH_SDA_PIN, TOUCH_SCL_PIN, &Wire);
  touchDevice.reset(new Arduino_FT3x68(touchBus, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE, TOUCH_INT_PIN));
  for (uint8_t i = 0; i < 5; i++) {
    if (touchDevice->begin()) {
      touchDevice->IIC_Write_Device_State(touchDevice->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,
                                          touchDevice->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);
      uiTouchReady = true;
      dualPrintf("[cypher-flock] FT3168 touch init ok id=0x%X\n",
                 (unsigned)touchDevice->IIC_Read_Device_ID());
      return;
    }
    dualPrintln("[cypher-flock] FT3168 touch init retry");
    delay(250);
  }
  dualPrintln("[cypher-flock] FT3168 touch init failed");
#endif
}

static void powerInit() {
#if ENABLE_POWER_STATUS
  powerReady = powerPmu.begin(Wire, AXP2101_SLAVE_ADDRESS, TOUCH_SDA_PIN, TOUCH_SCL_PIN);
  if (!powerReady) {
    dualPrintln("[cypher-flock] AXP2101 PMIC unavailable");
    return;
  }
  powerPmu.enableBattDetection();
  powerPmu.enableBattVoltageMeasure();
  powerPmu.enableVbusVoltageMeasure();
  powerPmu.enableSystemVoltageMeasure();
  powerRead(true);
  dualPrintf("[cypher-flock] AXP2101 PMIC init ok id=0x%X\n", (unsigned)powerPmu.getChipID());
#endif
}
#endif

static void displayInit() {
#if USE_AMOLED_DISPLAY
  dualPrintln("[cypher-flock] AMOLED displayInit begin");
  Wire.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN);
  Wire.setClock(400000);
  dualPrintf("[cypher-flock] I2C begin sda=%d scl=%d\n", TOUCH_SDA_PIN, TOUCH_SCL_PIN);

  dualPrintln("[cypher-flock] probing AMOLED XCA9554 expander");
  if (!amoledExpander.begin(0x20, &Wire)) {
    amoledExpanderReady = false;
    dualPrintln("[cypher-flock] AMOLED XCA9554 not found; trying SH8601 anyway");
  } else {
    amoledExpanderReady = true;
    for (uint8_t pin = 0; pin < 3; pin++) {
      amoledExpander.pinMode(pin, OUTPUT);
      amoledExpander.digitalWrite(pin, LOW);
    }
    amoledExpander.pinMode(7, OUTPUT);
    amoledExpander.digitalWrite(7, LOW);
    delay(200);
    for (uint8_t pin = 0; pin < 3; pin++) amoledExpander.digitalWrite(pin, HIGH);
    amoledExpander.digitalWrite(7, HIGH);
    dualPrintln("[cypher-flock] AMOLED XCA9554 init ok; SD enable high");
  }

  dualPrintln("[cypher-flock] creating SH8601 QSPI bus");
  amoledBus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
  amoled = new Arduino_SH8601(amoledBus, LCD_RST, AMOLED_ROTATION, AMOLED_W, AMOLED_H);
  dualPrintln("[cypher-flock] starting SH8601");
  uiDisplayReady = amoled->begin();
  if (!uiDisplayReady) {
    dualPrintln("[cypher-flock] SH8601 init FAILED");
    return;
  }
  dualPrintln("[cypher-flock] SH8601 init ok");
  amoled->setBrightness(AMOLED_BRIGHTNESS);
  amoled->fillScreen(AMOLED_BLACK);
  dualPrintln("[cypher-flock] AMOLED splash drawing");
  powerInit();
  touchInit();

  amoled->drawRoundRect(20, 36, AMOLED_W - 40, AMOLED_H - 72, 18, AMOLED_ACCENT);
  amoledText(55, 128, "CYPHER", 4, AMOLED_WHITE);
  amoledText(55, 178, "FLOCK", 4, AMOLED_ACCENT);
  amoledText(55, 250, "passive detector", 2, AMOLED_DIM);
  char pwr[16];
  powerLabel(pwr, sizeof(pwr));
  amoledPrintf(55, 282, 2, AMOLED_OK, "touch %s  power %s", uiTouchReady ? "ok" : "off", pwr);
  delay(5000);
#elif USE_CARDPUTER_DISPLAY
  dualPrintln("[cypher-flock] Cardputer displayInit begin");
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setTextWrap(false);
  M5Cardputer.Display.setBrightness(CARDPUTER_BRIGHTNESS);
  M5Cardputer.Display.fillScreen(CARD_BLACK);
  uiDisplayReady = true;
  cardputerUiDirty = true;

  M5Cardputer.Display.drawRoundRect(8, 10, M5Cardputer.Display.width() - 16,
                                    M5Cardputer.Display.height() - 20, 7, CARD_ACCENT);
  cardText(22, 31, "CYPHER", 2, CARD_WHITE);
  cardText(22, 58, "FLOCK", 2, CARD_ACCENT);
  cardText(22, 92, "passive detector", 1, CARD_DIM);
  cardText(136, 92, "Cardputer ADV", 1, CARD_OK);
  delay(2500);
#else
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.setClock(100000);

  uint8_t addr = OLED_ADDR;
  Wire.beginTransmission(addr);
  if (Wire.endTransmission() != 0) {
    uint8_t fallback = (OLED_ADDR == 0x3C) ? 0x3D : 0x3C;
    Wire.beginTransmission(fallback);
    if (Wire.endTransmission() == 0) {
      addr = fallback;
      dualPrintf("[cypher-flock] SSD1306 using fallback addr 0x%02X\n", addr);
    } else {
      dualPrintf("[cypher-flock] SSD1306 no ACK on 0x%02X or 0x%02X (sda=%d scl=%d)\n",
                 OLED_ADDR, fallback, OLED_SDA_PIN, OLED_SCL_PIN);
    }
  } else {
    dualPrintf("[cypher-flock] SSD1306 ACK at 0x%02X (sda=%d scl=%d)\n",
               addr, OLED_SDA_PIN, OLED_SCL_PIN);
  }

  uiDisplayReady = display.begin(SSD1306_SWITCHCAPVCC, addr);
  if (!uiDisplayReady) {
    dualPrintln("[cypher-flock] SSD1306 init FAILED");
    return;
  }
  uiFonts.begin(display);
  uiFonts.setFontMode(1);
  uiFonts.setFontDirection(0);
  uiFonts.setForegroundColor(SSD1306_WHITE);
  uiFonts.setBackgroundColor(SSD1306_BLACK);

  display.clearDisplay();
  display.drawRect(0, 0, OLED_W, OLED_H, SSD1306_WHITE);
  display.drawFastHLine(4, 4, 24, SSD1306_WHITE);
  display.drawFastVLine(4, 4, 12, SSD1306_WHITE);
  display.drawFastHLine(OLED_W - 28, 4, 24, SSD1306_WHITE);
  display.drawFastVLine(OLED_W - 5, 4, 12, SSD1306_WHITE);
  display.drawFastHLine(4, OLED_H - 5, 24, SSD1306_WHITE);
  display.drawFastVLine(4, OLED_H - 16, 12, SSD1306_WHITE);
  display.drawFastHLine(OLED_W - 28, OLED_H - 5, 24, SSD1306_WHITE);
  display.drawFastVLine(OLED_W - 5, OLED_H - 16, 12, SSD1306_WHITE);

  display.drawRoundRect(49, 6, 30, 16, 3, SSD1306_WHITE);
  display.fillRect(55, 3, 18, 4, SSD1306_WHITE);
  display.drawCircle(64, 14, 5, SSD1306_WHITE);
  display.drawPixel(64, 14, SSD1306_WHITE);
  display.drawFastHLine(18, 14, 24, SSD1306_WHITE);
  display.drawFastHLine(86, 14, 24, SSD1306_WHITE);
  display.drawFastHLine(26, 18, 12, SSD1306_WHITE);
  display.drawFastHLine(90, 18, 12, SSD1306_WHITE);

  uiFonts.setFont(u8g2_font_7x13B_tf);
  uiFonts.setForegroundColor(SSD1306_WHITE);
  uiFonts.drawStr(21, 40, "CYPHER FLOCK");
  display.drawFastHLine(24, 45, 80, SSD1306_WHITE);
  display.drawPixel(18, 45, SSD1306_WHITE);
  display.drawPixel(109, 45, SSD1306_WHITE);

  uiFonts.setFont(u8g2_font_5x7_tf);
  uiFonts.drawStr(14, 56, "detect flock cameras");
  display.display();
  delay(5000);
#endif
}

static void buttonsInit() {
#if BTN_UP_PIN >= 0
#if BTN_USE_PULLUPS
  pinMode(BTN_UP_PIN, INPUT_PULLUP);
#else
  pinMode(BTN_UP_PIN, INPUT);
#endif
#endif
#if BTN_DOWN_PIN >= 0
#if BTN_USE_PULLUPS
  pinMode(BTN_DOWN_PIN, INPUT_PULLUP);
#else
  pinMode(BTN_DOWN_PIN, INPUT);
#endif
#endif
#if BTN_SELECT_PIN >= 0
#if BTN_USE_PULLUPS
  pinMode(BTN_SELECT_PIN, INPUT_PULLUP);
#else
  pinMode(BTN_SELECT_PIN, INPUT);
#endif
#endif
}

static bool buttonRawPressed(int8_t pin) {
  if (pin < 0) return false;
  return digitalRead(pin) == BTN_ACTIVE_STATE;
}

static void applyUiChange(int delta);
static void cycleChannelModeFromButton() {
  uint8_t nextMode = runtimeChannelMode + 1;
  if (nextMode > CHANNEL_MODE_SINGLE) nextMode = CHANNEL_MODE_FULL_HOP;
  runtimeChannelMode = nextMode;
  applyInitialChannel();
  snprintf(uiStatusLine, sizeof(uiStatusLine), "mode: %s", channelModeName());
  uiStatusUntilMs = millis() + 1400;
#if USE_AMOLED_DISPLAY
  amoledNeedsFullRedraw = true;
#elif USE_CARDPUTER_DISPLAY
  cardputerUiDirty = true;
#endif
}

#if USE_AMOLED_DISPLAY
static void touchToggleStealth() {
  stealthMode = !stealthMode;
  uiBuzzerMuted = stealthMode;
  strlcpy(uiStatusLine, stealthMode ? "stealth on" : "stealth off", sizeof(uiStatusLine));
  uiStatusUntilMs = millis() + 1200;
  amoledNeedsFullRedraw = true;
  if (stealthMode && uiDisplayReady && amoled) amoled->fillScreen(AMOLED_BLACK);
}

static void touchHandleTap(int16_t x, int16_t y) {
  if (y >= AMOLED_H - 82) {
    uint8_t tab = constrain(x / (AMOLED_W / 4), 0, 3);
    const uint8_t tabPages[] = {0, 2, 3, 6};
    uiPage = tabPages[tab];
    strlcpy(uiStatusLine, "tap page change", sizeof(uiStatusLine));
    uiStatusUntilMs = millis() + 1200;
    amoledNeedsFullRedraw = true;
  } else if (y < 62 && x > AMOLED_W - 110) {
    touchToggleStealth();
  } else if (uiMenuMode) {
    applyUiChange(1);
  }
}

static void touchHandleSwipe(int16_t dx, int16_t dy) {
  if (abs(dx) > abs(dy)) {
    uiPage = (uint8_t)((uiPage + (dx < 0 ? 1 : 6)) % 7);
    strlcpy(uiStatusLine, "swipe page change", sizeof(uiStatusLine));
    uiStatusUntilMs = millis() + 1200;
    amoledNeedsFullRedraw = true;
    return;
  }
  if (uiPage == 3) {
    strlcpy(uiStatusLine, dy < 0 ? "feed scroll down" : "feed scroll up", sizeof(uiStatusLine));
    uiStatusUntilMs = millis() + 1000;
  } else if (uiMenuMode) {
    applyUiChange(dy < 0 ? 1 : -1);
  }
}

static void touchPoll() {
  if (!uiTouchReady || !touchDevice) return;
  unsigned long now = millis();
  if (now - touchLastPollAt < 24) return;
  touchLastPollAt = now;

  int32_t fingers = touchDevice->IIC_Read_Device_Value(touchDevice->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
  if (fingers > 0) {
    int32_t rawX = touchDevice->IIC_Read_Device_Value(touchDevice->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t rawY = touchDevice->IIC_Read_Device_Value(touchDevice->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    int16_t x = 0;
    int16_t y = 0;
    amoledRemapTouch(rawX, rawY, x, y);
    if (!touchActive) {
      touchActive = true;
      touchHoldSent = false;
      touchDownAt = now;
      touchDownX = touchLastX = x;
      touchDownY = touchLastY = y;
      return;
    }
    int16_t dx = x - touchDownX;
    int16_t dy = y - touchDownY;
    if (!touchHoldSent && now - touchDownAt > BTN_LONGPRESS_MS && abs(dx) < 38 && abs(dy) < 38) {
      touchHoldSent = true;
      touchToggleStealth();
    }
    touchLastX = x;
    touchLastY = y;
    return;
  }

  if (!touchActive) return;
  touchActive = false;
  int16_t dx = touchLastX - touchDownX;
  int16_t dy = touchLastY - touchDownY;
  if (touchHoldSent) return;
  if (abs(dx) > 46 || abs(dy) > 46) touchHandleSwipe(dx, dy);
  else touchHandleTap(touchLastX, touchLastY);
}
#endif

static void applyUiChange(int delta) {
  if (!uiMenuMode) {
    uiPage = (uint8_t)((uiPage + (delta > 0 ? 1 : 6)) % 7);
#if USE_AMOLED_DISPLAY
    amoledNeedsFullRedraw = true;
#elif USE_CARDPUTER_DISPLAY
    cardputerUiDirty = true;
#endif
    return;
  }
  if (uiMenuIndex == 0) {
    int v = (int)runtimeChannelMode + delta;
    if (v < CHANNEL_MODE_FULL_HOP) v = CHANNEL_MODE_SINGLE;
    if (v > CHANNEL_MODE_SINGLE) v = CHANNEL_MODE_FULL_HOP;
    runtimeChannelMode = (uint8_t)v;
    applyInitialChannel();
    strlcpy(uiStatusLine, "channel mode updated", sizeof(uiStatusLine));
    uiStatusUntilMs = millis() + 1200;
#if USE_AMOLED_DISPLAY
    amoledNeedsFullRedraw = true;
#elif USE_CARDPUTER_DISPLAY
    cardputerUiDirty = true;
#endif
  } else if (uiMenuIndex == 1) {
    uiBuzzerMuted = (delta > 0) ? true : false;
    strlcpy(uiStatusLine, uiBuzzerMuted ? "buzzer muted" : "buzzer unmuted", sizeof(uiStatusLine));
    uiStatusUntilMs = millis() + 1200;
#if USE_CARDPUTER_DISPLAY
    cardputerUiDirty = true;
#endif
  } else {
    strlcpy(uiStatusLine, "returning launcher", sizeof(uiStatusLine));
    uiStatusUntilMs = millis() + 800;
#if USE_AMOLED_DISPLAY
    amoledNeedsFullRedraw = true;
#elif USE_CARDPUTER_DISPLAY
    cardputerUiDirty = true;
#endif
    fyReturnToLauncher(650);
  }
}

static void onShortPress(ButtonState* b) {
  if (b->pin == BTN_UP_PIN) {
    applyUiChange(1);
  } else if (b->pin == BTN_DOWN_PIN) {
    applyUiChange(-1);
  } else if (b->pin == BTN_SELECT_PIN) {
#if USE_AMOLED_DISPLAY
    cycleChannelModeFromButton();
#else
    if (!uiMenuMode) {
      uiMenuMode = true;
      uiMenuIndex = 0;
      strlcpy(uiStatusLine, "menu opened", sizeof(uiStatusLine));
    } else {
      uiMenuIndex++;
      if (uiMenuIndex >= uiMenuItems) {
        uiMenuMode = false;
        uiMenuIndex = 0;
        strlcpy(uiStatusLine, "menu closed", sizeof(uiStatusLine));
      } else {
        strlcpy(uiStatusLine, "next menu item", sizeof(uiStatusLine));
      }
    }
    uiStatusUntilMs = millis() + 1200;
#endif
  }
}

static void onLongPress(ButtonState* b) {
  if (b->pin == BTN_SELECT_PIN) {
    stealthMode = !stealthMode;
    uiBuzzerMuted = stealthMode;
    strlcpy(uiStatusLine, stealthMode ? "stealth on" : "stealth off", sizeof(uiStatusLine));
    uiStatusUntilMs = millis() + 1200;
#if USE_AMOLED_DISPLAY
    amoledNeedsFullRedraw = true;
    if (stealthMode && uiDisplayReady && amoled) amoled->fillScreen(AMOLED_BLACK);
#elif USE_CARDPUTER_DISPLAY
    cardputerUiDirty = true;
    if (stealthMode && uiDisplayReady) M5Cardputer.Display.fillScreen(CARD_BLACK);
#else
    if (stealthMode && uiDisplayReady) {
      display.clearDisplay();
      display.display();
    }
#endif
  }
}

static void pollButton(ButtonState* b) {
  bool rawNow = buttonRawPressed(b->pin);
  unsigned long now = millis();
  if (rawNow != b->raw) {
    b->raw = rawNow;
    b->changedAt = now;
  }
  if ((now - b->changedAt) < BTN_DEBOUNCE_MS) return;
  if (b->pressed != b->raw) {
    b->pressed = b->raw;
    if (b->pressed) {
      b->pressedAt = now;
    } else {
      unsigned long held = now - b->pressedAt;
      if (held >= BTN_LONGPRESS_MS) onLongPress(b);
      else onShortPress(b);
    }
  }
}

#if USE_CARDPUTER_DISPLAY
static void cardputerSelectShort() {
  if (!uiMenuMode) {
    uiMenuMode = true;
    uiMenuIndex = 0;
    strlcpy(uiStatusLine, "menu opened", sizeof(uiStatusLine));
  } else {
    uiMenuIndex++;
    if (uiMenuIndex >= uiMenuItems) {
      uiMenuMode = false;
      uiMenuIndex = 0;
      strlcpy(uiStatusLine, "menu closed", sizeof(uiStatusLine));
    } else {
      strlcpy(uiStatusLine, "next menu item", sizeof(uiStatusLine));
    }
  }
  uiStatusUntilMs = millis() + 1200;
  cardputerUiDirty = true;
}

static void cardputerToggleStealth() {
  stealthMode = !stealthMode;
  uiBuzzerMuted = stealthMode;
  strlcpy(uiStatusLine, stealthMode ? "stealth on" : "stealth off", sizeof(uiStatusLine));
  uiStatusUntilMs = millis() + 1200;
  cardputerUiDirty = true;
  if (stealthMode && uiDisplayReady) M5Cardputer.Display.fillScreen(CARD_BLACK);
}

static void cardputerPollKeyboard() {
  M5Cardputer.update();
  bool prev = false;
  bool next = false;
  bool select = M5Cardputer.BtnA.wasClicked();
  bool cycle = false;
  bool stealth = false;

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
    select = select || keys.enter;
    stealth = stealth || keys.del || keys.tab;
    for (auto c : keys.word) {
      if (c == ',' || c == ';' || c == 'w' || c == 'W' || c == 'k' || c == 'K') prev = true;
      if (c == '.' || c == '/' || c == 's' || c == 'S' || c == 'j' || c == 'J') next = true;
      if (c == 'c' || c == 'C' || c == 'm' || c == 'M') cycle = true;
      if (c == '`' || c == 'q' || c == 'Q') stealth = true;
    }
  }

  if (prev) applyUiChange(-1);
  if (next) applyUiChange(1);
  if (cycle) cycleChannelModeFromButton();
  if (select) cardputerSelectShort();
  if (stealth) cardputerToggleStealth();
}
#endif

static void buttonsPoll() {
#if USE_CARDPUTER_DISPLAY
  cardputerPollKeyboard();
  return;
#endif
#if BTN_UP_PIN >= 0
  pollButton(&btnUp);
#endif
#if BTN_DOWN_PIN >= 0
  pollButton(&btnDown);
#endif
#if BTN_SELECT_PIN >= 0
  pollButton(&btnSelect);
#endif
#if USE_AMOLED_DISPLAY
  touchPoll();
#endif
}

#if USE_AMOLED_DISPLAY
static void displayRender() {
  if (!uiDisplayReady || !amoled || stealthMode) return;
  unsigned long now = millis();
  if (now - uiLastRefreshAt < AMOLED_REFRESH_MS) return;
  uiLastRefreshAt = now;

  bool fullRedraw = amoledNeedsFullRedraw ||
                    amoledLastPage != uiPage ||
                    amoledLastMenuMode != uiMenuMode ||
                    amoledLastMenuIndex != uiMenuIndex ||
                    amoledLastStealthMode != stealthMode;
  amoledNeedsFullRedraw = false;
  amoledLastPage = uiPage;
  amoledLastMenuMode = uiMenuMode;
  amoledLastMenuIndex = uiMenuIndex;
  amoledLastStealthMode = stealthMode;

  if (fullRedraw) amoled->fillScreen(AMOLED_BLACK);
  amoledHeader("CYPHER FLOCK", fullRedraw);
  amoled->fillRect(0, 86, AMOLED_W, AMOLED_H - 136, AMOLED_BLACK);
  const int16_t yOff = 24;

  if (uiPage == 0) {
    amoledMetric(24, 78 + yOff, "WiFi", (unsigned long)sessionWifi, AMOLED_ACCENT);
    amoledMetric(142, 78 + yOff, "BLE", (unsigned long)sessionBle, AMOLED_OK);
    amoledMetric(250, 78 + yOff, "Raven", (unsigned long)sessionRaven, AMOLED_WARN);
    amoledPill(24, 178 + yOff, "AP", apReady);
    amoledPill(86, 178 + yOff, "SD", sdReady);
#if ENABLE_GPS
    amoledPill(148, 178 + yOff, "GPS", gps.location.isValid());
#else
    amoledPill(148, 178 + yOff, "GPS", false);
#endif
    amoledPrintf(24, 242 + yOff, 2, AMOLED_DIM, "Queue drops: %lu", (unsigned long)alertQueueDrops);
    amoledPrintf(24, 278 + yOff, 2, AMOLED_DIM, "Heap: %lu", (unsigned long)ESP.getFreeHeap());
  } else if (uiPage == 1) {
    amoledText(24, 76 + yOff, "Type        Session      Total", 2, AMOLED_DIM);
    amoledPrintf(24, 126 + yOff, 2, AMOLED_WHITE, "WiFi        %6lu   %8lu", (unsigned long)sessionWifi, (unsigned long)(lifetimeWifi + sessionWifi));
    amoledPrintf(24, 166 + yOff, 2, AMOLED_WHITE, "BLE         %6lu   %8lu", (unsigned long)sessionBle, (unsigned long)(lifetimeBle + sessionBle));
    amoledPrintf(24, 206 + yOff, 2, AMOLED_WHITE, "Raven       %6lu   %8lu", (unsigned long)sessionRaven, (unsigned long)(lifetimeRaven + sessionRaven));
  } else if (uiPage == 2) {
    char method[32];
    uiFitText(method, sizeof(method), uiLastMethod[0] ? uiLastMethod : "waiting for detection", 30);
    amoledText(24, 78 + yOff, method, 2, AMOLED_ACCENT);
    amoledText(24, 126 + yOff, uiLastMac[0] ? uiLastMac : "--:--:--:--:--:--", 3, AMOLED_WHITE);
    amoledPrintf(24, 190 + yOff, 2, AMOLED_DIM, "%s  %u%%  %ddBm  Ch%u",
                 uiLastConfidenceLabel, (unsigned)uiLastConfidence, uiLastRssi, (unsigned)uiLastChannel);
    amoledBar(24, 232 + yOff, 320, 24, uiLastConfidence, AMOLED_ACCENT);
  } else if (uiPage == 3) {
    amoledText(24, 72 + yOff, "Live Feed", 3, AMOLED_ACCENT);
    for (uint8_t i = 0; i < 5; i++) {
      char row[42];
      uiFitText(row, sizeof(row), uiLiveFeed[i][0] ? uiLiveFeed[i] : "waiting...", 39);
      amoledPrintf(24, 124 + yOff + i * 44, 2, i == 0 ? AMOLED_WHITE : AMOLED_DIM, "%u  %s", (unsigned)(i + 1), row);
    }
  } else if (uiPage == 4) {
    amoledText(24, 72 + yOff, "GPS / Storage", 3, AMOLED_ACCENT);
#if ENABLE_GPS
    if (gps.location.isValid()) {
      amoledPrintf(24, 128 + yOff, 2, AMOLED_WHITE, "Lat %.5f", gps.location.lat());
      amoledPrintf(24, 166 + yOff, 2, AMOLED_WHITE, "Lon %.5f", gps.location.lng());
      amoledPrintf(24, 204 + yOff, 2, AMOLED_DIM, "Sat %u  %.1fmph", gps.satellites.value(), gps.speed.mph());
    } else {
      amoledText(24, 128 + yOff, "GPS waiting for NMEA", 2, AMOLED_DIM);
    }
#else
    amoledText(24, 128 + yOff, "GPS not compiled", 2, AMOLED_DIM);
#endif
    char sdLine[42];
#if ENABLE_SD_LOGGING
    uiFitText(sdLine, sizeof(sdLine), sdReady ? currentLogFile.c_str() : sdStatusLine, 34);
#else
    strlcpy(sdLine, "not compiled", sizeof(sdLine));
#endif
    amoledPrintf(24, 230 + yOff, 2, sdReady ? AMOLED_OK : AMOLED_WARN, "SD: %s", sdLine);
    amoledPrintf(24, 268 + yOff, 2, fySpiffsReady ? AMOLED_OK : AMOLED_WARN, "LittleFS: %s", fySpiffsReady ? "ready" : "off");
  } else if (uiPage == 5) {
    amoledText(24, 72 + yOff, "Activity", 3, AMOLED_ACCENT);
    amoled->drawRoundRect(24, 124 + yOff, 320, 184, 8, AMOLED_DIM);
    for (uint8_t i = 0; i < 25; i++) {
      uint8_t idx = (activityBucketIndex + i + 1) % 25;
      uint8_t h = min((uint32_t)150, activityBuckets[idx] * 12);
      int16_t x = 36 + i * 12;
      amoled->fillRect(x, 288 + yOff - h, 7, h, AMOLED_ACCENT);
    }
    amoledText(34, 318 + yOff, "last 25 seconds", 2, AMOLED_DIM);
  } else {
    int rssiPct = map(constrain(uiLastRssi, -95, -35), -95, -35, 0, 100);
    amoledText(24, 72 + yOff, "Proximity", 3, AMOLED_ACCENT);
    amoledPrintf(24, 130 + yOff, 3, AMOLED_WHITE, "%d dBm", uiLastRssi);
    amoledBar(24, 188 + yOff, 320, 30, (uint8_t)rssiPct, rssiPct > 65 ? AMOLED_DANGER : AMOLED_ACCENT);
    amoledPrintf(24, 248 + yOff, 2, AMOLED_DIM, "%s %u%%", uiLastConfidenceLabel, (unsigned)uiLastConfidence);
    amoledPrintf(24, 286 + yOff, 2, AMOLED_DIM, "Stealth %s", stealthMode ? "ON" : "OFF");
  }

  if (uiMenuMode) {
    amoled->drawRoundRect(18, 336, 332, 48, 8, AMOLED_WARN);
    amoledPrintf(32, 352, 2, AMOLED_WARN, "Menu: %s", uiMenuLabel());
  }
  amoledFooter(now);
}
#elif USE_CARDPUTER_DISPLAY
static void displayRender() {
  if (!uiDisplayReady || stealthMode) return;
  unsigned long now = millis();
  if (!cardputerUiDirty && now - uiLastRefreshAt < CARDPUTER_REFRESH_MS) return;
  uiLastRefreshAt = now;
  cardputerUiDirty = false;

  M5Cardputer.Display.fillScreen(CARD_BLACK);
  int16_t h = M5Cardputer.Display.height();

  if (uiPage == 0) {
    cardHeader("DASHBOARD");
    cardMetric(8, 30, "WiFi", (unsigned long)sessionWifi, CARD_ACCENT);
    cardMetric(83, 30, "BLE", (unsigned long)sessionBle, CARD_OK);
    cardMetric(156, 30, "Raven", (unsigned long)sessionRaven, CARD_WARN);
    cardPill(8, 77, "AP", apReady);
    cardPill(44, 77, "SD", sdReady);
#if ENABLE_GPS
    cardPill(80, 77, "GPS", gps.location.isValid());
#else
    cardPill(80, 77, "GPS", false);
#endif
    cardPrintf(128, 80, 1, CARD_DIM, "Q:%lu  Heap:%lu",
               (unsigned long)alertQueueDrops, (unsigned long)(ESP.getFreeHeap() / 1024));
  } else if (uiPage == 1) {
    cardHeader("STATS");
    cardText(8, 30, "Type        Session      Total", 1, CARD_DIM);
    cardPrintf(8, 48, 1, CARD_WHITE, "WiFi        %6lu   %8lu", (unsigned long)sessionWifi, (unsigned long)(lifetimeWifi + sessionWifi));
    cardPrintf(8, 66, 1, CARD_WHITE, "BLE         %6lu   %8lu", (unsigned long)sessionBle, (unsigned long)(lifetimeBle + sessionBle));
    cardPrintf(8, 84, 1, CARD_WHITE, "Raven       %6lu   %8lu", (unsigned long)sessionRaven, (unsigned long)(lifetimeRaven + sessionRaven));
  } else if (uiPage == 2) {
    char method[28];
    uiFitText(method, sizeof(method), uiLastMethod[0] ? uiLastMethod : "waiting for detection", 26);
    cardHeader("LAST HIT");
    cardText(8, 30, method, 1, CARD_ACCENT);
    cardText(8, 49, uiLastMac[0] ? uiLastMac : "--:--:--:--:--:--", 2, CARD_WHITE);
    cardPrintf(8, 78, 1, CARD_DIM, "%s  %u%%  %ddBm  Ch%u",
               uiLastConfidenceLabel, (unsigned)uiLastConfidence, uiLastRssi, (unsigned)uiLastChannel);
    cardBar(8, 95, 150, 12, uiLastConfidence, CARD_ACCENT);
  } else if (uiPage == 3) {
    cardHeader("LIVE FEED");
    for (uint8_t i = 0; i < 5; i++) {
      char row[36];
      uiFitText(row, sizeof(row), uiLiveFeed[i][0] ? uiLiveFeed[i] : "waiting...", 34);
      cardPrintf(8, 29 + i * 17, 1, i == 0 ? CARD_WHITE : CARD_DIM, "%u  %s", (unsigned)(i + 1), row);
    }
  } else if (uiPage == 4) {
    cardHeader("STORAGE");
#if ENABLE_GPS
    if (gps.location.isValid()) {
      cardPrintf(8, 32, 1, CARD_WHITE, "Lat %.5f", gps.location.lat());
      cardPrintf(8, 49, 1, CARD_WHITE, "Lon %.5f", gps.location.lng());
    } else {
      cardText(8, 32, "GPS waiting for NMEA", 1, CARD_DIM);
    }
#else
    cardText(8, 32, "GPS not compiled", 1, CARD_DIM);
#endif
#if ENABLE_SD_LOGGING
    cardPrintf(8, 58, 1, sdReady ? CARD_OK : CARD_WARN, "SD: %s", sdReady ? currentLogFile.c_str() : sdStatusLine);
#else
    cardText(8, 58, "SD logging not compiled", 1, CARD_DIM);
#endif
    cardPrintf(8, 80, 1, fySpiffsReady ? CARD_OK : CARD_WARN, "LittleFS: %s", fySpiffsReady ? "ready" : "off");
  } else if (uiPage == 5) {
    cardHeader("ACTIVITY");
    M5Cardputer.Display.drawRoundRect(8, 32, 222, 67, 5, CARD_DIM);
    for (uint8_t i = 0; i < 25; i++) {
      uint8_t idx = (activityBucketIndex + i + 1) % 25;
      uint32_t rawH = activityBuckets[idx] * 6;
      uint8_t barH = rawH > 58 ? 58 : (uint8_t)rawH;
      int16_t x = 14 + i * 8;
      M5Cardputer.Display.fillRect(x, 95 - barH, 5, barH, CARD_ACCENT);
    }
    cardText(10, 104, "last 25 seconds", 1, CARD_DIM);
  } else {
    int rssiPct = map(constrain(uiLastRssi, -95, -35), -95, -35, 0, 100);
    cardHeader("PROXIMITY");
    cardPrintf(8, 36, 2, CARD_WHITE, "%d dBm", uiLastRssi);
    cardBar(8, 68, 214, 16, (uint8_t)rssiPct, rssiPct > 65 ? CARD_DANGER : CARD_ACCENT);
    cardPrintf(8, 94, 1, CARD_DIM, "%s %u%%  Stealth %s",
               uiLastConfidenceLabel, (unsigned)uiLastConfidence, stealthMode ? "ON" : "OFF");
  }

  if (uiMenuMode) {
    int16_t y = h - 38;
    M5Cardputer.Display.drawRoundRect(126, y, 106, 18, 3, CARD_WARN);
    cardPrintf(133, y + 5, 1, CARD_WARN, "%s", uiMenuLabel());
  }
  cardFooter(now);
}
#else
static void uiSetFontSmall(uint16_t color = SSD1306_WHITE) {
  uiFonts.setFontMode(1);
  uiFonts.setFontDirection(0);
  uiFonts.setFont(u8g2_font_5x7_tf);
  uiFonts.setForegroundColor(color);
  uiFonts.setBackgroundColor(color == SSD1306_BLACK ? SSD1306_WHITE : SSD1306_BLACK);
}

static void uiSetFontBody(uint16_t color = SSD1306_WHITE) {
  uiFonts.setFontMode(1);
  uiFonts.setFont(u8g2_font_6x10_tf);
  uiFonts.setForegroundColor(color);
  uiFonts.setBackgroundColor(color == SSD1306_BLACK ? SSD1306_WHITE : SSD1306_BLACK);
}

static void uiText(int16_t x, int16_t y, const char* text, uint16_t color = SSD1306_WHITE) {
  uiSetFontSmall(color);
  uiFonts.drawStr(x, y + 7, text);
}

static void uiPrintf(int16_t x, int16_t y, const char* fmt, ...) {
  char buf[40];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  uiText(x, y, buf);
}

static void uiHeader(const char* title) {
  display.fillRect(0, 0, OLED_W, 12, SSD1306_WHITE);
  uiSetFontSmall(SSD1306_BLACK);
  uiFonts.drawStr(3, 9, title);
  char page[6];
  snprintf(page, sizeof(page), "%u/7", (unsigned)(uiPage + 1));
  uiFonts.drawStr(106, 9, page);
}

static void uiFooter(unsigned long now) {
  display.drawFastHLine(0, 55, OLED_W, SSD1306_WHITE);
  char line[28];
  if (uiMenuMode) {
    snprintf(line, sizeof(line), "MENU %s  UP/DN edit", uiMenuShortLabel());
  } else if (uiStatusUntilMs > now) {
    uiFitText(line, sizeof(line), uiStatusLine, 24);
  } else {
    snprintf(line, sizeof(line), "%s Ch%u %s", sniffingPaused ? "PAUSED" : "SCAN", currentChannel, channelModeName());
  }
  uiSetFontSmall();
  uiFonts.drawStr(2, 63, line);
}

static void uiPill(int16_t x, int16_t y, const char* label, bool active) {
  int16_t w = (int16_t)(strlen(label) * 6 + 6);
  if (active) {
    display.fillRoundRect(x, y, w, 11, 2, SSD1306_WHITE);
    uiText(x + 3, y + 2, label, SSD1306_BLACK);
  } else {
    display.drawRoundRect(x, y, w, 11, 2, SSD1306_WHITE);
    uiText(x + 3, y + 2, label, SSD1306_WHITE);
  }
}

static void uiBar(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t pct) {
  pct = constrain(pct, 0, 100);
  display.drawRect(x, y, w, h, SSD1306_WHITE);
  int16_t fill = (int16_t)((w - 4) * pct / 100);
  if (fill > 0) display.fillRect(x + 2, y + 2, fill, h - 4, SSD1306_WHITE);
}

static void uiMetric(int16_t x, int16_t y, const char* label, unsigned long value) {
  uiSetFontSmall();
  uiFonts.drawStr(x, y + 7, label);
  uiFonts.setFont(u8g2_font_profont17_tf);
  uiFonts.setForegroundColor(SSD1306_WHITE);
  char val[12];
  snprintf(val, sizeof(val), "%lu", value);
  uiFonts.drawStr(x, y + 24, val);
}

static void displayRender() {
  if (!uiDisplayReady || stealthMode) return;
  unsigned long now = millis();
  if (now - uiLastRefreshAt < OLED_REFRESH_MS) return;
  uiLastRefreshAt = now;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (uiPage == 0) {
    uiHeader("DASHBOARD");
    uiMetric(2, 16, "WiFi", (unsigned long)sessionWifi);
    uiMetric(46, 16, "BLE", (unsigned long)sessionBle);
    uiMetric(86, 16, "Raven", (unsigned long)sessionRaven);
    uiPill(2, 43, "AP", apReady);
    uiPill(28, 43, "SD", sdReady);
#if ENABLE_GPS
    uiPill(54, 43, "GPS", gps.location.isValid());
#else
    uiPill(54, 43, "GPS", false);
#endif
    uiPrintf(87, 45, "Q:%lu", (unsigned long)alertQueueDrops);
  } else if (uiPage == 1) {
    uiHeader("STATS");
    uiText(2, 15, "Type      Session   Total");
    uiPrintf(2, 27, "WiFi      %5lu %7lu", (unsigned long)sessionWifi, (unsigned long)(lifetimeWifi + sessionWifi));
    uiPrintf(2, 37, "BLE       %5lu %7lu", (unsigned long)sessionBle, (unsigned long)(lifetimeBle + sessionBle));
    uiPrintf(2, 47, "Raven     %5lu %7lu", (unsigned long)sessionRaven, (unsigned long)(lifetimeRaven + sessionRaven));
  } else if (uiPage == 2) {
    uiHeader("LAST HIT");
    char method[20];
    uiFitText(method, sizeof(method), uiLastMethod[0] ? uiLastMethod : "waiting for detection", 19);
    uiText(2, 15, method);
    uiText(2, 26, uiLastMac[0] ? uiLastMac : "--:--:--:--:--:--");
    uiPrintf(2, 38, "%s  %u%%", uiLastConfidenceLabel, (unsigned)uiLastConfidence);
    uiPrintf(76, 38, "%ddBm", uiLastRssi);
    uiBar(2, 48, 62, 7, uiLastConfidence);
    uiPrintf(74, 48, "Ch %u", uiLastChannel);
  } else if (uiPage == 3) {
    uiHeader("LIVE FEED");
    for (uint8_t i = 0; i < 4; i++) {
      char row[21];
      uiFitText(row, sizeof(row), uiLiveFeed[i][0] ? uiLiveFeed[i] : "waiting...", 20);
      uiPrintf(2, 15 + i * 10, "%u %s", (unsigned)(i + 1), row);
    }
  } else if (uiPage == 4) {
    uiHeader("GPS");
#if ENABLE_GPS
    if (gps.location.isValid()) {
      uiPill(2, 15, "FIX", true);
      uiPrintf(36, 16, "Sat %u", gps.satellites.value());
      uiPrintf(2, 29, "Lat %.5f", gps.location.lat());
      uiPrintf(2, 39, "Lon %.5f", gps.location.lng());
      uiPrintf(2, 49, "%.1fmph  %.0fm", gps.speed.mph(), gps.altitude.meters());
    } else {
      uiPill(2, 15, "NO FIX", false);
      uiPrintf(2, 31, "Sat %u", gps.satellites.isValid() ? gps.satellites.value() : 0);
      uiText(2, 43, "Waiting for NMEA");
    }
#else
    uiPill(2, 15, "DISABLED", false);
    uiText(2, 33, "GPS not compiled");
#endif
  } else if (uiPage == 5) {
    uiHeader("ACTIVITY");
    display.drawRect(0, 15, 127, 38, SSD1306_WHITE);
    for (uint8_t i = 0; i < 25; i++) {
      uint8_t idx = (activityBucketIndex + i + 1) % 25;
      uint8_t h = min((uint32_t)32, activityBuckets[idx] * 4);
      int16_t x = 3 + i * 5;
      display.fillRect(x, 51 - h, 3, h, SSD1306_WHITE);
    }
    uiText(4, 17, "last 25s");
  } else {
    uiHeader("PROXIMITY");
    int rssiPct = map(constrain(uiLastRssi, -95, -35), -95, -35, 0, 100);
    uiPrintf(2, 17, "RSSI %d dBm", uiLastRssi);
    uiBar(2, 29, 124, 12, (uint8_t)rssiPct);
    uiPrintf(2, 45, "%s %u%%", uiLastConfidenceLabel, (unsigned)uiLastConfidence);
    uiPrintf(76, 45, "Buzz %s", uiBuzzerMuted ? "OFF" : "ON");
  }

  uiFooter(now);
  display.display();
}
#endif

// ============================================================
// SERIAL COMMAND FALLBACK
// ============================================================

static const char* uiPageName(uint8_t page) {
  switch (page) {
    case 0: return "dashboard";
    case 1: return "stats";
    case 2: return "last hit";
    case 3: return "live feed";
    case 4: return "GPS";
    case 5: return "activity";
    case 6: return "proximity";
    default: return "unknown";
  }
}

static void cmdPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void cmdPrintf(const char* fmt, ...) {
  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  dualPrintf("[cypher-flock:cmd] %s\n", buf);
}

static char* trimCommand(char* s) {
  while (*s && isspace((unsigned char)*s)) s++;
  char* end = s + strlen(s);
  while (end > s && isspace((unsigned char)*(end - 1))) *(--end) = '\0';
  return s;
}

static int splitCommand(char* cmd, char** argv, int maxArgs) {
  int argc = 0;
  char* p = cmd;
  while (*p && argc < maxArgs) {
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) break;
    argv[argc++] = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    if (*p) *p++ = '\0';
  }
  return argc;
}

static bool argEquals(const char* a, const char* b) {
  return a && b && strcasecmp(a, b) == 0;
}

static void serialPrintHelp() {
  cmdPrintf("commands: help, status, page [next|prev|0-6], menu");
  cmdPrintf("scan: mode full|custom|single, channel 1-13, scan pause|resume");
  cmdPrintf("controls: buzzer on|off, stealth on|off, gps, storage, detections");
  cmdPrintf("session: reset session, save, launcher, reboot");
}

static void serialPrintStatus() {
  const char* scanState = sniffingStopped ? "stopped" : (sniffingPaused ? "paused" : "running");
  cmdPrintf("profile=%s uptime=%lus heap=%lu scan=%s mode=%s channel=%u detections=%d",
            PROFILE_NAME, (unsigned long)(millis() / 1000), (unsigned long)ESP.getFreeHeap(),
            scanState, channelModeName(), (unsigned)currentChannel, fyDetCount);
  cmdPrintf("littlefs=%s sd=%s gps=%s display=%s stealth=%s buzzer=%s",
            fySpiffsReady ? "ready" : "off",
#if ENABLE_SD_LOGGING
            sdReady ? "ready" : "not-ready",
#else
            "not-compiled",
#endif
#if ENABLE_GPS
            gps.location.isValid() ? "lock" : "no-lock",
#else
            "not-compiled",
#endif
            uiDisplayReady ? "ready" : "not-ready",
            stealthMode ? "on" : "off",
            uiBuzzerMuted ? "muted" : "on");
#if ENABLE_POWER_STATUS
  powerRead(true);
  cmdPrintf("power=%s battery=%s percent=%d charging=%s vbus=%s batt_mv=%u vbus_mv=%u sys_mv=%u touch=%s",
            powerReady ? "ready" : "not-ready",
            powerBatteryPresent ? "present" : "missing",
            powerBatteryPercent,
            powerCharging ? "yes" : "no",
            powerVbusPresent ? "yes" : "no",
            (unsigned)powerBatteryMv,
            (unsigned)powerVbusMv,
            (unsigned)powerSystemMv,
            uiTouchReady ? "ready" : "not-ready");
#endif
}

static void serialPrintPage() {
  cmdPrintf("page=%u name=\"%s\"", (unsigned)uiPage, uiPageName(uiPage));
}

static void serialSetPage(const char* arg) {
  if (argEquals(arg, "next")) {
    uiPage = (uint8_t)((uiPage + 1) % 7);
  } else if (argEquals(arg, "prev")) {
    uiPage = (uint8_t)((uiPage + 6) % 7);
  } else if (arg && isdigit((unsigned char)arg[0])) {
    int page = atoi(arg);
    if (page < 0 || page > 6) {
      cmdPrintf("page must be 0-6");
      return;
    }
    uiPage = (uint8_t)page;
  } else {
    cmdPrintf("usage: page [next|prev|0-6]");
    return;
  }
  strlcpy(uiStatusLine, "serial page change", sizeof(uiStatusLine));
  uiStatusUntilMs = millis() + 1200;
  serialPrintPage();
}

static void serialPrintMenu() {
  cmdPrintf("channel mode=%s channel=%u single_channel=%u", channelModeName(),
            (unsigned)currentChannel, (unsigned)runtimeSingleChannel);
  cmdPrintf("buzzer=%s hardware=%s stealth=%s menu=%s",
            uiBuzzerMuted ? "muted" : "on",
#if USE_BUZZER
            "enabled",
#else
            "disabled",
#endif
            stealthMode ? "on" : "off",
            uiMenuMode ? "open" : "closed");
}

static void serialSetMode(const char* arg) {
  if (argEquals(arg, "full")) {
    runtimeChannelMode = CHANNEL_MODE_FULL_HOP;
  } else if (argEquals(arg, "custom")) {
    runtimeChannelMode = CHANNEL_MODE_CUSTOM;
  } else if (argEquals(arg, "single")) {
    runtimeChannelMode = CHANNEL_MODE_SINGLE;
  } else {
    cmdPrintf("usage: mode full|custom|single");
    return;
  }
  applyInitialChannel();
  strlcpy(uiStatusLine, "serial mode change", sizeof(uiStatusLine));
  uiStatusUntilMs = millis() + 1200;
  cmdPrintf("mode=%s channel=%u", channelModeName(), (unsigned)currentChannel);
}

static void serialSetChannel(const char* arg) {
  if (!arg || !isdigit((unsigned char)arg[0])) {
    cmdPrintf("usage: channel 1-13");
    return;
  }
  int channel = atoi(arg);
  if (channel < 1 || channel > 13) {
    cmdPrintf("channel must be 1-13");
    return;
  }
  runtimeSingleChannel = (uint8_t)channel;
  runtimeChannelMode = CHANNEL_MODE_SINGLE;
  applyInitialChannel();
  strlcpy(uiStatusLine, "serial channel set", sizeof(uiStatusLine));
  uiStatusUntilMs = millis() + 1200;
  cmdPrintf("mode=%s channel=%u", channelModeName(), (unsigned)currentChannel);
}

static void serialSetBuzzer(const char* arg) {
  if (argEquals(arg, "on")) {
    uiBuzzerMuted = false;
  } else if (argEquals(arg, "off")) {
    uiBuzzerMuted = true;
  } else {
    cmdPrintf("usage: buzzer on|off");
    return;
  }
#if USE_BUZZER
  cmdPrintf("buzzer=%s", uiBuzzerMuted ? "muted" : "on");
#else
  cmdPrintf("buzzer=%s hardware=disabled", uiBuzzerMuted ? "muted" : "on");
#endif
}

static void serialSetStealth(const char* arg) {
  if (argEquals(arg, "on")) {
    stealthMode = true;
    uiBuzzerMuted = true;
#if USE_AMOLED_DISPLAY
    if (uiDisplayReady && amoled) amoled->fillScreen(AMOLED_BLACK);
#elif USE_CARDPUTER_DISPLAY
    cardputerUiDirty = true;
    if (uiDisplayReady) M5Cardputer.Display.fillScreen(CARD_BLACK);
#else
    if (uiDisplayReady) {
      display.clearDisplay();
      display.display();
    }
#endif
  } else if (argEquals(arg, "off")) {
    stealthMode = false;
    #if USE_CARDPUTER_DISPLAY
    cardputerUiDirty = true;
    #endif
  } else {
    cmdPrintf("usage: stealth on|off");
    return;
  }
  strlcpy(uiStatusLine, stealthMode ? "stealth on" : "stealth off", sizeof(uiStatusLine));
  uiStatusUntilMs = millis() + 1200;
  cmdPrintf("stealth=%s buzzer=%s", stealthMode ? "on" : "off", uiBuzzerMuted ? "muted" : "on");
}

static void serialPrintGps() {
#if ENABLE_GPS
  if (gps.location.isValid()) {
    cmdPrintf("gps=fix satellites=%lu lat=%.6f lon=%.6f speed_mph=%.1f altitude_m=%.1f",
              (unsigned long)(gps.satellites.isValid() ? gps.satellites.value() : 0),
              gps.location.lat(), gps.location.lng(), gps.speed.mph(), gps.altitude.meters());
  } else {
    cmdPrintf("gps=no-fix satellites=%lu waiting=NMEA",
              (unsigned long)(gps.satellites.isValid() ? gps.satellites.value() : 0));
  }
#else
  cmdPrintf("GPS not compiled");
#endif
}

static void serialPrintStorage() {
  cmdPrintf("littlefs=%s detections=%d dirty=%s last_save_count=%d",
            fySpiffsReady ? "ready" : "off", fyDetCount, fyDirty ? "yes" : "no", fyLastSaveCount);
#if ENABLE_SD_LOGGING
  cmdPrintf("sd=compiled ready=%s status=\"%s\" type=%s size_mb=%llu log=%s",
            sdReady ? "yes" : "no", sdStatusLine, sdCardTypeName(sdCardTypeValue),
            (unsigned long long)(sdCardSizeBytes / (1024ULL * 1024ULL)),
            currentLogFile.length() ? currentLogFile.c_str() : "-");
#else
  cmdPrintf("sd=not-compiled");
#endif
}

static void serialPrintDetections() {
  cmdPrintf("detections=%d wifi=%lu ble=%lu raven=%lu", fyDetCount,
            (unsigned long)sessionWifi, (unsigned long)sessionBle, (unsigned long)sessionRaven);
  if (uiLastMac[0]) {
    cmdPrintf("last mac=%s method=%s protocol=%s rssi=%d channel=%u confidence=%u label=%s name=\"%s\"",
              uiLastMac, uiLastMethod, uiLastProtocol, uiLastRssi,
              (unsigned)uiLastChannel, (unsigned)uiLastConfidence, uiLastConfidenceLabel, uiLastName);
  } else {
    cmdPrintf("last=none");
  }
}

static void serialResetSession() {
  fyDetCount = 0;
  fyDirty = true;
  sessionWifi = sessionBle = sessionRaven = 0;
  memset(uiLastMac, 0, sizeof(uiLastMac));
  memset(uiLastMethod, 0, sizeof(uiLastMethod));
  memset(uiLastName, 0, sizeof(uiLastName));
  memset(uiLastProtocol, 0, sizeof(uiLastProtocol));
  memset(uiLiveFeed, 0, sizeof(uiLiveFeed));
  strlcpy(uiLastConfidenceLabel, "LOW", sizeof(uiLastConfidenceLabel));
  uiLastRssi = 0;
  uiLastChannel = 0;
  uiLastConfidence = 0;
  strlcpy(uiStatusLine, "session reset", sizeof(uiStatusLine));
  uiStatusUntilMs = millis() + 1200;
  cmdPrintf("session reset");
}

static void serialExecuteCommand(char* raw) {
  char* cmd = trimCommand(raw);
  if (!*cmd || *cmd == '{') return;

  char* argv[4] = {nullptr, nullptr, nullptr, nullptr};
  int argc = splitCommand(cmd, argv, 4);
  if (argc == 0) return;

  if (argEquals(argv[0], "help")) {
    serialPrintHelp();
  } else if (argEquals(argv[0], "status")) {
    serialPrintStatus();
  } else if (argEquals(argv[0], "page")) {
    if (argc == 1) serialPrintPage();
    else serialSetPage(argv[1]);
  } else if (argEquals(argv[0], "menu")) {
    serialPrintMenu();
  } else if (argEquals(argv[0], "mode")) {
    serialSetMode(argc > 1 ? argv[1] : nullptr);
  } else if (argEquals(argv[0], "channel")) {
    serialSetChannel(argc > 1 ? argv[1] : nullptr);
  } else if (argEquals(argv[0], "scan")) {
    if (argEquals(argc > 1 ? argv[1] : nullptr, "pause")) setSniffPaused(true);
    else if (argEquals(argc > 1 ? argv[1] : nullptr, "resume")) setSniffPaused(false);
    else { cmdPrintf("usage: scan pause|resume"); return; }
    cmdPrintf("scan=%s", sniffingPaused ? "paused" : "running");
  } else if (argEquals(argv[0], "buzzer")) {
    serialSetBuzzer(argc > 1 ? argv[1] : nullptr);
  } else if (argEquals(argv[0], "stealth")) {
    serialSetStealth(argc > 1 ? argv[1] : nullptr);
  } else if (argEquals(argv[0], "gps")) {
    serialPrintGps();
  } else if (argEquals(argv[0], "storage")) {
    serialPrintStorage();
  } else if (argEquals(argv[0], "detections")) {
    serialPrintDetections();
  } else if (argEquals(argv[0], "reset") && argEquals(argc > 1 ? argv[1] : nullptr, "session")) {
    serialResetSession();
  } else if (argEquals(argv[0], "save")) {
    if (!fySpiffsReady) {
      cmdPrintf("save skipped: LittleFS not ready");
    } else {
      fyDirty = true;
      fySaveSession();
      cmdPrintf("save requested");
    }
  } else if (argEquals(argv[0], "launcher") || argEquals(argv[0], "return")) {
    cmdPrintf("returning to launcher");
    fyReturnToLauncher(250);
  } else if (argEquals(argv[0], "reboot")) {
    cmdPrintf("rebooting");
    delay(100);
    ESP.restart();
  } else {
    cmdPrintf("unknown command: %s (try help)", argv[0]);
  }
}

static void serialCommandTick() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') {
      serialCmdBuf[serialCmdLen] = '\0';
      serialExecuteCommand(serialCmdBuf);
      serialCmdLen = 0;
      serialSawCr = true;
    } else if (c == '\n') {
      if (serialSawCr) {
        serialSawCr = false;
        continue;
      }
      serialCmdBuf[serialCmdLen] = '\0';
      serialExecuteCommand(serialCmdBuf);
      serialCmdLen = 0;
    } else if (c == '\b' || c == 127) {
      serialSawCr = false;
      if (serialCmdLen > 0) serialCmdLen--;
    } else {
      serialSawCr = false;
      if (serialCmdLen + 1 >= sizeof(serialCmdBuf)) {
        serialCmdLen = 0;
        serialCmdBuf[0] = '\0';
        cmdPrintf("command too long");
      } else {
        serialCmdBuf[serialCmdLen++] = c;
      }
    }
  }
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
#if USE_AMOLED_DISPLAY
  unsigned long serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < USB_SERIAL_WAIT_MS) delay(10);
#endif
  delay(300);
  dualPrintln("[cypher-flock] setup begin");
  dualPrintf("[cypher-flock] profile=%s chip=%s heap=%lu psram=%lu\n",
             PROFILE_NAME, ESP.getChipModel(), (unsigned long)ESP.getFreeHeap(),
             (unsigned long)ESP.getPsramSize());

#if MIRROR_SERIAL
  Serial1.begin(MIRROR_BAUD, SERIAL_8N1, -1, MIRROR_TX_PIN);  // TX-only on GPIO43
#endif

#if USE_BUZZER
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
#endif

#if USE_LED
#if USE_RGB_LED
  rgbLed.begin();
  rgbLed.setBrightness(255);
  ledSet(false);
#else
  pinMode(LED_PIN, OUTPUT);
  ledSet(false);
#endif
#endif

  startupBeep();
#if USE_LED
  ledBootPulse(200);
#endif

  precompileOuis();
  memset(dedupeTable, 0, sizeof(dedupeTable));
  buttonsInit();
  dualPrintln("[cypher-flock] before displayInit");
  displayInit();
  dualPrintln("[cypher-flock] after displayInit");

  // SPIFFS — format on first boot if missing. Non-fatal if it fails.
  dualPrintln("[cypher-flock] before LittleFS");
  if (SPIFFS.begin(true)) {
    fySpiffsReady = true;
    dualPrintln("[cypher-flock] LittleFS ready");
    fyPromotePrevSession();
    lifetimeLoad();
  } else {
    dualPrintln("[cypher-flock] LittleFS init FAILED - running without persistence");
  }
  dualPrintln("[cypher-flock] before storageInit");
  storageInit();
  dualPrintln("[cypher-flock] after storageInit");

#if ENABLE_GPS
  SerialGPS.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
#endif

  WiFi.mode(WIFI_AP);
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_AP);
  esp_wifi_start();
  webInit();

  applyInitialChannel();

  wifi_promiscuous_filter_t filt = {
    .filter_mask = 0
#if PROCESS_MGMT_FRAMES
        | WIFI_PROMIS_FILTER_MASK_MGMT
#endif
#if PROCESS_DATA_FRAMES
        | WIFI_PROMIS_FILTER_MASK_DATA
#endif
  };
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&wifiSniffer);
  esp_wifi_set_promiscuous(true);
  bleInit();

  dualPrintln("[cypher-flock] v2 WiFi/BLE detector started");
  dualPrintf("[cypher-flock] profile=%s mode=%s primary_dwell=%u secondary_dwell=%u start_channel=%u rssi_min=%d littlefs=%d\n",
                PROFILE_NAME, channelModeName(), CHANNEL_DWELL_PRIMARY_MS,
                CHANNEL_DWELL_SECONDARY_MS, currentChannel, RSSI_MIN, fySpiffsReady ? 1 : 0);

  lastHeartbeat = millis();
  fyLastSaveAt  = millis();
  sessionStartMs = millis();
  lastLifetimeSaveAt = millis();
  lastActivityBucketAt = millis();
#if USE_RGB_LED
  rgbLastHeartbeatAt = millis();
#endif
}

void loop() {
  updateChannelMode();
  bleTick();
  drainAlertQueue();   // Serial.printf happens here, not in callback
  autosaveTick();      // periodic SPIFFS write if dirty
  lifetimeTick();
  activityTick();
  gpsTick();
  serialCommandTick();
  webServer.handleClient();
  rssiTrackExpire();
  heartbeatTick();     // audible beep-pair while a target is still in range
  buttonsPoll();
  displayRender();
  ledTick();           // turn off LED after LED_FLASH_MS
  printHeartbeat();
  delay(1);
}
