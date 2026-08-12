/*
 * desk_top_car_doubao_realtime.c
 * 已按豆包端到端实时语音大模型 Realtime WebSocket 方案合并。
 *
 * 功能：
 * 1. ESP32-S3 本地录音 + VAD
 * 2. 通过 WebSocket 连接豆包 Realtime API
 * 3. 上传 16kHz/16bit/mono PCM 音频
 * 4. 接收豆包返回的 PCM TTS 音频
 * 5. 降采样 24kHz -> 16kHz 后用 ES8311 播放
 *
 * 仍需填写：
 * DOUBAO_APP_ID / DOUBAO_ACCESS_KEY
 */


#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/ringbuf.h"

#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"

/* ================= 豆包 Realtime WebSocket include ================= */
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_netif_sntp.h"
#include <time.h>
#include <sys/time.h>

#define TAG "AUDIO_LCD_TEST"

#define WIFI_SSID      "ty"
#define WIFI_PASS      "12345678"


/* ================= 豆包端到端实时语音大模型 Realtime API 配置 =================
 *
 * 只需要填 DOUBAO_APP_ID 和 DOUBAO_ACCESS_KEY。
 * 不要把完整 Access Key / Token 发到聊天或公开仓库。
 *
 * WebSocket 接口文档要求：
 * URL: wss://openspeech.bytedance.com/api/v3/realtime/dialogue
 * X-Api-Resource-Id: volc.speech.dialog
 * X-Api-App-Key: 请填你的 DOUBAO_APP_KEY
 */
#define DOUBAO_WS_URL        "wss://openspeech.bytedance.com/api/v3/realtime/dialogue"
#define DOUBAO_APP_ID        "7610812747"
#define DOUBAO_ACCESS_KEY    "DgVHW_28T23S7-ZPMzzmw6wCrIXoMSwX"
#define DOUBAO_RESOURCE_ID   "volc.speech.dialog"
#define DOUBAO_APP_KEY       "PlgvMymc7f3tQnJ6"
#define DOUBAO_CONNECT_ID    "esp32s3-voice-car-001"
#define DOUBAO_TTS_PREBUFFER_BYTES   (4 * 1024)
/*
 * 1.2.1.1 = O2.0版本
 * 2.2.0.0 = SC2.0版本
 */
#define DOUBAO_MODEL_VERSION "1.2.1.1"

/*
 * 官方音色。先用 vv，后续可改成其它支持的音色。
 */
#define DOUBAO_SPEAKER       "zh_female_vv_jupiter_bigtts"

/*
 * ESP32 当前 ES8311 初始化为 16kHz。
 * 豆包返回 pcm_s16le 时文档要求 24kHz，所以本文件会把 24kHz 降采样为 16kHz 播放。
 */
#define DOUBAO_TTS_SAMPLE_RATE      16000
#define DOUBAO_TTS_FORMAT           "pcm_s16le"

/*
 * 录音发送策略：
 * - ESP32 本地 VAD 先录到一句话
 * - 建立 WebSocket
 * - 用 push_to_talk 模式按 20ms 一包发送
 * - 发送 EndASR
 * - 接收模型音频并播放
 */
#define DOUBAO_RECORD_MS            1200

/* ================= 乐乐唤醒与交互参数 =================
 *
 * 待机状态：只监听唤醒词“乐乐”，不播放云端闲聊回复。
 * 唤醒后：播放 /spiffs/awake.pcm。
 * 退出/撤回时：播放 /spiffs/leave.pcm。
 * 本地音频只保留 awake.pcm / leave.pcm。
 */
#define ASSISTANT_NAME_CN             "乐乐"
#define WAKE_WORD_CN                  "乐乐"
#define LELE_WAKE_RECORD_MS           2500
#define LELE_ACTIVE_RECORD_MS         5000
#define LELE_STANDBY_DELAY_MS         300
#define LELE_WAKE_ACK_PCM             "/spiffs/awake.pcm"
#define LELE_SLEEP_ACK_PCM            "/spiffs/leave.pcm"
#define DOUBAO_SUPPRESS_TTS_FOR_COMMAND 1
#define DOUBAO_FRAME_MS             20
#define DOUBAO_FRAME_SAMPLES        320
#define DOUBAO_FRAME_BYTES          (DOUBAO_FRAME_SAMPLES * sizeof(int16_t))
#define DOUBAO_STEREO_BYTES         (DOUBAO_FRAME_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t))

#define DOUBAO_TTS_PREBUFFER_TIMEOUT_MS   10000
#define DOUBAO_TTS_IDLE_TIMEOUT_MS        90000
#define DOUBAO_SESSION_TIMEOUT_MS           3000
#define DOUBAO_TTS_FIRST_PACKET_TIMEOUT_MS  8000
#define DOUBAO_TTS_TOTAL_TIMEOUT_MS       60000
#define DOUBAO_TTS_RINGBUF_BYTES    (8 * 1024)
#define DOUBAO_MAX_WS_FRAME_BYTES   (32 * 1024)

/*
 * 云端 TTS 边写边播模式：
 * - WebSocket 回调把豆包返回的 PCM 写入 SPIFFS 临时文件
 * - 播放端预缓存一小段后，从同一个临时文件边读边播
 * - 这样长文本/唱歌不会把内部 RAM ringbuf 打满
 */
#define DOUBAO_CLOUD_TTS_STREAM_PCM       "/spiffs/cloud_tts_stream.pcm"
#define DOUBAO_TTS_FILE_PREBUFFER_BYTES   (192 * 1024)
#define DOUBAO_TTS_FILE_MAX_BYTES         (1536 * 1024)
#define DOUBAO_TTS_FILE_MIN_PLAY_BYTES    (128 * 1024)
#define DOUBAO_TTS_FILE_FLUSH_BYTES       (16 * 1024)
#define DOUBAO_TTS_FILE_RESUME_BYTES      (64 * 1024)

/* ================= SPI LCD 引脚 ================= */

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
#define ES8311_I2C_SDA      GPIO_NUM_8
#define ES8311_I2C_SCL      GPIO_NUM_9
#define ES8311_I2C_FREQ_HZ  100000

/* ================= ES8311 I2S 引脚 ================= */

/*
 * 这里按 ESP32-S3 侧命名：
 * I2S_DOUT：ESP32-S3 -> ES8311 DIN
 * I2S_DIN ：ES8311 DOUT -> ESP32-S3
 */
#define AUDIO_I2S_PORT      I2S_NUM_0
#define AUDIO_MCLK_GPIO     GPIO_NUM_38
#define AUDIO_BCLK_GPIO     GPIO_NUM_39
#define AUDIO_LRCK_GPIO     GPIO_NUM_40
#define AUDIO_DOUT_GPIO     GPIO_NUM_42
#define AUDIO_DIN_GPIO      GPIO_NUM_41

#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_BITS          16
#define AUDIO_CHANNELS      2

#define AUDIO_FRAME_SAMPLES 256
#define AUDIO_FRAME_BYTES   (AUDIO_FRAME_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t))

/*
 * 没有给 NS4150B 的 EN/CTRL 引脚，所以这里不控制功放使能。
 * 如果你的 NS4150B 有 SHDN/CTRL 引脚，需要单独接 GPIO 并拉高。
 */
#define PA_ENABLE_GPIO      GPIO_NUM_NC

/* ================= DRV8876 电机控制引脚 ================= */

/*
 * 默认约定：
 * MOTOR1 = 左侧电机
 * MOTOR2 = 右侧电机
 *
 * 如果实际左右反了，后面 left/right 控制里对调即可。
 */
#define MOTOR1_PWM_GPIO     GPIO_NUM_5
#define MOTOR1_DIR_GPIO     GPIO_NUM_4

#define MOTOR2_PWM_GPIO     GPIO_NUM_1
#define MOTOR2_DIR_GPIO     GPIO_NUM_2

#define MOTOR_NSLEEP_GPIO   GPIO_NUM_18

#define MOTOR_PWM_FREQ_HZ   20000
#define MOTOR_PWM_RES       LEDC_TIMER_10_BIT
#define MOTOR_PWM_MAX_DUTY  1023

#define MOTOR_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define MOTOR_LEDC_TIMER    LEDC_TIMER_0

#define MOTOR1_LEDC_CH      LEDC_CHANNEL_0
#define MOTOR2_LEDC_CH      LEDC_CHANNEL_1
#define MOTOR_TURN_DURATION_MS   1500
/*
 * DRV8876 DIR 电平定义。
 * 这里定义的是驱动芯片输入层面的正反，不直接代表小车物理前进方向。
 */
#define MOTOR_DIR_FORWARD_LEVEL  1
#define MOTOR_DIR_REVERSE_LEVEL  0

/*
 * 电机物理方向修正。
 * 上一版实际测试：两侧电机都反了，语音“前进”变成了后退。
 * 这里整体反向：MOTOR1=左侧，MOTOR2=右侧。
 * 如果后续重新接线导致方向又反了，只改这两个极性，不要改控制函数。
 */
#define MOTOR1_POLARITY          (1)
#define MOTOR2_POLARITY          (-1)

/*
 * 默认电机动作参数。
 * 速度单位：百分比，范围 0~100。
 * 时长单位：ms。
 */
#define MOTOR_TEST_SPEED_PERCENT 85
#define MOTOR_TEST_DURATION_MS   2500
#define MOTOR_CMD_MAX_DURATION_MS 5000

/* ================= 电机异步命令队列 =================
 *
 * 语音/网络任务只负责投递电机命令；motor_task 独立执行动作。
 * 这样播放 TTS、等待 WebSocket、录音等逻辑不会被电机动作时间阻塞。
 */
typedef enum {
    MOTOR_CMD_STOP = 0,
    MOTOR_CMD_FORWARD,
    MOTOR_CMD_BACKWARD,
    MOTOR_CMD_LEFT,
    MOTOR_CMD_RIGHT,
    MOTOR_CMD_PATROL,
} motor_cmd_type_t;

typedef struct {
    motor_cmd_type_t type;
    int speed;
    int duration_ms;
} motor_cmd_t;

static QueueHandle_t motor_cmd_queue = NULL;

/* ================= LCD 状态显示队列 =================
 * 只有 lcd_status_task 真正操作 SPI LCD。
 * voice_cloud_task / motor_task 等任务只投递状态字符串。
 */
typedef struct {
    char text[24];
} lcd_status_msg_t;

static QueueHandle_t lcd_status_queue = NULL;


static esp_lcd_panel_handle_t lcd_panel = NULL;
static esp_lcd_panel_io_handle_t lcd_io = NULL;

static i2s_chan_handle_t i2s_tx_chan = NULL;
static i2s_chan_handle_t i2s_rx_chan = NULL;
static esp_codec_dev_handle_t codec_dev = NULL;
static i2c_master_bus_handle_t i2c_bus_handle = NULL;



/* ================= 简易 LCD 状态文字显示 ================= */

#define FONT_W 5
#define FONT_H 7

static const uint8_t GLYPH_SPACE[7]    = {0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t GLYPH_QUESTION[7] = {0x0E,0x11,0x01,0x02,0x04,0x00,0x04};
static const uint8_t GLYPH_DASH[7]     = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
static const uint8_t GLYPH_COLON[7]    = {0x00,0x04,0x04,0x00,0x04,0x04,0x00};
static const uint8_t GLYPH_DOT[7]      = {0x00,0x00,0x00,0x00,0x00,0x06,0x06};

static const uint8_t FONT_DIGITS[10][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E},
    {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E},
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E},
};

static const uint8_t FONT_LETTERS[26][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E},
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    {0x01,0x01,0x01,0x01,0x11,0x11,0x0E},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
};

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) |
           ((g & 0xFC) << 3) |
           ((b & 0xF8) >> 3);
}

static const uint8_t *font5x7_get_glyph(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = c - 'a' + 'A';
    }

    if (c >= 'A' && c <= 'Z') {
        return FONT_LETTERS[c - 'A'];
    }

    if (c >= '0' && c <= '9') {
        return FONT_DIGITS[c - '0'];
    }

    switch (c) {
    case ' ': return GLYPH_SPACE;
    case '-': return GLYPH_DASH;
    case ':': return GLYPH_COLON;
    case '.': return GLYPH_DOT;
    default:  return GLYPH_QUESTION;
    }
}

