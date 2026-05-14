#pragma once

#define PROFILE_NAME "M5Stack Cardputer ADV"

#define USE_CARDPUTER_DISPLAY 1
#define CARDPUTER_BRIGHTNESS 170
#define CARDPUTER_REFRESH_MS 180

#define BUZZER_PIN -1
#define USE_BUZZER 0

#define LED_PIN          -1
#define USE_LED          0
#define LED_ACTIVE_HIGH  1
#define LED_FLASH_MS     120

#define MIRROR_SERIAL    0
#define MIRROR_TX_PIN    -1
#define MIRROR_BAUD      115200

#define BTN_UP_PIN       -1
#define BTN_DOWN_PIN     -1
#define BTN_SELECT_PIN   -1
#define BTN_ACTIVE_STATE LOW
#define BTN_DEBOUNCE_MS  35
#define BTN_LONGPRESS_MS 800
#define BTN_USE_PULLUPS  0

#define OLED_SDA_PIN     -1
#define OLED_SCL_PIN     -1
#define OLED_ADDR        0x3C
#define OLED_W           240
#define OLED_H           135
#define OLED_RESET       -1
#define OLED_REFRESH_MS  CARDPUTER_REFRESH_MS

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
