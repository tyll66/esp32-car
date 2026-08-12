#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"

/* ================= 全局 ================= */

#define APP_TAG "DESK_CAR_MAIN"

/* ================= 手机热点 STA 配置 ================= */

#define WIFI_STA_SSID      "ty"
#define WIFI_STA_PASS      "12345678"

/* ================= 本机 SoftAP 配置 ================= */

#define WIFI_AP_SSID       "DesktopCar_AP"
#define WIFI_AP_PASS       "12345678"
#define WIFI_AP_CHANNEL    6
#define WIFI_AP_MAX_CONN   2

/* 另一个 ESP32 发控制信息到这个 UDP 端口 */
#define WIFI_REMOTE_UDP_PORT 3333

/* ================= STM32 UART 配置 ================= */

#define STM32_UART_PORT    UART_NUM_1
#define STM32_UART_TX_GPIO GPIO_NUM_43
#define STM32_UART_RX_GPIO GPIO_NUM_44
#define STM32_UART_BAUD    115200

/* ================= LCD 引脚 ================= */

#define LCD_PIN_BL      GPIO_NUM_16
#define LCD_PIN_CS      GPIO_NUM_10
#define LCD_PIN_MOSI    GPIO_NUM_11
#define LCD_PIN_SCLK    GPIO_NUM_12
#define LCD_PIN_DC      GPIO_NUM_14
#define LCD_PIN_RST     GPIO_NUM_21

#define LCD_H_RES       240
#define LCD_V_RES       240
#define LCD_SPI_HOST    SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ  (20 * 1000 * 1000)
#define LCD_BITS_PER_PIXEL  16

/* ================= ES8311 I2C 引脚 ================= */

#define ES8311_I2C_PORT      I2C_NUM_0
#define ES8311_I2C_SDA       GPIO_NUM_8
#define ES8311_I2C_SCL       GPIO_NUM_9
#define ES8311_I2C_FREQ_HZ   100000

/* ================= ES8311 I2S 引脚 ================= */

#define AUDIO_I2S_PORT      I2S_NUM_0
#define AUDIO_MCLK_GPIO     GPIO_NUM_38
#define AUDIO_BCLK_GPIO     GPIO_NUM_39
#define AUDIO_LRCK_GPIO     GPIO_NUM_40
#define AUDIO_DOUT_GPIO     GPIO_NUM_42
#define AUDIO_DIN_GPIO      GPIO_NUM_41

#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_BITS          16
#define AUDIO_CHANNELS      2

#define PA_ENABLE_GPIO      GPIO_NUM_NC

/* ================= SPIFFS ================= */

#define SPIFFS_BASE_PATH    "/spiffs"
#define SPIFFS_LABEL        "storage"

#define LELE_WAKE_ACK_PCM   "/spiffs/awake.pcm"
#define LELE_SLEEP_ACK_PCM  "/spiffs/leave.pcm"

/* ================= 豆包配置 ================= */

#define DOUBAO_WS_URL        "wss://openspeech.bytedance.com/api/v3/realtime/dialogue"

/*
 * 不建议继续硬编码真实密钥。
 * 先用占位，后续改成 menuconfig 或 NVS。
 */
#define DOUBAO_APP_ID        "7610812747"
#define DOUBAO_ACCESS_KEY    "DgVHW_28T23S7-ZPMzzmw6wCrIXoMSwX"
#define DOUBAO_RESOURCE_ID   "volc.speech.dialog"
#define DOUBAO_APP_KEY       "PlgvMymc7f3tQnJ6"
#define DOUBAO_CONNECT_ID    "esp32s3-voice-car-001"

#define ASSISTANT_NAME_CN    "乐乐"
#define WAKE_WORD_CN         "乐乐"