static void lcd_fill_rect(int x1, int y1, int x2, int y2, uint16_t color)
{
    if (lcd_panel == NULL) {
        return;
    }

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > LCD_H_RES) x2 = LCD_H_RES;
    if (y2 > LCD_V_RES) y2 = LCD_V_RES;

    int w = x2 - x1;
    int h = y2 - y1;
    if (w <= 0 || h <= 0) {
        return;
    }

    const int chunk_h = 16;
    uint16_t *buf = heap_caps_malloc(w * chunk_h * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (buf == NULL) {
        ESP_LOGE(TAG, "LCD fill buf malloc failed");
        return;
    }

    for (int i = 0; i < w * chunk_h; i++) {
        buf[i] = color;
    }

    int remain = h;
    int cur_y = y1;

    while (remain > 0) {
        int draw_h = remain > chunk_h ? chunk_h : remain;
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(lcd_panel, x1, cur_y, x2, cur_y + draw_h, buf));
        cur_y += draw_h;
        remain -= draw_h;
    }

    free(buf);
}

static void lcd_draw_char(int x, int y, char c, uint16_t color, int scale)
{
    const uint8_t *glyph = font5x7_get_glyph(c);

    for (int row = 0; row < FONT_H; row++) {
        for (int col = 0; col < FONT_W; col++) {
            if (glyph[row] & (1 << (FONT_W - 1 - col))) {
                int px = x + col * scale;
                int py = y + row * scale;
                lcd_fill_rect(px, py, px + scale, py + scale, color);
            }
        }
    }
}

static void lcd_draw_string(int x, int y, const char *str, uint16_t color, int scale)
{
    if (str == NULL) {
        return;
    }

    int cursor_x = x;
    int step = (FONT_W + 1) * scale;

    while (*str) {
        lcd_draw_char(cursor_x, y, *str, color, scale);
        cursor_x += step;
        str++;
    }
}

static int lcd_text_width_5x7(const char *text, int scale)
{
    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    int len = (int)strlen(text);
    return len * FONT_W * scale + (len - 1) * scale;
}

static void lcd_show_center_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        text = "READY";
    }

    int len = (int)strlen(text);
    int scale = 5;

    if (len > 8) {
        scale = 4;
    }

    if (len > 12) {
        scale = 3;
    }

    uint16_t bg = rgb565(0, 0, 0);
    uint16_t fg = rgb565(0, 255, 120);

    lcd_fill_rect(0, 0, LCD_H_RES, LCD_V_RES, bg);

    int text_w = lcd_text_width_5x7(text, scale);
    int text_h = FONT_H * scale;

    int x = (LCD_H_RES - text_w) / 2;
    int y = (LCD_V_RES - text_h) / 2;

    if (x < 0) x = 0;
    if (y < 0) y = 0;

    lcd_draw_string(x, y, text, fg, scale);
}

static void lcd_status_post(const char *text)
{
    if (lcd_status_queue == NULL || text == NULL) {
        return;
    }

    lcd_status_msg_t msg;
    memset(&msg, 0, sizeof(msg));

    strncpy(msg.text, text, sizeof(msg.text) - 1);
    msg.text[sizeof(msg.text) - 1] = '\0';

    xQueueOverwrite(lcd_status_queue, &msg);
}

static void lcd_status_task(void *arg)
{
    (void)arg;

    lcd_status_msg_t msg;
    char last_text[sizeof(msg.text)] = {0};

    lcd_show_center_text("BOOT");

    while (1) {
        if (xQueueReceive(lcd_status_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (strcmp(last_text, msg.text) == 0) {
            continue;
        }

        strncpy(last_text, msg.text, sizeof(last_text) - 1);
        last_text[sizeof(last_text) - 1] = '\0';

        lcd_show_center_text(msg.text);
    }
}

/* ================= LCD 初始化 ================= */

static void lcd_backlight_init(void)
{
    gpio_config_t bl_conf = {
        .pin_bit_mask = (1ULL << LCD_PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&bl_conf));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BL, 1));

    ESP_LOGI(TAG, "LCD backlight ON");
}

static void lcd_init(void)
{
    ESP_LOGI(TAG, "Initialize SPI LCD");

    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * 20 * sizeof(uint16_t),
    };

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
        &io_config,
        &lcd_io
    ));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(lcd_io, &panel_config, &lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(lcd_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(lcd_panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(lcd_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd_panel, true));

    ESP_LOGI(TAG, "LCD init done");
}

/* ================= 音频初始化 ================= */

static void audio_i2c_init(void)
{
    i2c_master_bus_config_t i2c_bus_conf = {
        .i2c_port = ES8311_I2C_PORT,
        .sda_io_num = ES8311_I2C_SDA,
        .scl_io_num = ES8311_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_conf, &i2c_bus_handle));

    ESP_LOGI(TAG, "I2C master bus init done: SDA GPIO%d, SCL GPIO%d",
             ES8311_I2C_SDA, ES8311_I2C_SCL);
}

static void audio_i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx_chan, &i2s_rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_MCLK_GPIO,
            .bclk = AUDIO_BCLK_GPIO,
            .ws = AUDIO_LRCK_GPIO,
            .dout = AUDIO_DOUT_GPIO,
            .din = AUDIO_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx_chan, &std_cfg));

    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx_chan));

    ESP_LOGI(TAG, "I2S init done");
    ESP_LOGI(TAG, "MCLK GPIO%d, BCLK GPIO%d, LRCK GPIO%d, DOUT GPIO%d, DIN GPIO%d",
             AUDIO_MCLK_GPIO, AUDIO_BCLK_GPIO, AUDIO_LRCK_GPIO, AUDIO_DOUT_GPIO, AUDIO_DIN_GPIO);
}

static void audio_codec_init(void)
{
    audio_i2c_init();
    audio_i2s_init();

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = AUDIO_I2S_PORT,
        .rx_handle = i2s_rx_chan,
        .tx_handle = i2s_tx_chan,
    };

    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (data_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data failed");
        abort();
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .bus_handle = i2c_bus_handle,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
    };

    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed");
        abort();
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_gpio failed");
        abort();
    }

    es8311_codec_cfg_t es8311_cfg = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .pa_pin = PA_ENABLE_GPIO,
        .use_mclk = true,
    };

    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    if (codec_if == NULL) {
        ESP_LOGE(TAG, "es8311_codec_new failed");
        abort();
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec_if,
        .data_if = data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
    };

    codec_dev = esp_codec_dev_new(&dev_cfg);
    if (codec_dev == NULL) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        abort();
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .channel = AUDIO_CHANNELS,
        .bits_per_sample = AUDIO_BITS,
    };

    ESP_ERROR_CHECK(esp_codec_dev_open(codec_dev, &fs));

    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(codec_dev, 80.0));
    ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(codec_dev, 30.0));

    ESP_LOGI(TAG, "ES8311 codec init done");
}

static void wifi_init_sta(void);
static void voice_cloud_task(void *arg);
static void lcd_status_task(void *arg);
static void lcd_status_post(const char *text);

static void spiffs_init(void);
static void play_local_pcm_file(const char *path);

static int clamp_int(int value, int min_value, int max_value);
static void time_sync_init(void);

static void motor_init(void);
static void motor_stop(void);
static void motor_forward(int speed_percent);
static void motor_backward(int speed_percent);
static void motor_left(int speed_percent);
static void motor_right(int speed_percent);
static void motor_task(void *arg);
static void motor_send_cmd(motor_cmd_type_t type, int speed, int duration_ms);
static bool doubao_try_execute_voice_command(const char *text);
static bool text_has_wake_word(const char *text);
static bool text_has_sleep_word(const char *text);

/* ================= app_main ================= */
void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 Doubao Realtime voice start");

    lcd_backlight_init();
    lcd_init();

    lcd_status_queue = xQueueCreate(1, sizeof(lcd_status_msg_t));
    if (lcd_status_queue == NULL) {
        ESP_LOGE(TAG, "lcd_status_queue create failed");
        abort();
    }

    xTaskCreate(lcd_status_task, "lcd_status_task", 4096, NULL, 4, NULL);
    lcd_status_post("BOOT");

    lcd_status_post("AUDIO");
    audio_codec_init();

    lcd_status_post("SPIFFS");
    spiffs_init();

    lcd_status_post("MOTOR");
    motor_init();

    motor_cmd_queue = xQueueCreate(1, sizeof(motor_cmd_t));
    if (motor_cmd_queue == NULL) {
        ESP_LOGE(TAG, "motor_cmd_queue create failed");
        abort();
    }

    xTaskCreate(motor_task, "motor_task", 4096, NULL, 6, NULL);

    lcd_status_post("WIFI");
    wifi_init_sta();

    lcd_status_post("TIME");
    time_sync_init();

    lcd_status_post("READY");
    xTaskCreate(voice_cloud_task, "voice_cloud_task", 16384, NULL, 5, NULL);
}

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL,
        NULL
    ));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_event_handler,
        NULL,
        NULL
    ));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 关闭 WiFi 省电，降低音频流卡顿概率 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "WiFi connecting to %s...", WIFI_SSID);

    xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY
    );

    ESP_LOGI(TAG, "WiFi connected");
}

static void time_sync_init(void)
{
    ESP_LOGI(TAG, "SNTP time sync start");

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    esp_err_t ret = esp_netif_sntp_init(&config);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(ret));
    }

    ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SNTP sync timeout: %s", esp_err_to_name(ret));
    }

    time_t now = 0;
    struct tm timeinfo = {0};

    time(&now);
    localtime_r(&now, &timeinfo);

    ESP_LOGI(TAG,
             "Current time: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900,
             timeinfo.tm_mon + 1,
             timeinfo.tm_mday,
             timeinfo.tm_hour,
             timeinfo.tm_min,
             timeinfo.tm_sec);

    if (timeinfo.tm_year + 1900 < 2024) {
        ESP_LOGW(TAG, "System time still invalid, TLS may fail");
    }
}

#define PCM_MONO_SAMPLES_PER_CHUNK    512
#define PCM_MONO_BYTES_PER_CHUNK      (PCM_MONO_SAMPLES_PER_CHUNK * sizeof(int16_t))
#define PCM_STEREO_BYTES_PER_CHUNK    (PCM_MONO_SAMPLES_PER_CHUNK * 2 * sizeof(int16_t))

/* ================= 麦克风上传参数 ================= */

#define MIC_DEFAULT_RECORD_MS         3000
#define MIC_MAX_RECORD_MS             2500

#define MIC_FRAME_SAMPLES             512
#define MIC_STEREO_BYTES              (MIC_FRAME_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t))
#define MIC_MONO_BYTES                (MIC_FRAME_SAMPLES * sizeof(int16_t))
/* ================= ESP32 端简单 VAD 参数 ================= */

#define MIC_VAD_START_AVG        1200
#define MIC_VAD_START_PEAK       4500

#define MIC_VAD_STOP_AVG         220
#define MIC_VAD_STOP_PEAK        1200

#define MIC_MIN_VOICE_MS         120
#define MIC_END_SILENCE_MS       300

static int16_t g_tts_stereo_buf[PCM_MONO_SAMPLES_PER_CHUNK * 2];

#define TTS_SILENCE_SAMPLES 320 

static int16_t g_tts_silence_buf[TTS_SILENCE_SAMPLES * 2];

static void play_tts_silence_20ms(void)
{
    esp_codec_dev_write(codec_dev,
                        g_tts_silence_buf,
                        sizeof(g_tts_silence_buf));
}
static void mono_to_stereo(const int16_t *mono, int16_t *stereo, int samples)
{
    for (int i = 0; i < samples; i++) {
        int16_t s = mono[i];
        stereo[i * 2 + 0] = s;
        stereo[i * 2 + 1] = s;
    }
}
static void stereo_to_mono_left(const int16_t *stereo, int16_t *mono, int samples)
{
    for (int i = 0; i < samples; i++) {
        /*
         * ES8311 当前 I2S 是 stereo。
         * 这里先取左声道。
         * stereo[i * 2 + 0] = L
         * stereo[i * 2 + 1] = R
         */
        mono[i] = stereo[i * 2 + 0];
    }
}

static bool mic_frame_has_voice(const int16_t *mono, int samples, int *avg_out, int *peak_out)
{
    int64_t sum = 0;
    int peak = 0;

    for (int i = 0; i < samples; i++) {
        int v = mono[i];
        if (v < 0) {
            v = -v;
        }

        sum += v;

        if (v > peak) {
            peak = v;
        }
    }

    int avg = samples > 0 ? (int)(sum / samples) : 0;

    if (avg_out) {
        *avg_out = avg;
    }

    if (peak_out) {
        *peak_out = peak;
    }

    return avg >= MIC_VAD_START_AVG || peak >= MIC_VAD_START_PEAK;
}
static void spiffs_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 8,
        .format_if_mount_failed = false,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        abort();
    }

    size_t total = 0;
    size_t used = 0;

    ret = esp_spiffs_info("storage", &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS info failed: %s", esp_err_to_name(ret));
        abort();
    }

    ESP_LOGI(TAG, "SPIFFS mounted, total=%d, used=%d", (int)total, (int)used);
}


