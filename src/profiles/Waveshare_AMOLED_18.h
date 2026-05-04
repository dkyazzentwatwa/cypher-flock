#pragma once

#define PROFILE_NAME "Waveshare ESP32-S3 AMOLED 1.8"

#ifndef ARDUINO_USB_MODE
#define ARDUINO_USB_MODE 0
#endif
#ifndef ARDUINO_USB_CDC_ON_BOOT
#define ARDUINO_USB_CDC_ON_BOOT 1
#endif

#define BUZZER_PIN -1
#define USE_BUZZER 0

#define LED_PIN          -1
#define USE_LED          0
#define LED_ACTIVE_HIGH  1
#define LED_FLASH_MS     120

#define MIRROR_SERIAL    0
#define MIRROR_TX_PIN    43
#define MIRROR_BAUD      115200

#define BTN_UP_PIN       -1
#define BTN_DOWN_PIN     -1
#define BTN_SELECT_PIN   0
#define BTN_ACTIVE_STATE LOW
#define BTN_DEBOUNCE_MS  35
#define BTN_LONGPRESS_MS 800
#define BTN_USE_PULLUPS  1

#define USE_AMOLED_DISPLAY 1
#define USE_TOUCH_INPUT    1
#define ENABLE_POWER_STATUS 1

#define AMOLED_W          368
#define AMOLED_H          448
#define AMOLED_ROTATION   0
#define AMOLED_BRIGHTNESS 255
#define AMOLED_REFRESH_MS 120
#define USB_SERIAL_WAIT_MS 15000

#define LCD_SDIO0         4
#define LCD_SDIO1         5
#define LCD_SDIO2         6
#define LCD_SDIO3         7
#define LCD_SCLK          11
#define LCD_CS            12
#define LCD_RST           -1

#define TOUCH_SDA_PIN     15
#define TOUCH_SCL_PIN     14
#define TOUCH_INT_PIN     21

#define OLED_SDA_PIN      TOUCH_SDA_PIN
#define OLED_SCL_PIN      TOUCH_SCL_PIN
#define OLED_ADDR         0x3C
#define OLED_W            AMOLED_W
#define OLED_H            AMOLED_H
#define OLED_RESET        -1
#define OLED_REFRESH_MS   AMOLED_REFRESH_MS

#define ENABLE_SD_LOGGING 1
#define USE_SD_MMC        1
#define SD_CS_PIN         -1
#define SD_MOSI_PIN       -1
#define SD_MISO_PIN       -1
#define SD_SCK_PIN        -1
#define SDMMC_CLK_PIN     2
#define SDMMC_CMD_PIN     1
#define SDMMC_D0_PIN      3

#define ENABLE_GPS        0
#define GPS_RX_PIN        -1
#define GPS_TX_PIN        -1
#define GPS_BAUD          9600

#define AP_SSID           "CypherFlock"
#define AP_PASSWORD       "flockpass"
#define AP_CHANNEL        6
