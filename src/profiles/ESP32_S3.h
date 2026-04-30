#pragma once

#define PROFILE_NAME "ESP32-S3"

#define BUZZER_PIN 3
#define USE_BUZZER 1

#define LED_PIN          21
#define USE_LED          1
#define LED_ACTIVE_HIGH  0
#define LED_FLASH_MS     120

#define MIRROR_SERIAL    1
#define MIRROR_TX_PIN    43
#define MIRROR_BAUD      115200

#define BTN_UP_PIN       4
#define BTN_DOWN_PIN     5
#define BTN_SELECT_PIN   6
#define BTN_ACTIVE_STATE LOW
#define BTN_DEBOUNCE_MS  35
#define BTN_LONGPRESS_MS 800
#define BTN_USE_PULLUPS  1

#define OLED_SDA_PIN     8
#define OLED_SCL_PIN     9
#define OLED_ADDR        0x3C
#define OLED_W           128
#define OLED_H           64
#define OLED_RESET       -1
#define OLED_REFRESH_MS  200

#define ENABLE_SD_LOGGING 0
#define SD_CS_PIN        -1
#define SD_MOSI_PIN      -1
#define SD_MISO_PIN      -1
#define SD_SCK_PIN       -1

#define ENABLE_GPS       0
#define GPS_RX_PIN       -1
#define GPS_TX_PIN       -1
#define GPS_BAUD         9600

#define AP_SSID          "CypherFlock"
#define AP_PASSWORD      "flockpass"
#define AP_CHANNEL       6