static void play_local_pcm_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "open local pcm failed: %s", path);
        return;
    }

    ESP_LOGI(TAG, "play local pcm: %s", path);

    int16_t *mono_buf = heap_caps_malloc(PCM_MONO_BYTES_PER_CHUNK, MALLOC_CAP_INTERNAL);
    int16_t *stereo_buf = heap_caps_malloc(PCM_STEREO_BYTES_PER_CHUNK, MALLOC_CAP_INTERNAL);

    if (mono_buf == NULL || stereo_buf == NULL) {
        ESP_LOGE(TAG, "local pcm buffer malloc failed");
        free(mono_buf);
        free(stereo_buf);
        fclose(fp);
        return;
    }

    /*
     * 播放前写一点静音，避免上一次输出残留。
     */
    memset(stereo_buf, 0, PCM_STEREO_BYTES_PER_CHUNK);
    for (int i = 0; i < 4; i++) {
        esp_codec_dev_write(codec_dev, stereo_buf, PCM_STEREO_BYTES_PER_CHUNK);
    }

    while (1) {
        size_t n = fread(mono_buf, 1, PCM_MONO_BYTES_PER_CHUNK, fp);
        if (n == 0) {
            break;
        }

        int samples = n / sizeof(int16_t);

        mono_to_stereo(mono_buf, stereo_buf, samples);

        int stereo_bytes = samples * 2 * sizeof(int16_t);

        esp_err_t ret = esp_codec_dev_write(codec_dev, stereo_buf, stereo_bytes);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "write local pcm failed: %s", esp_err_to_name(ret));
        }
    }

    /*
     * 播放后补一点静音，避免功放停在非零电平。
     */
    memset(stereo_buf, 0, PCM_STEREO_BYTES_PER_CHUNK);
    for (int i = 0; i < 6; i++) {
        esp_codec_dev_write(codec_dev, stereo_buf, PCM_STEREO_BYTES_PER_CHUNK);
    }

    free(mono_buf);
    free(stereo_buf);
    fclose(fp);

    ESP_LOGI(TAG, "local pcm done: %s", path);
}

static int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }

    if (value > max_value) {
        return max_value;
    }

    return value;
}


static void motor_set_one(ledc_channel_t channel, gpio_num_t dir_gpio, int speed_percent)
{
    speed_percent = clamp_int(speed_percent, -100, 100);

    if (speed_percent >= 0) {
        gpio_set_level(dir_gpio, MOTOR_DIR_FORWARD_LEVEL);
    } else {
        gpio_set_level(dir_gpio, MOTOR_DIR_REVERSE_LEVEL);
        speed_percent = -speed_percent;
    }

    uint32_t duty = (uint32_t)(speed_percent * MOTOR_PWM_MAX_DUTY / 100);

    ESP_ERROR_CHECK(ledc_set_duty(MOTOR_LEDC_MODE, channel, duty));
    ESP_ERROR_CHECK(ledc_update_duty(MOTOR_LEDC_MODE, channel));
}


static void motor_set_left(int speed_percent)
{
    motor_set_one(MOTOR1_LEDC_CH, MOTOR1_DIR_GPIO, speed_percent * MOTOR1_POLARITY);
}


static void motor_set_right(int speed_percent)
{
    motor_set_one(MOTOR2_LEDC_CH, MOTOR2_DIR_GPIO, speed_percent * MOTOR2_POLARITY);
}


static void motor_stop(void)
{
    lcd_status_post("STOP");
    ESP_ERROR_CHECK(ledc_set_duty(MOTOR_LEDC_MODE, MOTOR1_LEDC_CH, 0));
    ESP_ERROR_CHECK(ledc_update_duty(MOTOR_LEDC_MODE, MOTOR1_LEDC_CH));

    ESP_ERROR_CHECK(ledc_set_duty(MOTOR_LEDC_MODE, MOTOR2_LEDC_CH, 0));
    ESP_ERROR_CHECK(ledc_update_duty(MOTOR_LEDC_MODE, MOTOR2_LEDC_CH));

    ESP_LOGI(TAG, "Motor stop");
}


static void motor_forward(int speed_percent)
{
    lcd_status_post("FORWARD");
    /*
     * 左侧电机已通过 MOTOR1_POLARITY 做了物理方向修正。
     * 这里的 speed_percent > 0 表示“小车前进”。
     */
    motor_set_left(speed_percent);
    motor_set_right(speed_percent);

    ESP_LOGI(TAG, "Motor forward, speed=%d", speed_percent);
}


static void motor_backward(int speed_percent)
{
    lcd_status_post("BACKWARD");
    motor_set_left(-speed_percent);
    motor_set_right(-speed_percent);

    ESP_LOGI(TAG, "Motor backward, speed=%d", speed_percent);
}


static void motor_left(int speed_percent)
{
    lcd_status_post("LEFT");
    /*
     * 原地左转：左轮倒转，右轮正转。
     * 经过 MOTOR1_POLARITY 修正后，这里表达的是小车物理运动方向。
     */
    motor_set_left(speed_percent);
    motor_set_right(-speed_percent);

    ESP_LOGI(TAG, "Motor left spin, speed=%d", speed_percent);
}


static void motor_right(int speed_percent)
{
    lcd_status_post("RIGHT");
    /*
     * 原地右转：左轮正转，右轮倒转。
     */
    motor_set_left(-speed_percent);
    motor_set_right(speed_percent);

    ESP_LOGI(TAG, "Motor right spin, speed=%d", speed_percent);
}

static bool text_has_any(const char *text, const char *a, const char *b, const char *c)
{
    if (text == NULL) {
        return false;
    }

    if (a != NULL && strstr(text, a) != NULL) {
        return true;
    }

    if (b != NULL && strstr(text, b) != NULL) {
        return true;
    }

    if (c != NULL && strstr(text, c) != NULL) {
        return true;
    }

    return false;
}


static bool text_has_wake_word(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    /*
     * ASR 可能把“乐乐”识别成带空格、重复字或近音词。
     * 先做宽松匹配，后续可根据串口 ASR 日志继续补充。
     */
    if (strstr(text, WAKE_WORD_CN) != NULL ||
        strstr(text, "乐 乐") != NULL ||
        strstr(text, "勒勒") != NULL ||
        strstr(text, "了了") != NULL ||
        strstr(text, "乐了") != NULL ||
        strstr(text, "小乐") != NULL ||
        strstr(text, "lele") != NULL ||
        strstr(text, "Lele") != NULL ||
        strstr(text, "LELE") != NULL) {
        return true;
    }

    return false;
}

static bool text_has_sleep_word(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    /*
     * 手动退出活跃模式关键词。
     * 只有识别到这些词，才播放 leave.pcm 并回到待机；
     * 不再因为活跃监听超时自动退出。
     */
    if (strstr(text, "没事") != NULL ||
        strstr(text, "没事了") != NULL ||
        strstr(text, "不用了") != NULL ||
        strstr(text, "可以了") != NULL ||
        strstr(text, "退下") != NULL ||
        strstr(text, "撤了") != NULL ||
        strstr(text, "撤吧") != NULL ||
        strstr(text, "休息") != NULL ||
        strstr(text, "睡觉") != NULL ||
        strstr(text, "拜拜") != NULL ||
        strstr(text, "再见") != NULL ||
        strstr(text, "关闭语音") != NULL ||
        strstr(text, "退出") != NULL) {
        return true;
    }

    return false;
}

static bool doubao_try_execute_voice_command(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    int speed = MOTOR_TEST_SPEED_PERCENT;
    int duration_ms = MOTOR_TEST_DURATION_MS;

    /*
     * 手动退出活跃模式：
     * 这类词不执行运动，只让 voice_cloud_task 回到待机。
     * 返回 true 是为了把它当作本地命令处理，抑制云端 TTS。
     */
    if (text_has_sleep_word(text)) {
        ESP_LOGI(TAG, "VOICE CMD: sleep/standby, text=%s", text);
        motor_send_cmd(MOTOR_CMD_STOP, 0, 0);
        return true;
    }

    /*
     * 停止类命令优先级最高。
     * 这里只投递命令，不直接 vTaskDelay()，避免卡住语音/聊天任务。
     */
    if (text_has_any(text, "停止", "停下", "别动") ||
        text_has_any(text, "停车", "停住", "不要动")) {
        ESP_LOGI(TAG, "VOICE CMD: stop, text=%s", text);
        motor_send_cmd(MOTOR_CMD_STOP, 0, 0);
        return true;
    }

    if (text_has_any(text, "前进", "向前", "往前") ||
        text_has_any(text, "前走", "往前走", "向前走")) {
        ESP_LOGI(TAG, "VOICE CMD: forward, text=%s", text);
        motor_send_cmd(MOTOR_CMD_FORWARD, speed, duration_ms);
        return true;
    }

    if (text_has_any(text, "后退", "倒车", "向后") ||
        text_has_any(text, "往后", "往后退", "向后退")) {
        ESP_LOGI(TAG, "VOICE CMD: backward, text=%s", text);
        motor_send_cmd(MOTOR_CMD_BACKWARD, speed, duration_ms);
        return true;
    }

    if (text_has_any(text, "左转", "向左", "往左")) {
        ESP_LOGI(TAG, "VOICE CMD: left, text=%s", text);
        motor_send_cmd(MOTOR_CMD_LEFT, speed, MOTOR_TURN_DURATION_MS);
        return true;
    }

    if (text_has_any(text, "右转", "向右", "往右")) {
        ESP_LOGI(TAG, "VOICE CMD: right, text=%s", text);
        motor_send_cmd(MOTOR_CMD_RIGHT, speed, MOTOR_TURN_DURATION_MS);
        return true;
    }

    if (text_has_any(text, "巡逻", "开始巡逻", "自动巡逻")) {
        ESP_LOGI(TAG, "VOICE CMD: patrol, text=%s", text);
        motor_send_cmd(MOTOR_CMD_PATROL, speed, duration_ms);
        return true;
    }

    return false;
}

static void motor_send_cmd(motor_cmd_type_t type, int speed, int duration_ms)
{
    if (motor_cmd_queue == NULL) {
        ESP_LOGW(TAG, "motor_cmd_queue not ready");
        return;
    }

    motor_cmd_t cmd = {
        .type = type,
        .speed = clamp_int(speed, 0, 100),
        .duration_ms = clamp_int(duration_ms, 100, MOTOR_CMD_MAX_DURATION_MS),
    };

    if (type == MOTOR_CMD_STOP) {
        cmd.speed = 0;
        cmd.duration_ms = 0;
    }

    /*
     * 队列长度为 1，使用覆盖写入：新命令可以打断旧动作。
     * 例如小车正在前进时，说“停止”会覆盖队列中的旧命令。
     */
    xQueueOverwrite(motor_cmd_queue, &cmd);
}

static void motor_apply_cmd(const motor_cmd_t *cmd)
{
    if (cmd == NULL) {
        motor_stop();
        return;
    }

    switch (cmd->type) {
    case MOTOR_CMD_STOP:
motor_stop();
        break;

    case MOTOR_CMD_FORWARD:
motor_forward(cmd->speed);
        break;

    case MOTOR_CMD_BACKWARD:
motor_backward(cmd->speed);
        break;

    case MOTOR_CMD_LEFT:
motor_left(cmd->speed);
        break;

    case MOTOR_CMD_RIGHT:
motor_right(cmd->speed);
        break;

    case MOTOR_CMD_PATROL:
motor_forward(cmd->speed);
        break;

    default:
motor_stop();
        break;
    }
}

