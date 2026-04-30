#pragma once

#define ESP32_DEVKIT 1
#define ESP32_S3     2
#define ESP32_CYPHERBOX 3

#ifndef BOARD_PROFILE
#error "BOARD_PROFILE must be set. Use -DBOARD_PROFILE=ESP32_DEVKIT, -DBOARD_PROFILE=ESP32_S3, or -DBOARD_PROFILE=ESP32_CYPHERBOX"
#endif

#if BOARD_PROFILE == ESP32_DEVKIT
#include "profiles/ESP32_DevKit.h"
#elif BOARD_PROFILE == ESP32_S3
#include "profiles/ESP32_S3.h"
#elif BOARD_PROFILE == ESP32_CYPHERBOX
#include "profiles/Cypherbox.h"
#else
#error "Unknown BOARD_PROFILE"
#endif

#define CHANNEL_MODE_FULL_HOP   0
#define CHANNEL_MODE_CUSTOM     1
#define CHANNEL_MODE_SINGLE     2

#define CHANNEL_MODE CHANNEL_MODE_FULL_HOP
#define CHANNEL_DWELL_PRIMARY_MS 500
#define CHANNEL_DWELL_SECONDARY_MS 200
#define SINGLE_CHANNEL 1

#define HEARTBEAT_MS    30000
#define RSSI_MIN        -95
#define ALERT_COOLDOWN_MS 5000

#define HB_DEVICE_ACTIVE_MS    3000
#define HB_BEEP_INTERVAL_MS    10000
#define REDISCOVER_MS          300000
#define NEW_CHIRP_LO_HZ        2000
#define NEW_CHIRP_HI_HZ        2800
#define NEW_CHIRP_NOTE_MS      55
#define NEW_CHIRP_GAP_MS       25
#define HB_BEEP_HZ             1500
#define HB_BEEP_NOTE_MS        70
#define HB_BEEP_GAP_MS         70

#define ENABLE_SSID_MATCH 1
#define CHECK_ADDR1 1
#define CHECK_ADDR3 0
#define STOP_ON_SSID_HIT 0
#define STOP_ON_OUI_HIT  0
#define PROCESS_MGMT_FRAMES 1
#define PROCESS_DATA_FRAMES 1

#define MAX_DETECTIONS       200
#define FY_SESSION_FILE      "/session.json"
#define FY_SESSION_TMP       "/session.tmp"
#define FY_PREV_FILE         "/prev_session.json"
#define FY_LIFETIME_FILE     "/flock_session.dat"
#define AUTOSAVE_INTERVAL_MS 60000

#define BLE_SCAN_DURATION_S 2
#define BLE_SCAN_INTERVAL_MS 3000

#define CONF_MAC_PREFIX       40
#define CONF_SSID_PATTERN     50
#define CONF_SSID_FLOCK_FMT   65
#define CONF_BLE_NAME         45
#define CONF_MFG_ID           60
#define CONF_RAVEN_UUID       70
#define CONF_RAVEN_MULTI_UUID 90
#define CONF_PENGUIN_SERIAL   80
#define CONF_PENGUIN_NUM      15
#define CONF_BONUS_STRONG_RSSI 10
#define CONF_BONUS_MULTI_METHOD 20
#define CONF_BONUS_BLE_STATIC_ADDR 10
#define CONF_BONUS_STATIONARY 15
#define CONFIDENCE_ALARM_THRESHOLD 40
#define CONFIDENCE_HIGH 70
#define CONFIDENCE_CERTAIN 85

#define AP_WEB_SERVER_PORT 80
