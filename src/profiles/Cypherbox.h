#pragma once

#define PROFILE_NAME "Cypherbox"

#define BUZZER_PIN 26
#define USE_BUZZER 0

#define LED_PIN          26
#define USE_LED          1
#define LED_ACTIVE_HIGH  1
#define LED_FLASH_MS     120

#define MIRROR_SERIAL    0
#define MIRROR_TX_PIN    17
#define MIRROR_BAUD      115200

#define BTN_UP_PIN       34
#define BTN_DOWN_PIN     35
#define BTN_SELECT_PIN   15
#define BTN_HOME_PIN     2
#define BTN_ACTIVE_STATE LOW
#define BTN_DEBOUNCE_MS  35
#define BTN_LONGPRESS_MS 800
#define BTN_USE_PULLUPS  0

#define OLED_SDA_PIN     21
#define OLED_SCL_PIN     22
#define OLED_ADDR        0x3C
#define OLED_W           128
#define OLED_H           64
#define OLED_RESET       -1
#define OLED_REFRESH_MS  200

#define ENABLE_SD_LOGGING 1
#define SD_CS_PIN        5
#define SD_MOSI_PIN      23
#define SD_MISO_PIN      19
#define SD_SCK_PIN       18

#define ENABLE_GPS       1
#define GPS_RX_PIN       16
#define GPS_TX_PIN       17
#define GPS_BAUD         9600

#define ENABLE_RFID      0
#define RFID_RST_PIN     25
#define RFID_SS_PIN      27
#define RFID_MOSI_PIN    23
#define RFID_MISO_PIN    19
#define RFID_SCK_PIN     18

#define AP_SSID          "CypherFlock"
#define AP_PASSWORD      "flockpass"
#define AP_CHANNEL       6