static void motor_task(void *arg)
{
    motor_cmd_t cmd;

    while (1) {
        if (xQueueReceive(motor_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        while (1) {
            motor_apply_cmd(&cmd);

            if (cmd.type == MOTOR_CMD_STOP) {
                break;
            }

            int remaining_ms = clamp_int(cmd.duration_ms, 100, MOTOR_CMD_MAX_DURATION_MS);
            bool interrupted = false;

            while (remaining_ms > 0) {
                motor_cmd_t next_cmd;
                int step_ms = remaining_ms > 50 ? 50 : remaining_ms;

                if (xQueueReceive(motor_cmd_queue, &next_cmd, pdMS_TO_TICKS(step_ms)) == pdTRUE) {
                    cmd = next_cmd;
                    interrupted = true;
                    break;
                }

                remaining_ms -= step_ms;
            }

            if (!interrupted) {
                motor_stop();
                break;
            }
        }
    }
}

static void motor_init(void)
{
    ESP_LOGI(TAG, "Motor init");

    gpio_config_t gpio_conf = {
        .pin_bit_mask =
            (1ULL << MOTOR1_DIR_GPIO) |
            (1ULL << MOTOR2_DIR_GPIO) |
            (1ULL << MOTOR_NSLEEP_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&gpio_conf));

    /*
     * DRV8876 nSLEEP 拉高，退出休眠。
     */
    ESP_ERROR_CHECK(gpio_set_level(MOTOR_NSLEEP_GPIO, 1));

    ledc_timer_config_t timer_conf = {
        .speed_mode = MOTOR_LEDC_MODE,
        .duty_resolution = MOTOR_PWM_RES,
        .timer_num = MOTOR_LEDC_TIMER,
        .freq_hz = MOTOR_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

    ledc_channel_config_t ch1_conf = {
        .gpio_num = MOTOR1_PWM_GPIO,
        .speed_mode = MOTOR_LEDC_MODE,
        .channel = MOTOR1_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = MOTOR_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };

    ESP_ERROR_CHECK(ledc_channel_config(&ch1_conf));

    ledc_channel_config_t ch2_conf = {
        .gpio_num = MOTOR2_PWM_GPIO,
        .speed_mode = MOTOR_LEDC_MODE,
        .channel = MOTOR2_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = MOTOR_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };

    ESP_ERROR_CHECK(ledc_channel_config(&ch2_conf));

    motor_stop();

    ESP_LOGI(TAG, "Motor init done: M1 PWM GPIO%d DIR GPIO%d, M2 PWM GPIO%d DIR GPIO%d, nSLEEP GPIO%d",
             MOTOR1_PWM_GPIO,
             MOTOR1_DIR_GPIO,
             MOTOR2_PWM_GPIO,
             MOTOR2_DIR_GPIO,
             MOTOR_NSLEEP_GPIO);
}

/* ================= 豆包 Realtime WebSocket 工具 ================= */

typedef struct {
    int16_t group[3];
    int group_count;
} downsample_24k_to_16k_state_t;

typedef struct {
    EventGroupHandle_t event_group;
    RingbufHandle_t tts_ringbuf;

    /*
     * 文件流式 TTS 缓存。
     * tts_file_wr 只在 WebSocket 回调中写入，播放端另开 rb 文件句柄读取。
     */
    FILE *tts_file_wr;
    bool tts_file_mode;

    /*
     * tts_file_bytes 是已经 fflush 后，播放端可以安全读取的字节数。
     * tts_file_total_bytes 是实际已经 fwrite 成功的总字节数。
     * pending 表示还没 flush 给读端看的缓存量。
     */
    volatile uint32_t tts_file_bytes;
    uint32_t tts_file_total_bytes;
    uint32_t tts_file_pending_bytes;
    bool tts_file_too_large;

    char session_id[48];

    uint8_t *rx_buf;
    int rx_len;
    int rx_expected;

    bool connection_started;
    bool session_started;
    bool tts_done;
    bool failed;
    bool ws_connected;

    char asr_text[256];
    char chat_text[512];

    volatile uint32_t tts_bytes_rx;
    bool tts_play_started;
    bool cmd_executed;
    bool is_command;
    bool enable_motor_cmd;
    bool play_tts_audio;
    bool suppress_tts_for_command;
} doubao_ctx_t;


typedef struct {
    doubao_ctx_t ctx;
    esp_websocket_client_handle_t ws;
    char headers[2048];
    bool active;
} doubao_session_t;

static void doubao_tts_stream_flush(doubao_ctx_t *ctx)
{
    if (ctx == NULL || ctx->tts_file_wr == NULL) {
        return;
    }

    if (ctx->tts_file_pending_bytes == 0 &&
        ctx->tts_file_bytes == ctx->tts_file_total_bytes) {
        return;
    }

    fflush(ctx->tts_file_wr);

    /*
     * 只有 flush 后才把数据暴露给播放端。
     * 这样播放端不会读到还没落到 SPIFFS 的尾部数据。
     */
    ctx->tts_file_bytes = ctx->tts_file_total_bytes;
    ctx->tts_file_pending_bytes = 0;
}

static esp_err_t doubao_session_open(doubao_session_t *s);
static void doubao_session_close(doubao_session_t *s);
static esp_err_t doubao_run_one_turn_reuse(doubao_session_t *s,
                                           uint8_t *pcm,
                                           uint32_t pcm_len,
                                           char *asr_out,
                                           size_t asr_out_size,
                                           bool enable_motor_cmd,
                                           bool play_tts_audio,
                                           bool *is_cmd_out);

#define DB_BIT_WS_CONNECTED        BIT0
#define DB_BIT_WS_DISCONNECTED     BIT1
#define DB_BIT_CONNECTION_STARTED  BIT2
#define DB_BIT_SESSION_STARTED     BIT3
#define DB_BIT_TTS_DONE            BIT4
#define DB_BIT_FAILED              BIT5

#define DB_MSG_FULL_CLIENT_REQUEST   0x1
#define DB_MSG_AUDIO_ONLY_REQUEST    0x2
#define DB_MSG_FULL_SERVER_RESPONSE  0x9
#define DB_MSG_AUDIO_ONLY_RESPONSE   0xB
#define DB_MSG_ERROR_INFORMATION     0xF

#define DB_FLAG_NO_SEQUENCE          0x0
#define DB_FLAG_EVENT                0x4

#define DB_SERIALIZATION_RAW         0x0
#define DB_SERIALIZATION_JSON        0x1

#define DB_COMPRESSION_NONE          0x0

#define DB_EVENT_START_CONNECTION    1
#define DB_EVENT_FINISH_CONNECTION   2
#define DB_EVENT_START_SESSION       100
#define DB_EVENT_FINISH_SESSION      102
#define DB_EVENT_TASK_REQUEST        200
#define DB_EVENT_END_ASR             400

#define DB_EVENT_CONNECTION_STARTED  50
#define DB_EVENT_CONNECTION_FAILED   51
#define DB_EVENT_CONNECTION_FINISHED 52
#define DB_EVENT_SESSION_STARTED     150
#define DB_EVENT_SESSION_FINISHED    152
#define DB_EVENT_SESSION_FAILED      153
#define DB_EVENT_USAGE_RESPONSE      154
#define DB_EVENT_TTS_SENTENCE_START  350
#define DB_EVENT_TTS_SENTENCE_END    351
#define DB_EVENT_TTS_RESPONSE        352
#define DB_EVENT_TTS_ENDED           359
#define DB_EVENT_ASR_INFO            450
#define DB_EVENT_ASR_RESPONSE        451
#define DB_EVENT_ASR_ENDED           459
#define DB_EVENT_CHAT_RESPONSE       550
#define DB_EVENT_CHAT_ENDED          559

static bool doubao_str_invalid(const char *s)
{
    if (s == NULL || strlen(s) == 0) {
        return true;
    }

    if (strstr(s, "请填") != NULL) {
        return true;
    }

    return false;
}

static void db_write_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

static uint32_t db_read_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static void doubao_make_uuid(char out[37])
{
    uint32_t a = esp_random();
    uint32_t b = esp_random();
    uint32_t c = esp_random();
    uint32_t d = esp_random();

    snprintf(out, 37,
             "%08lx-%04lx-%04lx-%04lx-%08lx%04lx",
             (unsigned long)a,
             (unsigned long)((b >> 16) & 0xFFFF),
             (unsigned long)(b & 0xFFFF),
             (unsigned long)((c >> 16) & 0xFFFF),
             (unsigned long)c,
             (unsigned long)(d & 0xFFFF));
}

static bool doubao_event_has_session_id(uint32_t event_id)
{
    /*
     * Connection 级别事件一般不带 session_id；Session / ASR / Chat / TTS 级别事件带 session_id。
     * 文档中的 TTSResponse 示例也包含 session_id。
     */
    if (event_id == DB_EVENT_CONNECTION_STARTED ||
        event_id == DB_EVENT_CONNECTION_FAILED ||
        event_id == DB_EVENT_CONNECTION_FINISHED) {
        return false;
    }

    return true;
}

static esp_err_t doubao_send_frame(esp_websocket_client_handle_t ws,
                                   uint8_t msg_type,
                                   uint8_t serialization,
                                   uint32_t event_id,
                                   const char *session_id,
                                   const uint8_t *payload,
                                   uint32_t payload_len)
{
    if (ws == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t session_len = 0;
    if (session_id != NULL && session_id[0] != '\0') {
        session_len = strlen(session_id);
    }

    uint32_t frame_len = 4 + 4 + payload_len + 4;
    if (session_len > 0) {
        frame_len += 4 + session_len;
    }

    uint8_t *frame = heap_caps_malloc(frame_len, MALLOC_CAP_INTERNAL);
    if (frame == NULL) {
        frame = malloc(frame_len);
    }

    if (frame == NULL) {
        ESP_LOGE(TAG, "doubao frame malloc failed, len=%lu", (unsigned long)frame_len);
        return ESP_ERR_NO_MEM;
    }

    uint32_t off = 0;

    frame[off++] = 0x11;  // version=1, header_size=1(4 bytes)
    frame[off++] = (uint8_t)((msg_type << 4) | DB_FLAG_EVENT);
    frame[off++] = (uint8_t)((serialization << 4) | DB_COMPRESSION_NONE);
    frame[off++] = 0x00;

    db_write_u32_be(frame + off, event_id);
    off += 4;

    if (session_len > 0) {
        db_write_u32_be(frame + off, session_len);
        off += 4;
        memcpy(frame + off, session_id, session_len);
        off += session_len;
    }

    db_write_u32_be(frame + off, payload_len);
    off += 4;

    if (payload_len > 0 && payload != NULL) {
        memcpy(frame + off, payload, payload_len);
        off += payload_len;
    }

    int sent = esp_websocket_client_send_bin(ws, (const char *)frame, (int)off, pdMS_TO_TICKS(10000));
    free(frame);

    if (sent != (int)off) {
        ESP_LOGE(TAG, "doubao send frame failed, sent=%d, want=%lu", sent, (unsigned long)off);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t doubao_send_json_event(esp_websocket_client_handle_t ws,
                                        uint32_t event_id,
                                        const char *session_id,
                                        const char *json)
{
    if (json == NULL) {
        json = "{}";
    }

    ESP_LOGI(TAG, "Doubao send json event=%lu, len=%u",
         (unsigned long)event_id,
         (unsigned int)strlen(json));

    return doubao_send_frame(ws,
                             DB_MSG_FULL_CLIENT_REQUEST,
                             DB_SERIALIZATION_JSON,
                             event_id,
                             session_id,
                             (const uint8_t *)json,
                             strlen(json));
}

static esp_err_t doubao_send_audio_event(esp_websocket_client_handle_t ws,
                                         const char *session_id,
                                         const uint8_t *pcm,
                                         uint32_t pcm_len)
{
    return doubao_send_frame(ws,
                             DB_MSG_AUDIO_ONLY_REQUEST,
                             DB_SERIALIZATION_RAW,
                             DB_EVENT_TASK_REQUEST,
                             session_id,
                             pcm,
                             pcm_len);
}

static char *doubao_build_start_session_json(void)
{
    cJSON *root = cJSON_CreateObject();

    cJSON *asr = cJSON_CreateObject();
    cJSON *audio_info = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_info, "format", "pcm");
    cJSON_AddNumberToObject(audio_info, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_info, "channel", 1);
    cJSON_AddItemToObject(asr, "audio_info", audio_info);
    cJSON_AddItemToObject(root, "asr", asr);

    cJSON *dialog = cJSON_CreateObject();
    cJSON_AddStringToObject(dialog, "bot_name", ASSISTANT_NAME_CN);
    cJSON_AddStringToObject(dialog, "system_role",
        "你叫乐乐，是桌面小车助手。"
        "只用中文回答。"
        "普通回答最多8个汉字。"
        "不要解释，不要闲聊，不要扩展背景。"
        "用户要求唱歌时，唱两三句流行歌曲就行"
        "用户发出前进、后退、左转、右转、停止、巡逻等控制命令时，只回答“收到”。"
        "听不清或内容无关时，只回答“没听清”。");

    cJSON_AddStringToObject(dialog, "speaking_style",
        "普通回答极简短句；唱歌时可用轻快节奏唱一小段原创短歌。");

    cJSON *dialog_extra = cJSON_CreateObject();
    cJSON_AddStringToObject(dialog_extra, "model", DOUBAO_MODEL_VERSION);
    cJSON_AddStringToObject(dialog_extra, "input_mod", "push_to_talk");
    cJSON_AddBoolToObject(dialog_extra, "enable_user_query_exit", true);
    cJSON_AddItemToObject(dialog, "extra", dialog_extra);
    cJSON_AddItemToObject(root, "dialog", dialog);

    cJSON *tts = cJSON_CreateObject();
    cJSON_AddStringToObject(tts, "speaker", DOUBAO_SPEAKER);

    cJSON *tts_audio = cJSON_CreateObject();
    cJSON_AddNumberToObject(tts_audio, "channel", 1);
    cJSON_AddStringToObject(tts_audio, "format", DOUBAO_TTS_FORMAT);
    cJSON_AddNumberToObject(tts_audio, "sample_rate", DOUBAO_TTS_SAMPLE_RATE);
    cJSON_AddItemToObject(tts, "audio_config", tts_audio);

    cJSON_AddItemToObject(root, "tts", tts);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json;
}

static void doubao_parse_json_event(doubao_ctx_t *ctx, uint32_t event_id, const uint8_t *payload, uint32_t payload_len)
{
    if (payload == NULL || payload_len == 0) {
        if (event_id == DB_EVENT_CONNECTION_STARTED) {
            ctx->connection_started = true;
            xEventGroupSetBits(ctx->event_group, DB_BIT_CONNECTION_STARTED);
            ESP_LOGI(TAG, "Doubao ConnectionStarted");
        } else if (event_id == DB_EVENT_SESSION_STARTED) {
            ctx->session_started = true;
            xEventGroupSetBits(ctx->event_group, DB_BIT_SESSION_STARTED);
            ESP_LOGI(TAG, "Doubao SessionStarted");
        } else if (event_id == DB_EVENT_TTS_ENDED) {
            doubao_tts_stream_flush(ctx);
            ctx->tts_done = true;
            xEventGroupSetBits(ctx->event_group, DB_BIT_TTS_DONE);
            ESP_LOGI(TAG, "Doubao TTSEnded");
        } else if (event_id == DB_EVENT_ASR_ENDED) {
            if (ctx->asr_text[0] == '\0') {
                ESP_LOGW(TAG, "ASR ended with empty text, skip waiting TTS");
                ctx->tts_done = true;
                xEventGroupSetBits(ctx->event_group, DB_BIT_TTS_DONE);
            }
        }
        return;
    }

    char *json_str = malloc(payload_len + 1);
    if (json_str == NULL) {
        return;
    }

    memcpy(json_str, payload, payload_len);
    json_str[payload_len] = '\0';

    ESP_LOGI(TAG, "Doubao JSON event=%lu, len=%lu",
         (unsigned long)event_id,
         (unsigned long)payload_len);
    if (event_id == DB_EVENT_ASR_ENDED) {
    /*
     * ASR 已结束，但没有识别出有效文本。
     * 这种情况下服务端可能不会返回 Chat/TTS。
     */
    if (ctx->asr_text[0] == '\0') {
        ESP_LOGW(TAG, "ASR ended with empty text, skip waiting TTS");
        ctx->tts_done = true;
        xEventGroupSetBits(ctx->event_group, DB_BIT_TTS_DONE);
    }

    free(json_str);
    return;
    }

    if (event_id == DB_EVENT_CONNECTION_STARTED) {
        ctx->connection_started = true;
        xEventGroupSetBits(ctx->event_group, DB_BIT_CONNECTION_STARTED);
        free(json_str);
        return;
    }

    if (event_id == DB_EVENT_CONNECTION_FAILED ||
        event_id == DB_EVENT_SESSION_FAILED) {
        ctx->failed = true;
        xEventGroupSetBits(ctx->event_group, DB_BIT_FAILED);
        free(json_str);
        return;
    }

    if (event_id == DB_EVENT_SESSION_STARTED) {
        ctx->session_started = true;
        xEventGroupSetBits(ctx->event_group, DB_BIT_SESSION_STARTED);
        free(json_str);
        return;
    }

    if (event_id == DB_EVENT_TTS_ENDED) {
        doubao_tts_stream_flush(ctx);
        ctx->tts_done = true;
        xEventGroupSetBits(ctx->event_group, DB_BIT_TTS_DONE);
        free(json_str);
        return;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        free(json_str);
        return;
    }

    if (event_id == DB_EVENT_ASR_RESPONSE) {
        cJSON *results = cJSON_GetObjectItem(root, "results");
        if (cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) {
            cJSON *first = cJSON_GetArrayItem(results, 0);
            cJSON *text = cJSON_GetObjectItem(first, "text");
            cJSON *is_interim = cJSON_GetObjectItem(first, "is_interim");

            if (cJSON_IsString(text)) {
                bool interim = cJSON_IsBool(is_interim) && cJSON_IsTrue(is_interim);

                strncpy(ctx->asr_text, text->valuestring, sizeof(ctx->asr_text) - 1);
                ctx->asr_text[sizeof(ctx->asr_text) - 1] = '\0';

                ESP_LOGI(TAG, "ASR%s: %s",
                         interim ? "(interim)" : "",
                         ctx->asr_text);

                /*
                 * 最终 ASR 文本一出来就尝试执行电机命令。
                 * 不在这里直接阻塞驱动电机，只向 motor_task 投递队列命令。
                 */
                if (!interim && !ctx->cmd_executed && ctx->enable_motor_cmd) {
                    ctx->cmd_executed = doubao_try_execute_voice_command(ctx->asr_text);
                    ctx->is_command = ctx->cmd_executed;
                    if (ctx->cmd_executed) {
                        ESP_LOGI(TAG, "Voice command queued at final ASR");
                    }
                }
            }
        }
    } else if (event_id == DB_EVENT_CHAT_RESPONSE) {
        cJSON *content = cJSON_GetObjectItem(root, "content");
        if (cJSON_IsString(content)) {
            size_t old_len = strlen(ctx->chat_text);
            size_t add_len = strlen(content->valuestring);
            if (old_len + add_len < sizeof(ctx->chat_text) - 1) {
                strcat(ctx->chat_text, content->valuestring);
            }
            ESP_LOGI(TAG, "Chat: %s", content->valuestring);
        }
    }

    cJSON_Delete(root);
    free(json_str);
}

static void doubao_handle_protocol_frame(doubao_ctx_t *ctx, const uint8_t *data, int len)
{
    if (ctx == NULL || data == NULL || len < 12) {
        return;
    }

    uint8_t header0 = data[0];
    uint8_t header_size_words = header0 & 0x0F;
    uint8_t msg_type = data[1] >> 4;
    uint8_t flags = data[1] & 0x0F;
    uint8_t serialization = data[2] >> 4;

    int off = header_size_words * 4;
    if (off < 4 || off > len) {
        return;
    }

    uint32_t event_id = 0;

    if (flags == DB_FLAG_EVENT) {
        if (off + 4 > len) {
            return;
        }
        event_id = db_read_u32_be(data + off);
        off += 4;
    }

    if (doubao_event_has_session_id(event_id)) {
        if (off + 4 > len) {
            return;
        }

        uint32_t session_len = db_read_u32_be(data + off);
        off += 4;

        if (off + session_len > (uint32_t)len) {
            return;
        }

        off += session_len;
    }

    if (off + 4 > len) {
        return;
    }

    uint32_t payload_len = db_read_u32_be(data + off);
    off += 4;

    if (off + payload_len > (uint32_t)len) {
        ESP_LOGW(TAG, "Doubao frame truncated: event=%lu, payload=%lu, len=%d, off=%d",
                 (unsigned long)event_id, (unsigned long)payload_len, len, off);
        return;
    }

    const uint8_t *payload = data + off;

    ESP_LOGD(TAG, "Doubao frame type=0x%x ser=0x%x event=%lu payload=%lu",
             msg_type, serialization, (unsigned long)event_id, (unsigned long)payload_len);

    if (msg_type == DB_MSG_ERROR_INFORMATION) {
        ESP_LOGE(TAG, "Doubao error frame: %.*s", (int)payload_len, (const char *)payload);
        ctx->failed = true;
        xEventGroupSetBits(ctx->event_group, DB_BIT_FAILED);
        return;
    }

    if (msg_type == DB_MSG_AUDIO_ONLY_RESPONSE && event_id == DB_EVENT_TTS_RESPONSE) {
        if (payload_len == 0) {
            return;
        }

        /*
         * 文件流式模式：
         * 长文本/唱歌时，WebSocket 回调不再往大 ringbuf 里塞数据，
         * 而是直接写入 SPIFFS 临时 PCM 文件。播放端会边读边播。
         */
        if (ctx->tts_file_mode) {
            if (ctx->tts_file_wr == NULL || ctx->tts_file_too_large) {
                return;
            }

            if (ctx->tts_file_total_bytes + payload_len > DOUBAO_TTS_FILE_MAX_BYTES) {
                ESP_LOGW(TAG,
                         "cloud TTS stream too large, stop caching: total=%lu max=%lu",
                         (unsigned long)ctx->tts_file_total_bytes,
                         (unsigned long)DOUBAO_TTS_FILE_MAX_BYTES);
                doubao_tts_stream_flush(ctx);
                ctx->tts_file_too_large = true;
                ctx->tts_done = true;
                xEventGroupSetBits(ctx->event_group, DB_BIT_TTS_DONE);
                return;
            }

            size_t written = fwrite(payload, 1, payload_len, ctx->tts_file_wr);
            if (written != payload_len) {
                size_t total = 0;
                size_t used = 0;
                esp_spiffs_info("storage", &total, &used);
                ESP_LOGE(TAG,
                         "cloud TTS stream write failed: want=%lu written=%lu spiffs=%u/%u",
                         (unsigned long)payload_len,
                         (unsigned long)written,
                         (unsigned int)used,
                         (unsigned int)total);
                ctx->failed = true;
                xEventGroupSetBits(ctx->event_group, DB_BIT_FAILED);
                return;
            }

            ctx->tts_file_total_bytes += (uint32_t)written;
            ctx->tts_file_pending_bytes += (uint32_t)written;
            ctx->tts_bytes_rx += (uint32_t)written;

            /*
             * 不要每包都 fflush。每包 flush 会让 SPIFFS 频繁擦写/GC，
             * 前半段最容易卡顿。这里按 8KB 一批暴露给播放端。
             */
            if (ctx->tts_file_pending_bytes >= DOUBAO_TTS_FILE_FLUSH_BYTES) {
                doubao_tts_stream_flush(ctx);
            }

            return;
        }

        /*
         * 非文件模式兜底：只用于短 TTS 或不播放 TTS 的情况。
         * 不能在 WebSocket 回调里阻塞 1000ms，否则长流会被服务端断开。
         */
        if (ctx->tts_ringbuf != NULL) {
            const uint8_t *p = payload;
            uint32_t remain = payload_len;

            while (remain > 0) {
                uint32_t chunk = remain > 2048 ? 2048 : remain;

                BaseType_t ok = xRingbufferSend(
                    ctx->tts_ringbuf,
                    p,
                    chunk,
                    pdMS_TO_TICKS(20)
                );

                if (ok != pdTRUE) {
                    ESP_LOGW(TAG, "TTS ringbuf full, drop %lu bytes",
                             (unsigned long)chunk);
                    return;
                }

                ctx->tts_bytes_rx += chunk;
                p += chunk;
                remain -= chunk;
            }
        }

        return;
    }

    if (msg_type == DB_MSG_FULL_SERVER_RESPONSE) {
        doubao_parse_json_event(ctx, event_id, payload, payload_len);
    }
}

static void doubao_ws_event_handler(void *handler_args,
                                    esp_event_base_t base,
                                    int32_t event_id,
                                    void *event_data)
{
    doubao_ctx_t *ctx = (doubao_ctx_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    if (ctx == NULL) {
        return;
    }

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Doubao WebSocket connected");
        ctx->ws_connected = true;
        xEventGroupSetBits(ctx->event_group, DB_BIT_WS_CONNECTED);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Doubao WebSocket disconnected");
        ctx->ws_connected = false;
        xEventGroupSetBits(ctx->event_group, DB_BIT_WS_DISCONNECTED);
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "Doubao WebSocket error");
        ctx->failed = true;
        xEventGroupSetBits(ctx->event_group, DB_BIT_FAILED);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data == NULL || data->data_ptr == NULL || data->data_len <= 0) {
            break;
        }
        if (data->payload_offset == 0 && data->data_len == data->payload_len) {
        doubao_handle_protocol_frame(ctx,
                                     (const uint8_t *)data->data_ptr,
                                     data->data_len);
        break;
        }
        /*
         * esp_websocket_client 可能把一个 WebSocket message 拆成多次回调。
         * 这里按 payload_offset/payload_len 聚合完整 message 后再解析豆包二进制协议。
         */
        if (data->payload_offset == 0) {
            free(ctx->rx_buf);
            ctx->rx_buf = NULL;
            ctx->rx_len = 0;
            ctx->rx_expected = data->payload_len;

            if (ctx->rx_expected <= 0 || ctx->rx_expected > DOUBAO_MAX_WS_FRAME_BYTES) {
                ESP_LOGW(TAG, "Invalid WS payload_len=%d", ctx->rx_expected);
                ctx->rx_expected = 0;
                break;
            }

            ctx->rx_buf = malloc(ctx->rx_expected);
            if (ctx->rx_buf == NULL) {
                ESP_LOGE(TAG, "rx_buf malloc failed");
                ctx->rx_expected = 0;
                break;
            }
        }

        if (ctx->rx_buf != NULL &&
            data->payload_offset + data->data_len <= ctx->rx_expected) {
            memcpy(ctx->rx_buf + data->payload_offset, data->data_ptr, data->data_len);
            ctx->rx_len += data->data_len;
        }

        if (ctx->rx_buf != NULL &&
            ctx->rx_expected > 0 &&
            data->payload_offset + data->data_len >= ctx->rx_expected) {
            doubao_handle_protocol_frame(ctx, ctx->rx_buf, ctx->rx_expected);
            free(ctx->rx_buf);
            ctx->rx_buf = NULL;
            ctx->rx_len = 0;
            ctx->rx_expected = 0;
        }

        break;

    default:
        break;
    }
}

/* ================= 豆包 TTS PCM 24k -> ES8311 16k 播放 ================= */

static void play_pcm16k_mono_chunk(const int16_t *mono, int samples)
{
    if (mono == NULL || samples <= 0) {
        return;
    }

    int played = 0;

    while (played < samples) {
        int n = samples - played;
        if (n > PCM_MONO_SAMPLES_PER_CHUNK) {
            n = PCM_MONO_SAMPLES_PER_CHUNK;
        }

        mono_to_stereo(&mono[played], g_tts_stereo_buf, n);

        esp_err_t ret = esp_codec_dev_write(
            codec_dev,
            g_tts_stereo_buf,
            n * 2 * sizeof(int16_t)
        );

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "TTS write failed: %s", esp_err_to_name(ret));
        }

        played += n;
    }
}

static void play_pcm24k_as_16k_chunk(downsample_24k_to_16k_state_t *state,
                                     const uint8_t *pcm,
                                     uint32_t pcm_len)
{
    if (state == NULL || pcm == NULL || pcm_len < 2) {
        return;
    }

    const int16_t *in = (const int16_t *)pcm;
    int in_samples = pcm_len / sizeof(int16_t);

    int out_cap = (in_samples + 4) * 2 / 3 + 4;
    int16_t *out = heap_caps_malloc(out_cap * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    if (out == NULL) {
        out = malloc(out_cap * sizeof(int16_t));
    }

    if (out == NULL) {
        return;
    }

    int out_samples = 0;

    for (int i = 0; i < in_samples; i++) {
        state->group[state->group_count++] = in[i];

        if (state->group_count == 3) {
            if (out_samples + 2 <= out_cap) {
                out[out_samples++] = state->group[0];
                out[out_samples++] = state->group[2];
            }

            state->group_count = 0;
        }
    }

    if (out_samples > 0) {
        play_pcm16k_mono_chunk(out, out_samples);
    }

    free(out);
}

static void doubao_drain_tts_audio(doubao_ctx_t *ctx,
                                   downsample_24k_to_16k_state_t *ds_state,
                                   int total_timeout_ms)
{
    int64_t start_ms = esp_timer_get_time() / 1000;

    /*
     * SPIFFS 边写边播模式：
     * WebSocket 回调持续 fwrite + fflush；
     * 本函数等待少量预缓存后，另开 rb 文件句柄边读边播放。
     */
    if (ctx->tts_file_mode) {
        int64_t first_wait_start_ms = start_ms;
        int64_t last_audio_ms = start_ms;
        bool played_once = false;

        while (!ctx->tts_done && ctx->tts_file_bytes == 0) {
            EventBits_t bits = xEventGroupGetBits(ctx->event_group);
            if (bits & DB_BIT_FAILED) {
                break;
            }

            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - first_wait_start_ms > DOUBAO_TTS_FIRST_PACKET_TIMEOUT_MS) {
                ESP_LOGW(TAG, "cloud TTS first packet timeout");
                break;
            }

            if (now_ms - start_ms > total_timeout_ms) {
                ESP_LOGW(TAG, "cloud TTS total timeout before first packet");
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(20));
        }

        int64_t prebuffer_start_ms = esp_timer_get_time() / 1000;
        while (!ctx->tts_done &&
               ctx->tts_file_bytes < DOUBAO_TTS_FILE_PREBUFFER_BYTES) {
            EventBits_t bits = xEventGroupGetBits(ctx->event_group);
            if (bits & DB_BIT_FAILED) {
                break;
            }

            int64_t now_ms = esp_timer_get_time() / 1000;
            if (ctx->tts_file_bytes >= DOUBAO_TTS_FILE_MIN_PLAY_BYTES &&
                now_ms - prebuffer_start_ms > DOUBAO_TTS_PREBUFFER_TIMEOUT_MS) {
                ESP_LOGW(TAG,
                        "cloud TTS prebuffer timeout, start anyway: have=%lu target=%lu",
                        (unsigned long)ctx->tts_file_bytes,
                        (unsigned long)DOUBAO_TTS_FILE_PREBUFFER_BYTES);
                break;
            }

            if (now_ms - start_ms > total_timeout_ms) {
                ESP_LOGW(TAG, "cloud TTS total timeout during prebuffer");
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(20));
        }

        doubao_tts_stream_flush(ctx);

        FILE *rf = fopen(DOUBAO_CLOUD_TTS_STREAM_PCM, "rb");
        if (rf == NULL) {
            ESP_LOGE(TAG, "open cloud TTS stream for read failed");
            return;
        }

        uint8_t *read_buf = heap_caps_malloc(PCM_MONO_BYTES_PER_CHUNK,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (read_buf == NULL) {
            ESP_LOGE(TAG, "cloud TTS read buffer malloc failed");
            fclose(rf);
            return;
        }

        uint32_t read_bytes = 0;

        while (1) {
            EventBits_t bits = xEventGroupGetBits(ctx->event_group);
            if (bits & DB_BIT_FAILED) {
                break;
            }

            uint32_t available = ctx->tts_file_bytes;

            if (read_bytes < available) {
                uint32_t can_read = available - read_bytes;
                if (can_read > PCM_MONO_BYTES_PER_CHUNK) {
                    can_read = PCM_MONO_BYTES_PER_CHUNK;
                }

                size_t n = fread(read_buf, 1, can_read, rf);
                if (n > 0) {
                    if (ctx->play_tts_audio &&
                        !(ctx->suppress_tts_for_command && ctx->is_command)) {
                        played_once = true;
                        last_audio_ms = esp_timer_get_time() / 1000;

                        if (DOUBAO_TTS_SAMPLE_RATE == 16000) {
                            int samples = n / sizeof(int16_t);
                            if (samples > 0) {
                                play_pcm16k_mono_chunk((const int16_t *)read_buf, samples);
                            }
                        } else {
                            play_pcm24k_as_16k_chunk(ds_state, read_buf, (uint32_t)n);
                        }
                    }

                    read_bytes += (uint32_t)n;
                    continue;
                }

                /*
                 * 读到当前 EOF 后，SPIFFS 文件后续还会继续增长。
                 * 必须 clearerr，否则后续 fread 可能一直返回 0。
                 */
                clearerr(rf);
            }

            if (ctx->tts_done && read_bytes >= ctx->tts_file_bytes) {
                break;
            }

            /*
            * 播放端已经追上写入端。
            * 这种情况最容易导致唱歌开头一卡一卡。
            * 不要马上插静音，先等 SPIFFS 文件继续增长一段。
            */
            if (!ctx->tts_done && read_bytes >= ctx->tts_file_bytes) {
                int64_t underrun_start_ms = esp_timer_get_time() / 1000;

                while (!ctx->tts_done) {
                    EventBits_t bits2 = xEventGroupGetBits(ctx->event_group);
                    if (bits2 & DB_BIT_FAILED) {
                        break;
                    }

                    uint32_t now_available = ctx->tts_file_bytes;
                    uint32_t buffered = 0;

                    if (now_available > read_bytes) {
                        buffered = now_available - read_bytes;
                    }

                    /*
                    * 重新攒够一小段数据后再继续播放。
                    * 64KB 大约是 2 秒 16kHz/16bit/mono PCM。
                    */
                    if (buffered >= DOUBAO_TTS_FILE_RESUME_BYTES) {
                        break;
                    }

                    int64_t wait_now_ms = esp_timer_get_time() / 1000;

                    /*
                    * 最多等 1.5 秒，防止一直卡在这里。
                    */
                    if (wait_now_ms - underrun_start_ms > 1500) {
                        break;
                    }

                    if (wait_now_ms - start_ms > total_timeout_ms) {
                        ESP_LOGW(TAG, "cloud TTS stream total timeout");
                        break;
                    }

                    vTaskDelay(pdMS_TO_TICKS(20));
                }

                /*
                * 如果等待后已经有新数据了，回到 while(1) 开头继续 fread。
                */
                if (ctx->tts_file_bytes > read_bytes) {
                    continue;
                }
            }

            int64_t now_ms = esp_timer_get_time() / 1000;

            if (played_once) {
                /*
                * 仍然没有新数据时，才少量补静音。
                * 这里不要用 120ms，太敏感，唱歌开头容易被切碎。
                */
                if (now_ms - last_audio_ms > 500) {
                    play_tts_silence_20ms();
                }

                if (now_ms - last_audio_ms > DOUBAO_TTS_IDLE_TIMEOUT_MS) {
                    ESP_LOGW(TAG, "cloud TTS stream idle timeout");
                    break;
                }
            }

            if (now_ms - start_ms > total_timeout_ms) {
                ESP_LOGW(TAG, "cloud TTS stream total timeout");
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(10));
        }

        free(read_buf);
        fclose(rf);

        if (ctx->play_tts_audio && !(ctx->suppress_tts_for_command && ctx->is_command)) {
            for (int i = 0; i < 6; i++) {
                play_tts_silence_20ms();
            }
        }

        ESP_LOGI(TAG, "cloud TTS stream done: written=%lu read=%lu",
                 (unsigned long)ctx->tts_file_bytes,
                 (unsigned long)read_bytes);
        return;
    }

    /*
     * 不播放 TTS 且没有 ringbuf 时，只等待服务端 TTS 结束或超时。
     * 待机唤醒阶段会走到这里。
     */
    if (ctx->tts_ringbuf == NULL) {
        while (!ctx->tts_done) {
            EventBits_t bits = xEventGroupGetBits(ctx->event_group);
            if (bits & DB_BIT_FAILED) {
                break;
            }

            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - start_ms > total_timeout_ms) {
                ESP_LOGW(TAG, "TTS wait-only timeout");
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(20));
        }

        return;
    }

    int64_t first_wait_start_ms = start_ms;
    int64_t prebuffer_start_ms = start_ms;
    int64_t last_audio_ms = start_ms;

    bool played_once = false;

    while (!ctx->tts_done && ctx->tts_bytes_rx == 0) {
        EventBits_t bits = xEventGroupGetBits(ctx->event_group);
        if (bits & DB_BIT_FAILED) {
            break;
        }

        int64_t now_ms = esp_timer_get_time() / 1000;

        if (now_ms - first_wait_start_ms > DOUBAO_TTS_FIRST_PACKET_TIMEOUT_MS) {
            ESP_LOGW(TAG, "TTS first packet timeout");
            return;
        }

        if (now_ms - start_ms > total_timeout_ms) {
            ESP_LOGW(TAG, "TTS total timeout before first packet");
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (ctx->tts_bytes_rx > 0) {
        prebuffer_start_ms = esp_timer_get_time() / 1000;
        last_audio_ms = prebuffer_start_ms;
    }

    while (!ctx->tts_done &&
           ctx->tts_bytes_rx < DOUBAO_TTS_PREBUFFER_BYTES) {
        EventBits_t bits = xEventGroupGetBits(ctx->event_group);
        if (bits & DB_BIT_FAILED) {
            break;
        }

        int64_t now_ms = esp_timer_get_time() / 1000;

        if (now_ms - prebuffer_start_ms > DOUBAO_TTS_PREBUFFER_TIMEOUT_MS) {
            break;
        }

        if (now_ms - start_ms > total_timeout_ms) {
            ESP_LOGW(TAG, "TTS total timeout during prebuffer");
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    while (1) {
        size_t item_size = 0;

        uint8_t *item = (uint8_t *)xRingbufferReceive(
            ctx->tts_ringbuf,
            &item_size,
            pdMS_TO_TICKS(5)
        );

        if (item != NULL) {
            bool should_play_audio = ctx->play_tts_audio &&
                !(ctx->suppress_tts_for_command && ctx->is_command);

            if (should_play_audio) {
                played_once = true;
                last_audio_ms = esp_timer_get_time() / 1000;

                if (DOUBAO_TTS_SAMPLE_RATE == 16000) {
                    int samples = item_size / sizeof(int16_t);
                    if (samples > 0) {
                        play_pcm16k_mono_chunk((const int16_t *)item, samples);
                    }
                } else {
                    play_pcm24k_as_16k_chunk(ds_state, item, item_size);
                }
            } else {
                last_audio_ms = esp_timer_get_time() / 1000;
            }

            vRingbufferReturnItem(ctx->tts_ringbuf, item);
        } else {
            int64_t now_ms = esp_timer_get_time() / 1000;

            if (ctx->tts_done) {
                break;
            }

            if (played_once) {
                play_tts_silence_20ms();

                if (now_ms - last_audio_ms > DOUBAO_TTS_IDLE_TIMEOUT_MS) {
                    ESP_LOGW(TAG, "TTS idle timeout");
                    break;
                }
            }

            if (now_ms - start_ms > total_timeout_ms) {
                ESP_LOGW(TAG, "TTS total timeout");
                break;
            }
        }

        EventBits_t bits = xEventGroupGetBits(ctx->event_group);
        if (bits & DB_BIT_FAILED) {
            break;
        }
    }

    if (ctx->play_tts_audio && !(ctx->suppress_tts_for_command && ctx->is_command)) {
        for (int i = 0; i < 6; i++) {
            play_tts_silence_20ms();
        }
    }
}

/* ================= 豆包录音：复用本地 VAD，返回整句 PCM ================= */

static esp_err_t record_mic_pcm_vad(int duration_ms,
                                    uint8_t **pcm_out,
                                    uint32_t *pcm_len_out)
{
    if (pcm_out == NULL || pcm_len_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *pcm_out = NULL;
    *pcm_len_out = 0;

    duration_ms = clamp_int(duration_ms, 500, MIC_MAX_RECORD_MS);

    int total_samples = AUDIO_SAMPLE_RATE * duration_ms / 1000;
    int total_bytes = total_samples * sizeof(int16_t);

    ESP_LOGI(TAG, "Start MIC VAD recording for Doubao: %d ms max", duration_ms);

    int16_t *rx_stereo = heap_caps_malloc(MIC_STEREO_BYTES, MALLOC_CAP_INTERNAL);
    int16_t *tx_mono = heap_caps_malloc(MIC_MONO_BYTES, MALLOC_CAP_INTERNAL);

    int16_t *record_mono = heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (record_mono == NULL) {
        record_mono = heap_caps_malloc(total_bytes, MALLOC_CAP_INTERNAL);
    }

    if (rx_stereo == NULL || tx_mono == NULL || record_mono == NULL) {
        ESP_LOGE(TAG, "MIC buffer malloc failed");
        free(rx_stereo);
        free(tx_mono);
        free(record_mono);
        return ESP_ERR_NO_MEM;
    }

    bool voice_started = false;
    int recorded_samples = 0;
    int elapsed_samples = 0;
    int voice_ms = 0;
    int silence_ms_after_voice = 0;

    while (elapsed_samples < total_samples) {
        int remain_samples = total_samples - elapsed_samples;
        int samples_this = remain_samples > MIC_FRAME_SAMPLES ? MIC_FRAME_SAMPLES : remain_samples;
        int stereo_bytes = samples_this * AUDIO_CHANNELS * sizeof(int16_t);

        memset(rx_stereo, 0, MIC_STEREO_BYTES);
        memset(tx_mono, 0, MIC_MONO_BYTES);

        esp_err_t ret = esp_codec_dev_read(codec_dev, rx_stereo, stereo_bytes);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "esp_codec_dev_read failed: %s", esp_err_to_name(ret));
            break;
        }

        stereo_to_mono_left(rx_stereo, tx_mono, samples_this);

        int avg = 0;
        int peak = 0;
        bool has_voice = mic_frame_has_voice(tx_mono, samples_this, &avg, &peak);
        int frame_ms = samples_this * 1000 / AUDIO_SAMPLE_RATE;

        if (!voice_started) {
            if (has_voice) {
                voice_started = true;
                voice_ms += frame_ms;
                silence_ms_after_voice = 0;
                ESP_LOGI(TAG, "VAD voice start: avg=%d, peak=%d", avg, peak);
            } else {
                elapsed_samples += samples_this;
                continue;
            }
        } else {
            if (has_voice) {
                voice_ms += frame_ms;
                silence_ms_after_voice = 0;
            } else {
                silence_ms_after_voice += frame_ms;
            }
        }

        if (recorded_samples + samples_this <= total_samples) {
            memcpy(&record_mono[recorded_samples], tx_mono, samples_this * sizeof(int16_t));
            recorded_samples += samples_this;
        }

        elapsed_samples += samples_this;


        if (voice_started &&
            voice_ms >= MIC_MIN_VOICE_MS &&
            silence_ms_after_voice >= MIC_END_SILENCE_MS) {
            ESP_LOGI(TAG, "VAD voice end: voice_ms=%d, silence_ms=%d",
                     voice_ms, silence_ms_after_voice);
            break;
        }
    }

    free(rx_stereo);
    free(tx_mono);

    if (!voice_started || voice_ms < MIC_MIN_VOICE_MS || recorded_samples <= 0) {
        ESP_LOGI(TAG, "No voice detected");
        free(record_mono);
        return ESP_OK;
    }

    *pcm_out = (uint8_t *)record_mono;
    *pcm_len_out = recorded_samples * sizeof(int16_t);

    ESP_LOGI(TAG, "Doubao MIC PCM ready: %lu bytes", (unsigned long)*pcm_len_out);
    return ESP_OK;
}

/* ================= 豆包单轮对话：录音PCM -> WebSocket -> 播放音频 ================= */

static esp_err_t doubao_session_open(doubao_session_t *s)
{
    if (s == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(s, 0, sizeof(*s));

    if (doubao_str_invalid(DOUBAO_APP_ID) || doubao_str_invalid(DOUBAO_ACCESS_KEY)) {
        ESP_LOGE(TAG, "Please configure DOUBAO_APP_ID and DOUBAO_ACCESS_KEY");
        return ESP_FAIL;
    }

    s->ctx.event_group = xEventGroupCreate();
    s->ctx.tts_ringbuf = NULL;

    if (s->ctx.event_group == NULL) {
        ESP_LOGE(TAG, "Doubao event_group create failed");
        return ESP_ERR_NO_MEM;
    }

    doubao_make_uuid(s->ctx.session_id);

    int header_len = snprintf(s->headers, sizeof(s->headers),
        "X-Api-App-ID: %s\r\n"
        "X-Api-Access-Key: %s\r\n"
        "X-Api-Resource-Id: %s\r\n"
        "X-Api-App-Key: %s\r\n"
        "X-Api-Connect-Id: %s\r\n",
        DOUBAO_APP_ID,
        DOUBAO_ACCESS_KEY,
        DOUBAO_RESOURCE_ID,
        DOUBAO_APP_KEY,
        DOUBAO_CONNECT_ID);

    ESP_LOGI(TAG, "Doubao headers len=%d", header_len);

    if (header_len <= 0 || header_len >= sizeof(s->headers)) {
        ESP_LOGE(TAG, "Doubao headers truncated");
        vEventGroupDelete(s->ctx.event_group);
        memset(s, 0, sizeof(*s));
        return ESP_FAIL;
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri = DOUBAO_WS_URL,
        .headers = s->headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 8192,
        .task_stack = 8192,
        .network_timeout_ms = 15000,
        .reconnect_timeout_ms = 1000,
        .disable_auto_reconnect = true,
    };

    s->ws = esp_websocket_client_init(&ws_cfg);
    if (s->ws == NULL) {
        vEventGroupDelete(s->ctx.event_group);
        memset(s, 0, sizeof(*s));
        return ESP_FAIL;
    }

    esp_websocket_register_events(s->ws, WEBSOCKET_EVENT_ANY, doubao_ws_event_handler, &s->ctx);

    esp_err_t ret = esp_websocket_client_start(s->ws);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket start failed: %s", esp_err_to_name(ret));
        doubao_session_close(s);
        return ret;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s->ctx.event_group,
        DB_BIT_WS_CONNECTED | DB_BIT_FAILED,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(12000)
    );

    if (!(bits & DB_BIT_WS_CONNECTED) || (bits & DB_BIT_FAILED)) {
        ESP_LOGE(TAG, "WebSocket connect timeout/fail");
        doubao_session_close(s);
        return ESP_FAIL;
    }

    ret = doubao_send_json_event(s->ws, DB_EVENT_START_CONNECTION, NULL, "{}");
    if (ret != ESP_OK) {
        doubao_session_close(s);
        return ret;
    }

    bits = xEventGroupWaitBits(
        s->ctx.event_group,
        DB_BIT_CONNECTION_STARTED | DB_BIT_FAILED,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(10000)
    );

    if (!(bits & DB_BIT_CONNECTION_STARTED) || (bits & DB_BIT_FAILED)) {
        ESP_LOGE(TAG, "StartConnection failed");
        doubao_session_close(s);
        return ESP_FAIL;
    }

    s->active = true;

    ESP_LOGI(TAG, "Doubao persistent session ready");
    return ESP_OK;
}

static void doubao_session_close(doubao_session_t *s)
{
    if (s == NULL) {
        return;
    }

    if (s->ws != NULL) {
        if (s->ctx.session_started) {
            doubao_send_json_event(s->ws, DB_EVENT_FINISH_SESSION, s->ctx.session_id, "{}");
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (s->ctx.connection_started) {
            doubao_send_json_event(s->ws, DB_EVENT_FINISH_CONNECTION, NULL, "{}");
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        esp_websocket_client_stop(s->ws);
        esp_websocket_client_destroy(s->ws);
        s->ws = NULL;
    }

    if (s->ctx.rx_buf != NULL) {
        free(s->ctx.rx_buf);
        s->ctx.rx_buf = NULL;
    }

    if (s->ctx.tts_file_wr != NULL) {
        fclose(s->ctx.tts_file_wr);
        s->ctx.tts_file_wr = NULL;
    }

    s->ctx.tts_file_mode = false;
    s->ctx.tts_file_bytes = 0;
    s->ctx.tts_file_total_bytes = 0;
    s->ctx.tts_file_pending_bytes = 0;
    s->ctx.tts_file_too_large = false;

    if (s->ctx.tts_ringbuf != NULL) {
        vRingbufferDelete(s->ctx.tts_ringbuf);
        s->ctx.tts_ringbuf = NULL;
    }

    if (s->ctx.event_group != NULL) {
        vEventGroupDelete(s->ctx.event_group);
        s->ctx.event_group = NULL;
    }

    memset(s, 0, sizeof(*s));
}


static esp_err_t doubao_start_one_session(doubao_session_t *s)
{
    if (s == NULL || s->ws == NULL || !s->active) {
        return ESP_ERR_INVALID_ARG;
    }

    doubao_ctx_t *ctx = &s->ctx;

    /*
     * 每一轮使用新的 session_id。
     */
    doubao_make_uuid(ctx->session_id);

    ctx->session_started = false;
    ctx->tts_done = false;
    ctx->failed = false;
    ctx->asr_text[0] = '\0';
    ctx->chat_text[0] = '\0';
    ctx->tts_bytes_rx = 0;
    ctx->tts_file_bytes = 0;
    ctx->tts_file_total_bytes = 0;
    ctx->tts_file_pending_bytes = 0;
    ctx->tts_file_too_large = false;
    ctx->tts_play_started = false;
    ctx->cmd_executed = false;

    xEventGroupClearBits(
        ctx->event_group,
        DB_BIT_SESSION_STARTED | DB_BIT_TTS_DONE | DB_BIT_FAILED | DB_BIT_WS_DISCONNECTED
    );

    char *start_session_json = doubao_build_start_session_json();
    if (start_session_json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = doubao_send_json_event(
        s->ws,
        DB_EVENT_START_SESSION,
        ctx->session_id,
        start_session_json
    );

    free(start_session_json);

    if (ret != ESP_OK) {
        return ret;
    }

    EventBits_t bits = xEventGroupWaitBits(
        ctx->event_group,
        DB_BIT_SESSION_STARTED | DB_BIT_FAILED,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(10000)
    );

    if (!(bits & DB_BIT_SESSION_STARTED) || (bits & DB_BIT_FAILED)) {
        ESP_LOGE(TAG, "StartSession failed in reused connection");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Doubao one session started: %s", ctx->session_id);
    return ESP_OK;
}

static esp_err_t doubao_run_one_turn_reuse(doubao_session_t *s,
                                           uint8_t *pcm,
                                           uint32_t pcm_len,
                                           char *asr_out,
                                           size_t asr_out_size,
                                           bool enable_motor_cmd,
                                           bool play_tts_audio,
                                           bool *is_cmd_out)
{
    if (asr_out != NULL && asr_out_size > 0) {
        asr_out[0] = '\0';
    }

    if (is_cmd_out != NULL) {
        *is_cmd_out = false;
    }

    if (s == NULL || s->ws == NULL || !s->active || pcm == NULL || pcm_len == 0) {
        if (pcm != NULL) {
            free(pcm);
        }
        return ESP_ERR_INVALID_ARG;
    }

    doubao_ctx_t *ctx = &s->ctx;

    /*
     * 清空上一轮状态，但不要清空 connection/session 状态。
     */
    ctx->tts_done = false;
    ctx->failed = false;
    ctx->asr_text[0] = '\0';
    ctx->chat_text[0] = '\0';
    ctx->tts_bytes_rx = 0;
    ctx->tts_file_bytes = 0;
    ctx->tts_file_total_bytes = 0;
    ctx->tts_file_pending_bytes = 0;
    ctx->tts_file_too_large = false;
    ctx->tts_play_started = false;
    ctx->cmd_executed = false;
    ctx->is_command = false;
    ctx->enable_motor_cmd = enable_motor_cmd;
    ctx->play_tts_audio = play_tts_audio;
    ctx->suppress_tts_for_command = DOUBAO_SUPPRESS_TTS_FOR_COMMAND ? true : false;

    xEventGroupClearBits(
        ctx->event_group,
        DB_BIT_TTS_DONE | DB_BIT_FAILED | DB_BIT_WS_DISCONNECTED
    );

    if (ctx->rx_buf != NULL) {
        free(ctx->rx_buf);
        ctx->rx_buf = NULL;
        ctx->rx_len = 0;
        ctx->rx_expected = 0;
    }

    if (ctx->tts_file_wr != NULL) {
        fclose(ctx->tts_file_wr);
        ctx->tts_file_wr = NULL;
    }

    ctx->tts_file_mode = false;
    ctx->tts_file_bytes = 0;
    ctx->tts_file_total_bytes = 0;
    ctx->tts_file_pending_bytes = 0;
    ctx->tts_file_too_large = false;

    if (ctx->tts_ringbuf != NULL) {
        vRingbufferDelete(ctx->tts_ringbuf);
        ctx->tts_ringbuf = NULL;
    }
    esp_err_t ret = doubao_start_one_session(s);
    if (ret != ESP_OK) {
        free(pcm);
        return ret;
    }

    ESP_LOGI(TAG, "Doubao reuse turn start, pcm_len=%lu", (unsigned long)pcm_len);

    uint32_t sent = 0;
    ret = ESP_OK;

    while (sent < pcm_len) {
        uint32_t chunk = pcm_len - sent;
        if (chunk > DOUBAO_FRAME_BYTES) {
            chunk = DOUBAO_FRAME_BYTES;
        }

        ret = doubao_send_audio_event(s->ws, ctx->session_id, pcm + sent, chunk);
        if (ret != ESP_OK) {
            free(pcm);
            return ret;
        }

        sent += chunk;

        /*
         * 这里仍按 20ms 发送。
         * 后续如果要进一步提速，可以改成边录边发。
         */
        vTaskDelay(pdMS_TO_TICKS(DOUBAO_FRAME_MS));
    }

    free(pcm);
    pcm = NULL;

    ESP_LOGI(TAG, "Doubao audio uploaded in reused session: %lu bytes", (unsigned long)sent);

    /*
     * 有声音输出的轮次使用 SPIFFS 边写边播。
     * 待机唤醒识别阶段 play_tts_audio=false，不缓存也不播放云端 TTS。
     */
    ctx->tts_file_mode = play_tts_audio;
    ctx->tts_file_bytes = 0;
    ctx->tts_file_total_bytes = 0;
    ctx->tts_file_pending_bytes = 0;
    ctx->tts_file_too_large = false;
    ctx->tts_ringbuf = NULL;

    if (ctx->tts_file_mode) {
        remove(DOUBAO_CLOUD_TTS_STREAM_PCM);

        /*
         * 大文件反复写入前主动触发 GC，避免上一次大 TTS 删除后
         * 空间尚未整理，下一轮一开始就 fwrite failed。
         */
        esp_err_t gc_ret = esp_spiffs_gc("storage", DOUBAO_TTS_FILE_MAX_BYTES + (64 * 1024));
        if (gc_ret != ESP_OK) {
            ESP_LOGW(TAG, "SPIFFS GC before cloud TTS: %s", esp_err_to_name(gc_ret));
        }

        size_t spiffs_total = 0;
        size_t spiffs_used = 0;
        esp_spiffs_info("storage", &spiffs_total, &spiffs_used);
        ESP_LOGI(TAG, "SPIFFS before cloud TTS: used=%u total=%u",
                 (unsigned int)spiffs_used,
                 (unsigned int)spiffs_total);

        ctx->tts_file_wr = fopen(DOUBAO_CLOUD_TTS_STREAM_PCM, "wb");
        if (ctx->tts_file_wr == NULL) {
            ESP_LOGE(TAG, "open cloud TTS stream cache failed");
            return ESP_FAIL;
        }

        setvbuf(ctx->tts_file_wr, NULL, _IOFBF, DOUBAO_TTS_FILE_FLUSH_BYTES);

        ESP_LOGI(TAG, "cloud TTS stream cache opened");
    }

    lcd_status_post("THINK");
    ret = doubao_send_json_event(s->ws, DB_EVENT_END_ASR, ctx->session_id, "{}");
    if (ret != ESP_OK) {
        return ret;
    }

    downsample_24k_to_16k_state_t ds_state = {
        .group = {0, 0, 0},
        .group_count = 0,
    };

    if (play_tts_audio) {
        lcd_status_post("SPEAK");
    }

    doubao_drain_tts_audio(ctx, &ds_state, DOUBAO_TTS_TOTAL_TIMEOUT_MS);

    bool is_cmd = ctx->cmd_executed;

    if (ctx->asr_text[0] != '\0') {
        ESP_LOGI(TAG, "Final ASR text: %s", ctx->asr_text);

        if (asr_out != NULL && asr_out_size > 0) {
            strncpy(asr_out, ctx->asr_text, asr_out_size - 1);
            asr_out[asr_out_size - 1] = '\0';
        }

        if (enable_motor_cmd && !ctx->cmd_executed) {
            is_cmd = doubao_try_execute_voice_command(ctx->asr_text);
            ctx->cmd_executed = is_cmd;
            ctx->is_command = is_cmd;
        }

        if (is_cmd) {
            ESP_LOGI(TAG, "Voice command queued/executed");
        } else {
            ESP_LOGI(TAG, "Voice input treated as chat/wake");
        }
    }

    if (is_cmd_out != NULL) {
        *is_cmd_out = is_cmd;
    }

    if (ctx->chat_text[0] != '\0') {
        ESP_LOGI(TAG, "Final Chat text: %s", ctx->chat_text);
    }

    doubao_send_json_event(s->ws, DB_EVENT_FINISH_SESSION, ctx->session_id, "{}");
    vTaskDelay(pdMS_TO_TICKS(100));
    ctx->session_started = false;

    if (ctx->tts_file_wr != NULL) {
        fclose(ctx->tts_file_wr);
        ctx->tts_file_wr = NULL;
    }

    ctx->tts_file_mode = false;
    ctx->tts_file_bytes = 0;
    ctx->tts_file_total_bytes = 0;
    ctx->tts_file_pending_bytes = 0;
    ctx->tts_file_too_large = false;

    if (ctx->tts_ringbuf != NULL) {
        vRingbufferDelete(ctx->tts_ringbuf);
        ctx->tts_ringbuf = NULL;
    }

    EventBits_t bits = xEventGroupGetBits(ctx->event_group);
    if ((bits & DB_BIT_FAILED) || (bits & DB_BIT_WS_DISCONNECTED)) {
        ESP_LOGE(TAG, "Doubao reused turn failed/disconnected");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* ================= 豆包主语音任务 ================= */

static void voice_cloud_task(void *arg)
{
    doubao_session_t session;
    memset(&session, 0, sizeof(session));

    bool interaction_active = false;

    while (1) {
        if (!session.active) {
            lcd_status_post("CLOUD");
            esp_err_t open_ret = doubao_session_open(&session);
            if (open_ret != ESP_OK) {
                ESP_LOGE(TAG, "Doubao persistent session open failed: %s", esp_err_to_name(open_ret));
                lcd_status_post("ERROR");
                doubao_session_close(&session);
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            interaction_active = false;
            lcd_status_post("STANDBY");
        }

        if (!interaction_active) {
            uint8_t *wake_pcm = NULL;
            uint32_t wake_len = 0;
            char wake_asr[256] = {0};
            lcd_status_post("LISTEN");
            esp_err_t rec_ret = record_mic_pcm_vad(LELE_WAKE_RECORD_MS, &wake_pcm, &wake_len);
            if (rec_ret != ESP_OK || wake_pcm == NULL || wake_len == 0) {
                if (wake_pcm != NULL) {
                    free(wake_pcm);
                    wake_pcm = NULL;
                }

                vTaskDelay(pdMS_TO_TICKS(LELE_STANDBY_DELAY_MS));
                continue;
            }

            /*
             * 待机状态只借助云端 ASR 判断是否听到“乐乐”。
             * 这里禁用电机命令，并丢弃云端 TTS，避免未唤醒时胡乱说话。
             */
            lcd_status_post("UPLOAD");
            esp_err_t turn_ret = doubao_run_one_turn_reuse(
                &session,
                wake_pcm,
                wake_len,
                wake_asr,
                sizeof(wake_asr),
                false,
                false,
                NULL
            );
            wake_pcm = NULL;

            if (turn_ret != ESP_OK) {
            lcd_status_post("ERROR");
            doubao_session_close(&session);
                vTaskDelay(pdMS_TO_TICKS(1500));
                continue;
            }

            ESP_LOGI(TAG, "Wake ASR text: %s", wake_asr);

            if (text_has_wake_word(wake_asr)) {
                interaction_active = true;
                lcd_status_post("AWAKE");
/*
                 * 建议把 /spiffs/awake.pcm 做成“我在”。
                 * 如果文件不存在，只会打印错误，不影响后续交互。
                 */
                play_local_pcm_file(LELE_WAKE_ACK_PCM);
                lcd_status_post("ACTIVE");
                vTaskDelay(pdMS_TO_TICKS(200));
            } else {
                ESP_LOGI(TAG, "Wake word not detected, keep standby");
                lcd_status_post("STANDBY");
            }

            continue;
        }

        /*
         * 已唤醒：继续等待下一句话。
         * 没有检测到语音时不自动退出；
         * 只有识别到“撤了/没事了/不用了/再见”等关键词才回到待机。
         */
        uint8_t *mic_pcm = NULL;
        uint32_t mic_len = 0;
        char asr_text[256] = {0};
        bool is_cmd = false;
        lcd_status_post("LISTEN");
        esp_err_t rec_ret = record_mic_pcm_vad(LELE_ACTIVE_RECORD_MS, &mic_pcm, &mic_len);
        if (rec_ret != ESP_OK || mic_pcm == NULL || mic_len == 0) {
            if (mic_pcm != NULL) {
                free(mic_pcm);
                mic_pcm = NULL;
            }

            /*
             * 活跃状态下没有检测到语音，不再自动撤回。
             * 继续保持唤醒状态，等待下一次有效语音。
             */
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        ESP_LOGI(TAG, "DEBUG: active turn, mic_len=%lu", (unsigned long)mic_len);

        lcd_status_post("UPLOAD");
        esp_err_t turn_ret = doubao_run_one_turn_reuse(
            &session,
            mic_pcm,
            mic_len,
            asr_text,
            sizeof(asr_text),
            true,
            true,
            &is_cmd
        );
        mic_pcm = NULL;

        ESP_LOGI(TAG, "DEBUG: active turn ret=%s, asr=%s, is_cmd=%d",
                 esp_err_to_name(turn_ret), asr_text, is_cmd ? 1 : 0);

        if (turn_ret != ESP_OK) {
            lcd_status_post("ERROR");
            doubao_session_close(&session);
            interaction_active = false;
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }

        /*
         * 只有识别到退出关键词才撤回待机。
         * 这里复用原来的撤回动作：播放 leave.pcm、停止电机、回到待机。
         */
        if (text_has_sleep_word(asr_text)) {
            lcd_status_post("SLEEP");
            play_local_pcm_file(LELE_SLEEP_ACK_PCM);
            motor_send_cmd(MOTOR_CMD_STOP, 0, 0);
            interaction_active = false;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        lcd_status_post(is_cmd ? "CMD" : "READY");
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}