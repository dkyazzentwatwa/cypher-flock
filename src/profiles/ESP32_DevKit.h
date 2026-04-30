#pragma once

#define PROFILE_NAME "ESP32 DevKit"

#define BUZZER_PIN 27
#define USE_BUZZER 0

#define LED_PIN          27
#define USE_LED          1
#define LED_ACTIVE_HIGH  1
#define LED_FLASH_MS     120

#define MIRROR_SERIAL    0
#define MIRROR_TX_PIN    17
#define MIRROR_BAUD      115200

#define BTN_UP_PIN       34
#define BTN_DOWN_PIN     36
#define BTN_SELECT_PIN   39
#define BTN_ACTIVE_STATE LOW
#define BTN_DEBOUNCE_MS  35
#define BTN_LONGPRESS_MS 800
#define BTN_USE_PULLUPS  0

#define OLED_SDA_PIN     5
#define OLED_SCL_PIN     4
#define OLED_ADDR        0x3C
#define OLED_W           128
#define OLED_H           64
#define OLED_RESET       -1
#define OLED_REFRESH_MS  200

#define ENABLE_SD_LOGGING 0
#define SD_CS_PIN        2
#define SD_MOSI_PIN      -1
#define SD_MISO_PIN      -1
#define SD_SCK_PIN       -1

#define ENABLE_GPS       0
#define GPS_RX_PIN       16
#define GPS_TX_PIN       17
#define GPS_BAUD         9600

#define AP_SSID          "CypherFlock"
#define AP_PASSWORD      "flockpass"
#define AP_CHANNEL       6
