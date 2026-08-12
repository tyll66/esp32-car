#include "doubao_client.h"


static const char *TAG = "DOUBAO_CLIENT";

/* ============================================================
 * 默认配置
 * 如果 app_config.h 里已经定义了这些宏，这里不会重复定义。
 * ============================================================ */

#ifndef DOUBAO_WS_URL
#define DOUBAO_WS_URL        "wss://openspeech.bytedance.com/api/v3/realtime/dialogue"
#endif

#ifndef DOUBAO_RESOURCE_ID
#define DOUBAO_RESOURCE_ID   "volc.speech.dialog"
#endif

#ifndef DOUBAO_CONNECT_ID
#define DOUBAO_CONNECT_ID    "esp32s3-voice-car-001"
#endif

#ifndef DOUBAO_MODEL_VERSION
#define DOUBAO_MODEL_VERSION "1.2.1.1"
#endif

#ifndef DOUBAO_SPEAKER
#define DOUBAO_SPEAKER       "zh_female_vv_jupiter_bigtts"
#endif

#ifndef DOUBAO_TTS_SAMPLE_RATE
#define DOUBAO_TTS_SAMPLE_RATE 16000
#endif

#ifndef DOUBAO_TTS_FORMAT
#define DOUBAO_TTS_FORMAT "pcm_s16le"
#endif

#ifndef ASSISTANT_NAME_CN
#define ASSISTANT_NAME_CN "乐乐"
#endif

#ifndef WAKE_WORD_CN
#define WAKE_WORD_CN "乐乐"
#endif

#ifndef LELE_WAKE_RECORD_MS
#define LELE_WAKE_RECORD_MS 2500
#endif

#ifndef LELE_ACTIVE_RECORD_MS
#define LELE_ACTIVE_RECORD_MS 5000
#endif

#ifndef LELE_STANDBY_DELAY_MS
#define LELE_STANDBY_DELAY_MS 300
#endif

#ifndef LELE_WAKE_ACK_PCM
#define LELE_WAKE_ACK_PCM "/spiffs/awake.pcm"
#endif

#ifndef LELE_SLEEP_ACK_PCM
#define LELE_SLEEP_ACK_PCM "/spiffs/leave.pcm"
#endif

#ifndef DOUBAO_SUPPRESS_TTS_FOR_COMMAND
#define DOUBAO_SUPPRESS_TTS_FOR_COMMAND 1
#endif

#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 16000
#endif

#ifndef AUDIO_CHANNELS
#define AUDIO_CHANNELS 2
#endif

#ifndef DOUBAO_APP_ID
#define DOUBAO_APP_ID "YOUR_DOUBAO_APP_ID"
#endif

#ifndef DOUBAO_ACCESS_KEY
#define DOUBAO_ACCESS_KEY "YOUR_DOUBAO_ACCESS_KEY"
#endif

#ifndef DOUBAO_APP_KEY
#define DOUBAO_APP_KEY "YOUR_DOUBAO_APP_KEY"
#endif

/* ============================================================
 * 音频参数
 * ============================================================ */

#define DOUBAO_FRAME_MS              20
#define DOUBAO_FRAME_SAMPLES         320
#define DOUBAO_FRAME_BYTES           (DOUBAO_FRAME_SAMPLES * sizeof(int16_t))

#define MIC_FRAME_SAMPLES            512
#define MIC_STEREO_BYTES             (MIC_FRAME_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t))
#define MIC_MONO_BYTES               (MIC_FRAME_SAMPLES * sizeof(int16_t))

#define MIC_MAX_RECORD_MS            6000
#define MIC_MIN_RECORD_MS            500

/*
 * 简易 VAD 阈值。
 * 你后续可根据串口打印的 avg / peak 继续调。
 */
#define MIC_VAD_START_AVG            1200
#define MIC_VAD_START_PEAK           4500
#define MIC_MIN_VOICE_MS             120
#define MIC_END_SILENCE_MS           300

#define PCM_MONO_SAMPLES_PER_CHUNK   512
#define PCM_MONO_BYTES_PER_CHUNK     (PCM_MONO_SAMPLES_PER_CHUNK * sizeof(int16_t))
#define PCM_STEREO_BYTES_PER_CHUNK   (PCM_MONO_SAMPLES_PER_CHUNK * 2 * sizeof(int16_t))

#define TTS_RINGBUF_BYTES            (128 * 1024)
#define TTS_PREBUFFER_BYTES          (4 * 1024)
#define TTS_FIRST_PACKET_TIMEOUT_MS  8000
#define TTS_TOTAL_TIMEOUT_MS         60000
#define TTS_IDLE_TIMEOUT_MS          15000
#define MAX_WS_FRAME_BYTES           (32 * 1024)

/* ============================================================
 * 豆包二进制协议常量
 * ============================================================ */

#define DB_BIT_WS_CONNECTED          BIT0
#define DB_BIT_WS_DISCONNECTED       BIT1
#define DB_BIT_CONNECTION_STARTED    BIT2
#define DB_BIT_SESSION_STARTED       BIT3
#define DB_BIT_TTS_DONE              BIT4
#define DB_BIT_FAILED                BIT5

#define DB_MSG_FULL_CLIENT_REQUEST   0x1
#define DB_MSG_AUDIO_ONLY_REQUEST    0x2
#define DB_MSG_FULL_SERVER_RESPONSE  0x9
#define DB_MSG_AUDIO_ONLY_RESPONSE   0xB
#define DB_MSG_ERROR_INFORMATION     0xF

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

#define DB_EVENT_TTS_RESPONSE        352
#define DB_EVENT_TTS_ENDED           359

#define DB_EVENT_ASR_RESPONSE        451
#define DB_EVENT_ASR_ENDED           459

#define DB_EVENT_CHAT_RESPONSE       550
#define DB_EVENT_CHAT_ENDED          559


LV_IMAGE_DECLARE(img_car_think);
LV_IMAGE_DECLARE(img_car_talk);
LV_IMAGE_DECLARE(img_car_listening);

/* ============================================================
 * 类型定义
 * ============================================================ */

typedef struct {
    int16_t group[3];
    int group_count;
} downsample_24k_to_16k_state_t;

typedef struct {
    EventGroupHandle_t event_group;
    //环形缓冲区，存pcm音频数据
    RingbufHandle_t tts_ringbuf;

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

    bool cmd_executed;
    bool is_command;
    bool enable_command;
    bool play_tts_audio;
    bool suppress_tts_for_command;
} doubao_ctx_t;

typedef struct {
    doubao_ctx_t ctx;
    esp_websocket_client_handle_t ws;
    char headers[2048];
    bool active;
} doubao_session_t;

/* ============================================================
 * 模块状态
 * ============================================================ */

static TaskHandle_t s_doubao_task_handle = NULL;
static volatile bool s_doubao_started = false;
static volatile bool s_doubao_stop_req = false;

/* ============================================================
 * 前置声明
 * ============================================================ */

static void voice_cloud_task(void *arg);

static esp_err_t doubao_session_open(doubao_session_t *s);
static void doubao_session_close(doubao_session_t *s);
static esp_err_t doubao_start_one_session(doubao_session_t *s);

static esp_err_t doubao_run_one_turn_reuse(
    doubao_session_t *s,
    uint8_t *pcm,
    uint32_t pcm_len,
    char *asr_out,
    size_t asr_out_size,
    bool enable_command,
    bool play_tts_audio,
    bool *is_cmd_out
);

static esp_err_t record_mic_pcm_vad(
    int duration_ms,
    uint8_t **pcm_out,
    uint32_t *pcm_len_out
);

static void play_local_pcm_file(const char *path);

/* ============================================================
 * 小工具
 * ============================================================ */

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

static bool doubao_str_invalid(const char *s)
{
    if (s == NULL || s[0] == '\0') {
        return true;
    }

    if (strstr(s, "YOUR_") != NULL ||
        strstr(s, "请填") != NULL) {
        return true;
    }

    return false;
}
//字节转化，转化为大端序，多用于网络，音频，协议
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
           ((uint32_t)p[3]);
}
//创建用于标识对话的ID
static void doubao_make_uuid(char out[37])
{
    uint32_t a = esp_random();
    uint32_t b = esp_random();
    uint32_t c = esp_random();
    uint32_t d = esp_random();

    snprintf(out,
             37,
             "%08lx-%04lx-%04lx-%04lx-%08lx%04lx",
             (unsigned long)a,
             (unsigned long)((b >> 16) & 0xFFFF),
             (unsigned long)(b & 0xFFFF),
             (unsigned long)((c >> 16) & 0xFFFF),
             (unsigned long)c,
             (unsigned long)(d & 0xFFFF));
}
//判断是否需要上传ID
static bool doubao_event_has_session_id(uint32_t event_id)
{
    if (event_id == DB_EVENT_CONNECTION_STARTED ||
        event_id == DB_EVENT_CONNECTION_FAILED ||
        event_id == DB_EVENT_CONNECTION_FINISHED) {
        return false;
    }

    return true;
}

static bool text_has_wake_word(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    //判断父字符串中是否包含子字符串
    if (strstr(text, WAKE_WORD_CN) != NULL ||
        strstr(text, "乐 乐") != NULL ||
        strstr(text, "勒勒") != NULL ||
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

/* ============================================================
 * 本地 PCM 播放
 * ============================================================ */

static void play_local_pcm_file(const char *path)
{
    if (path == NULL) {
        return;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "open local pcm failed: %s", path);
        return;
    }

    ESP_LOGI(TAG, "play local pcm: %s", path);

    int16_t *mono_buf = heap_caps_malloc(
        PCM_MONO_BYTES_PER_CHUNK,
        //优先分配SRAM,8字节对齐
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );

    if (mono_buf == NULL) {
        mono_buf = malloc(PCM_MONO_BYTES_PER_CHUNK);
    }

    if (mono_buf == NULL) {
        ESP_LOGE(TAG, "local pcm buffer malloc failed");
        fclose(fp);
        return;
    }

    audio_es8311_play_silence_ms(80);

    while (1) {
        size_t n = fread(mono_buf, 1, PCM_MONO_BYTES_PER_CHUNK, fp);
        if (n == 0) {
            break;
        }

        size_t samples = n / sizeof(int16_t);
        if (samples > 0) {
            esp_err_t ret = audio_es8311_write_mono_pcm16(mono_buf, samples);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "write local pcm failed: %s", esp_err_to_name(ret));
                break;
            }
        }
    }

    audio_es8311_play_silence_ms(120);

    free(mono_buf);
    fclose(fp);

    ESP_LOGI(TAG, "local pcm done: %s", path);
}

/* ============================================================
 * 麦克风录音 + 简易 VAD
 * ============================================================ */
//计算音量平均值，峰值
static bool mic_frame_has_voice(const int16_t *mono,
                                int samples,
                                int *avg_out,
                                int *peak_out)
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

    if (avg_out != NULL) {
        *avg_out = avg;
    }

    if (peak_out != NULL) {
        *peak_out = peak;
    }

    return avg >= MIC_VAD_START_AVG || peak >= MIC_VAD_START_PEAK;
}

static esp_err_t record_mic_pcm_vad(int duration_ms,
                                    uint8_t **pcm_out,
                                    uint32_t *pcm_len_out)
{
    if (pcm_out == NULL || pcm_len_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *pcm_out = NULL;
    *pcm_len_out = 0;

    duration_ms = clamp_int(duration_ms, MIC_MIN_RECORD_MS, MIC_MAX_RECORD_MS);

    int total_samples = AUDIO_SAMPLE_RATE * duration_ms / 1000;
    int total_bytes = total_samples * sizeof(int16_t);

    ESP_LOGI(TAG, "Start MIC VAD recording, max=%d ms", duration_ms);
    //存放硬件麦克风原始双声道立体声 PCM 数据
    int16_t *rx_stereo = heap_caps_malloc(
        MIC_STEREO_BYTES,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );
    //存放混合后的单声道 PCM 数据
    int16_t *frame_mono = heap_caps_malloc(
        MIC_MONO_BYTES,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );

    int16_t *record_mono = heap_caps_malloc(
        total_bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (record_mono == NULL) {
        record_mono = heap_caps_malloc(
            total_bytes,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
    }

    if (rx_stereo == NULL || frame_mono == NULL || record_mono == NULL) {
        ESP_LOGE(TAG, "MIC buffer malloc failed");
        free(rx_stereo);
        free(frame_mono);
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
        int samples_this = remain_samples > MIC_FRAME_SAMPLES ?
                           MIC_FRAME_SAMPLES :
                           remain_samples;

        int stereo_bytes = samples_this * AUDIO_CHANNELS * sizeof(int16_t);
        
        //PCM数据存放区
        memset(rx_stereo, 0, MIC_STEREO_BYTES);
        memset(frame_mono, 0, MIC_MONO_BYTES);

        //将数据存到rx_stereo
        esp_err_t ret = audio_es8311_read(rx_stereo, stereo_bytes);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "audio read failed: %s", esp_err_to_name(ret));
            break;
        }
        //将数据转化为单声道frame_mono
        audio_es8311_stereo_to_mono_left(rx_stereo, frame_mono, samples_this);

        int avg = 0;
        int peak = 0;
        //判断单声道是否有有效数据
        bool has_voice = mic_frame_has_voice(frame_mono, samples_this, &avg, &peak);
        int frame_ms = samples_this * 1000 / AUDIO_SAMPLE_RATE;

        if (!voice_started) {
            if (has_voice) {
                voice_started = true;
                voice_ms += frame_ms;
                silence_ms_after_voice = 0;
                ESP_LOGI(TAG, "VAD start: avg=%d peak=%d", avg, peak);
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
        //把当前这一帧采样全部存进去之后，会不会超出录音缓冲区最大容量。
        if (recorded_samples + samples_this <= total_samples) {
            memcpy(&record_mono[recorded_samples],
                   frame_mono,
                   samples_this * sizeof(int16_t));
            recorded_samples += samples_this;
        }

        elapsed_samples += samples_this;

        if (voice_started &&
            voice_ms >= MIC_MIN_VOICE_MS &&
            silence_ms_after_voice >= MIC_END_SILENCE_MS) {
            ESP_LOGI(TAG,
                     "VAD end: voice_ms=%d silence_ms=%d",
                     voice_ms,
                     silence_ms_after_voice);
            break;
        }
    }

    free(rx_stereo);
    free(frame_mono);

    if (!voice_started || voice_ms < MIC_MIN_VOICE_MS || recorded_samples <= 0) {
        ESP_LOGI(TAG, "No voice detected");
        free(record_mono);
        return ESP_OK;
    }

    *pcm_out = (uint8_t *)record_mono;
    *pcm_len_out = recorded_samples * sizeof(int16_t);

    ESP_LOGI(TAG, "MIC PCM ready: %lu bytes", (unsigned long)*pcm_len_out);

    return ESP_OK;
}

/* ============================================================
 * 豆包帧发送
 * ============================================================ */
//websocket句柄
//消息类型:
// 0x01：音频上行（麦克风语音流）
// 0x02：文本指令下发
// 0x03：心跳包
// 0x04：结束会话标志
// 0x05：云端应答接收）
//标记 payload 载荷内部使用的数据序列化方式，告知服务端如何解析负载：
// 0x00：原始二进制音频（PCM/ADPCM 裸流）
// 0x01：JSON 字符串文本
// 0x02：Protobuf 序列化
// 0x03：MsgPack
//uint32_t event_id 事件序列号
//const char *session_id 会话 ID
//实际要发送的业务数据缓冲区指针，承载核心业务内容
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
    //四字节事件序列号+四字节数据长度+业务载荷本体+...
    uint32_t frame_len = 4 + 4 + payload_len + 4;
    if (session_len > 0) {
        //存session_id长度用的4字节和本体
        frame_len += 4 + session_len;

    }

    uint8_t *frame = heap_caps_malloc(
        frame_len,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );

    if (frame == NULL) {
        frame = malloc(frame_len);
    }

    if (frame == NULL) {
        ESP_LOGE(TAG, "frame malloc failed, len=%lu", (unsigned long)frame_len);
        return ESP_ERR_NO_MEM;
    }

    uint32_t off = 0;

    frame[off++] = 0x11;
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

    int sent = esp_websocket_client_send_bin(
        ws,
        (const char *)frame,
        (int)off,
        pdMS_TO_TICKS(10000)
    );

    free(frame);

    if (sent != (int)off) {
        ESP_LOGE(TAG,
                 "send frame failed, sent=%d want=%lu",
                 sent,
                 (unsigned long)off);
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

    ESP_LOGI(TAG,
             "send json event=%lu len=%u",
             (unsigned long)event_id,
             (unsigned int)strlen(json));

    return doubao_send_frame(
        ws,
        DB_MSG_FULL_CLIENT_REQUEST,
        DB_SERIALIZATION_JSON,
        event_id,
        session_id,
        (const uint8_t *)json,
        strlen(json)
    );
}
//消息类型不同，序列化方式不同，事件id不同
static esp_err_t doubao_send_audio_event(esp_websocket_client_handle_t ws,
                                         const char *session_id,
                                         const uint8_t *pcm,
                                         uint32_t pcm_len)
{
    return doubao_send_frame(
        ws,
        DB_MSG_AUDIO_ONLY_REQUEST,
        DB_SERIALIZATION_RAW,
        DB_EVENT_TASK_REQUEST,
        session_id,
        pcm,
        pcm_len
    );
}

/* ============================================================
 * StartSession JSON
 * ============================================================ */

static char *doubao_build_start_session_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON *asr = cJSON_CreateObject();
    cJSON *asr_audio = cJSON_CreateObject();

    cJSON_AddStringToObject(asr_audio, "format", "pcm");
    cJSON_AddNumberToObject(asr_audio, "sample_rate", 16000);
    cJSON_AddNumberToObject(asr_audio, "channel", 1);
    cJSON_AddItemToObject(asr, "audio_info", asr_audio);
    cJSON_AddItemToObject(root, "asr", asr);

    cJSON *dialog = cJSON_CreateObject();
    cJSON_AddStringToObject(dialog, "bot_name", ASSISTANT_NAME_CN);
    cJSON_AddStringToObject(
        dialog,
        "system_role",
        "你叫乐乐，是桌面小车助手。"
        "只用中文回答。"
        "普通回答最多8个汉字。"
        "不要解释，不要闲聊，不要扩展背景。"
        "用户发出前进、后退、左转、右转、停止等控制命令时，只回答“收到”。"
        "听不清或内容无关时，只回答“没听清”。"
    );

    cJSON_AddStringToObject(
        dialog,
        "speaking_style",
        "普通回答极简短句。"
    );

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

/* ============================================================
 * 豆包帧解析
 * ============================================================ */

static void doubao_parse_json_event(doubao_ctx_t *ctx,
                                    uint32_t event_id,
                                    const uint8_t *payload,
                                    uint32_t payload_len)
{
    if (ctx == NULL) {
        return;
    }

    if (payload == NULL || payload_len == 0) {
        if (event_id == DB_EVENT_CONNECTION_STARTED) {
            ctx->connection_started = true;
            xEventGroupSetBits(ctx->event_group, DB_BIT_CONNECTION_STARTED);
            ESP_LOGI(TAG, "ConnectionStarted");
        } else if (event_id == DB_EVENT_SESSION_STARTED) {
            ctx->session_started = true;
            xEventGroupSetBits(ctx->event_group, DB_BIT_SESSION_STARTED);
            ESP_LOGI(TAG, "SessionStarted");
        } else if (event_id == DB_EVENT_TTS_ENDED) {
            ctx->tts_done = true;
            xEventGroupSetBits(ctx->event_group, DB_BIT_TTS_DONE);
            ESP_LOGI(TAG, "TTSEnded");
        } else if (event_id == DB_EVENT_ASR_ENDED) {
            if (ctx->asr_text[0] == '\0') {
                ESP_LOGW(TAG, "ASR ended with empty text");
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

    ESP_LOGI(TAG,
             "JSON event=%lu len=%lu",
             (unsigned long)event_id,
             (unsigned long)payload_len);

    if (event_id == DB_EVENT_CONNECTION_FAILED ||
        event_id == DB_EVENT_SESSION_FAILED) {
        ESP_LOGE(TAG, "server failed event=%lu: %s", (unsigned long)event_id, json_str);
        ctx->failed = true;
        xEventGroupSetBits(ctx->event_group, DB_BIT_FAILED);
        free(json_str);
        return;
    }

    if (event_id == DB_EVENT_CONNECTION_STARTED) {
        ctx->connection_started = true;
        xEventGroupSetBits(ctx->event_group, DB_BIT_CONNECTION_STARTED);
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
        //获取子节点
        cJSON *results = cJSON_GetObjectItem(root, "results");
        //判断子节点是不是json数组对象
        if (cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) {
            cJSON *first = cJSON_GetArrayItem(results, 0);
            cJSON *text = cJSON_GetObjectItem(first, "text");
            cJSON *is_interim = cJSON_GetObjectItem(first, "is_interim");

            if (cJSON_IsString(text)) {
                bool interim = cJSON_IsBool(is_interim) && cJSON_IsTrue(is_interim);

                strncpy(ctx->asr_text, text->valuestring, sizeof(ctx->asr_text) - 1);
                ctx->asr_text[sizeof(ctx->asr_text) - 1] = '\0';

                ESP_LOGI(TAG,
                         "ASR%s: %s",
                         interim ? "(interim)" : "",
                         ctx->asr_text);

                if (!interim &&
                    !ctx->cmd_executed &&
                    ctx->enable_command) {
                    ctx->cmd_executed = command_router_handle_voice_text(ctx->asr_text);
                    ctx->is_command = ctx->cmd_executed;
                }
            }
        }
    } else if (event_id == DB_EVENT_ASR_ENDED) {
        if (ctx->asr_text[0] == '\0') {
            ESP_LOGW(TAG, "ASR ended empty");
            ctx->tts_done = true;
            xEventGroupSetBits(ctx->event_group, DB_BIT_TTS_DONE);
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
//二进制解析
static void doubao_handle_protocol_frame(doubao_ctx_t *ctx,
                                         const uint8_t *data,
                                         int len)
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
        ESP_LOGW(TAG,
                 "frame truncated event=%lu payload=%lu len=%d off=%d",
                 (unsigned long)event_id,
                 (unsigned long)payload_len,
                 len,
                 off);
        return;
    }

    const uint8_t *payload = data + off;

    ESP_LOGD(TAG,
             "frame type=0x%x ser=0x%x event=%lu payload=%lu",
             msg_type,
             serialization,
             (unsigned long)event_id,
             (unsigned long)payload_len);

    if (msg_type == DB_MSG_ERROR_INFORMATION) {
        ESP_LOGE(TAG, "error frame: %.*s", (int)payload_len, (const char *)payload);
        ctx->failed = true;
        xEventGroupSetBits(ctx->event_group, DB_BIT_FAILED);
        return;
    }

    if (msg_type == DB_MSG_AUDIO_ONLY_RESPONSE &&
        event_id == DB_EVENT_TTS_RESPONSE) {
        if (payload_len == 0 || ctx->tts_ringbuf == NULL) {
            return;
        }

        const uint8_t *p = payload;
        uint32_t remain = payload_len;

        while (remain > 0) {
            uint32_t chunk = remain > 2048 ? 2048 : remain;

            BaseType_t ok = xRingbufferSend(
                ctx->tts_ringbuf,
                p,
                chunk,
                pdMS_TO_TICKS(30)
            );

            if (ok != pdTRUE) {
                ESP_LOGW(TAG, "TTS ringbuf full, drop %lu bytes", (unsigned long)chunk);
                return;
            }

            ctx->tts_bytes_rx += chunk;
            p += chunk;
            remain -= chunk;
        }

        return;
    }

    if (msg_type == DB_MSG_FULL_SERVER_RESPONSE) {
        doubao_parse_json_event(ctx, event_id, payload, payload_len);
    }
}
//websocket回调函数
static void doubao_ws_event_handler(void *handler_args,
                                    esp_event_base_t base,
                                    int32_t event_id,
                                    void *event_data)
{
    (void)base;

    doubao_ctx_t *ctx = (doubao_ctx_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    if (ctx == NULL) {
        return;
    }

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        ctx->ws_connected = true;
        xEventGroupSetBits(ctx->event_group, DB_BIT_WS_CONNECTED);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        ctx->ws_connected = false;
        xEventGroupSetBits(ctx->event_group, DB_BIT_WS_DISCONNECTED);
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        ctx->failed = true;
        xEventGroupSetBits(ctx->event_group, DB_BIT_FAILED);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data == NULL || data->data_ptr == NULL || data->data_len <= 0) {
            break;
        }

        if (data->payload_offset == 0 &&
            data->data_len == data->payload_len) {
            doubao_handle_protocol_frame(
                ctx,
                (const uint8_t *)data->data_ptr,
                data->data_len
            );
            break;
        }

        if (data->payload_offset == 0) {
            free(ctx->rx_buf);
            ctx->rx_buf = NULL;
            ctx->rx_len = 0;
            ctx->rx_expected = data->payload_len;

            if (ctx->rx_expected <= 0 ||
                ctx->rx_expected > MAX_WS_FRAME_BYTES) {
                ESP_LOGW(TAG, "invalid WS payload_len=%d", ctx->rx_expected);
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
            memcpy(ctx->rx_buf + data->payload_offset,
                   data->data_ptr,
                   data->data_len);
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

/* ============================================================
 * TTS 播放
 * ============================================================ */
//24khz转16khz播放
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
    int16_t *out = heap_caps_malloc(
        out_cap * sizeof(int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );

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
        audio_es8311_write_mono_pcm16(out, out_samples);
    }

    free(out);
}
//从 TTS 环形缓冲区持续取出云端下发的语音 PCM 数据，降采样后送到 ES8311 声卡播放；
//同时做多层超时保护、指令静音屏蔽、预缓冲等待、收尾静音填充，完整走完一次 AI 语音播报流程。
static void doubao_drain_tts_audio(doubao_ctx_t *ctx,
                                   downsample_24k_to_16k_state_t *ds_state,
                                   int total_timeout_ms)
{
    if (ctx == NULL) {
        return;
    }

    int64_t start_ms = esp_timer_get_time() / 1000;
    int64_t last_audio_ms = start_ms;
    bool played_once = false;

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

    while (!ctx->tts_done && ctx->tts_bytes_rx < TTS_PREBUFFER_BYTES) {
        EventBits_t bits = xEventGroupGetBits(ctx->event_group);
        if (bits & DB_BIT_FAILED) {
            break;
        }

        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - start_ms > TTS_FIRST_PACKET_TIMEOUT_MS) {
            ESP_LOGW(TAG, "TTS first packet/prebuffer timeout");
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    while (1) {
        size_t item_size = 0;

        uint8_t *item = (uint8_t *)xRingbufferReceive(
            ctx->tts_ringbuf,
            &item_size,
            pdMS_TO_TICKS(10)
        );

        if (item != NULL) {
            bool should_play = ctx->play_tts_audio &&
                !(ctx->suppress_tts_for_command && ctx->is_command);

            if (should_play) {
                played_once = true;
                last_audio_ms = esp_timer_get_time() / 1000;

                if (DOUBAO_TTS_SAMPLE_RATE == 16000) {
                    int samples = item_size / sizeof(int16_t);
                    if (samples > 0) {
                        audio_es8311_write_mono_pcm16((const int16_t *)item, samples);
                    }
                } else {
                    play_pcm24k_as_16k_chunk(ds_state, item, item_size);
                }
            }

            vRingbufferReturnItem(ctx->tts_ringbuf, item);
        } else {
            int64_t now_ms = esp_timer_get_time() / 1000;

            if (ctx->tts_done) {
                break;
            }

            if (played_once && now_ms - last_audio_ms > TTS_IDLE_TIMEOUT_MS) {
                ESP_LOGW(TAG, "TTS idle timeout");
                break;
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

    if (ctx->play_tts_audio &&
        !(ctx->suppress_tts_for_command && ctx->is_command)) {
        audio_es8311_play_silence_ms(120);
    }
}

/* ============================================================
 * Session 管理
 * ============================================================ */
// doubao_session_open：创建并初始化一套完整豆包语音云端会话，完成从资源申请 → WebSocket 创建 → 鉴权头组装 → 网络连接 → 握手协议交互全流程，最终返回可用的长连接会话。
static esp_err_t doubao_session_open(doubao_session_t *s)
{
    if (s == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(s, 0, sizeof(*s));

    if (doubao_str_invalid(DOUBAO_APP_ID) ||
        doubao_str_invalid(DOUBAO_ACCESS_KEY) ||
        doubao_str_invalid(DOUBAO_APP_KEY)) {
        ESP_LOGE(TAG, "Please configure DOUBAO_APP_ID / ACCESS_KEY / APP_KEY");
        return ESP_FAIL;
    }

    s->ctx.event_group = xEventGroupCreate();
    if (s->ctx.event_group == NULL) {
        ESP_LOGE(TAG, "event group create failed");
        return ESP_ERR_NO_MEM;
    }

    doubao_make_uuid(s->ctx.session_id);

    int header_len = snprintf(
        s->headers,
        sizeof(s->headers),
        "X-Api-App-ID: %s\r\n"
        "X-Api-Access-Key: %s\r\n"
        "X-Api-Resource-Id: %s\r\n"
        "X-Api-App-Key: %s\r\n"
        "X-Api-Connect-Id: %s\r\n",
        DOUBAO_APP_ID,
        DOUBAO_ACCESS_KEY,
        DOUBAO_RESOURCE_ID,
        DOUBAO_APP_KEY,
        DOUBAO_CONNECT_ID
    );

    if (header_len <= 0 || header_len >= sizeof(s->headers)) {
        ESP_LOGE(TAG, "headers truncated");
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
        ESP_LOGE(TAG, "esp_websocket_client_init failed");
        vEventGroupDelete(s->ctx.event_group);
        memset(s, 0, sizeof(*s));
        return ESP_FAIL;
    }

    esp_websocket_register_events(
        s->ws,
        WEBSOCKET_EVENT_ANY,
        doubao_ws_event_handler,
        &s->ctx
    );

    esp_err_t ret = esp_websocket_client_start(s->ws);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "websocket start failed: %s", esp_err_to_name(ret));
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
        ESP_LOGE(TAG, "websocket connect timeout/fail");
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

    ESP_LOGI(TAG, "persistent connection ready");

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

    free(s->ctx.rx_buf);
    s->ctx.rx_buf = NULL;

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
//在已存在的 WebSocket 链路上，创建单次语音交互会话（一问一答，每次说话都要重新跑一遍）。
static esp_err_t doubao_start_one_session(doubao_session_t *s)
{
    if (s == NULL || s->ws == NULL || !s->active) {
        return ESP_ERR_INVALID_ARG;
    }

    doubao_ctx_t *ctx = &s->ctx;

    doubao_make_uuid(ctx->session_id);

    ctx->session_started = false;
    ctx->tts_done = false;
    ctx->failed = false;
    ctx->asr_text[0] = '\0';
    ctx->chat_text[0] = '\0';
    ctx->tts_bytes_rx = 0;
    ctx->cmd_executed = false;
    ctx->is_command = false;

    xEventGroupClearBits(
        ctx->event_group,
        DB_BIT_SESSION_STARTED |
        DB_BIT_TTS_DONE |
        DB_BIT_FAILED |
        DB_BIT_WS_DISCONNECTED
    );

    char *json = doubao_build_start_session_json();
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = doubao_send_json_event(
        s->ws,
        DB_EVENT_START_SESSION,
        ctx->session_id,
        json
    );

    free(json);

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
        ESP_LOGE(TAG, "StartSession failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "session started: %s", ctx->session_id);

    return ESP_OK;
}

/* ============================================================
 * 单轮语音
 * ============================================================ */
// 一轮完整语音交互主入口函数
// 复用已经建好的 WebSocket 长连接，一次性走完：
// 重置状态 → 创建单次业务会话 → 上传整段麦克风 PCM 语音 → 通知云端语音上传完毕 → 接收并播放 AI 语音回答 → 提取识别文字、判断小车指令 → 关闭本次会话。
static esp_err_t doubao_run_one_turn_reuse(doubao_session_t *s,
                                           uint8_t *pcm,
                                           uint32_t pcm_len,
                                           char *asr_out,
                                           size_t asr_out_size,
                                           bool enable_command,
                                           bool play_tts_audio,
                                           bool *is_cmd_out)
{
    if (asr_out != NULL && asr_out_size > 0) {
        asr_out[0] = '\0';
    }

    if (is_cmd_out != NULL) {
        *is_cmd_out = false;
    }

    if (s == NULL || s->ws == NULL || !s->active ||
        pcm == NULL || pcm_len == 0) {
        free(pcm);
        return ESP_ERR_INVALID_ARG;
    }

    doubao_ctx_t *ctx = &s->ctx;

    ctx->tts_done = false;
    ctx->failed = false;
    ctx->asr_text[0] = '\0';
    ctx->chat_text[0] = '\0';
    ctx->tts_bytes_rx = 0;
    ctx->cmd_executed = false;
    ctx->is_command = false;
    ctx->enable_command = enable_command;
    ctx->play_tts_audio = play_tts_audio;
    ctx->suppress_tts_for_command = DOUBAO_SUPPRESS_TTS_FOR_COMMAND ? true : false;

    if (ctx->rx_buf != NULL) {
        free(ctx->rx_buf);
        ctx->rx_buf = NULL;
        ctx->rx_len = 0;
        ctx->rx_expected = 0;
    }

    if (ctx->tts_ringbuf != NULL) {
        vRingbufferDelete(ctx->tts_ringbuf);
        ctx->tts_ringbuf = NULL;
    }

    if (play_tts_audio) {
        ctx->tts_ringbuf = xRingbufferCreate(TTS_RINGBUF_BYTES, RINGBUF_TYPE_BYTEBUF);
        if (ctx->tts_ringbuf == NULL) {
            ESP_LOGE(TAG, "TTS ringbuf create failed");
            free(pcm);
            return ESP_ERR_NO_MEM;
        }
    }

    xEventGroupClearBits(
        ctx->event_group,
        DB_BIT_TTS_DONE | DB_BIT_FAILED | DB_BIT_WS_DISCONNECTED
    );

    esp_err_t ret = doubao_start_one_session(s);
    if (ret != ESP_OK) {
        free(pcm);
        return ret;
    }

    ESP_LOGI(TAG, "turn start, pcm_len=%lu", (unsigned long)pcm_len);

    uint32_t sent = 0;

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
        vTaskDelay(pdMS_TO_TICKS(DOUBAO_FRAME_MS));
    }

    free(pcm);
    pcm = NULL;

    ESP_LOGI(TAG, "audio uploaded: %lu bytes", (unsigned long)sent);

    lcd_image_post(&img_car_think);

    ret = doubao_send_json_event(s->ws, DB_EVENT_END_ASR, ctx->session_id, "{}");
    if (ret != ESP_OK) {
        return ret;
    }

    downsample_24k_to_16k_state_t ds_state = {
        .group = {0, 0, 0},
        .group_count = 0,
    };

    if (play_tts_audio) {
        lcd_image_post(&img_car_talk);
    }

    doubao_drain_tts_audio(ctx, &ds_state, TTS_TOTAL_TIMEOUT_MS);

    bool is_cmd = ctx->cmd_executed;

    if (ctx->asr_text[0] != '\0') {
        ESP_LOGI(TAG, "Final ASR: %s", ctx->asr_text);

        if (asr_out != NULL && asr_out_size > 0) {
            strncpy(asr_out, ctx->asr_text, asr_out_size - 1);
            asr_out[asr_out_size - 1] = '\0';
        }

        if (enable_command && !ctx->cmd_executed) {
            is_cmd = command_router_handle_voice_text(ctx->asr_text);
            ctx->cmd_executed = is_cmd;
            ctx->is_command = is_cmd;
        }
    }

    if (ctx->chat_text[0] != '\0') {
        ESP_LOGI(TAG, "Final Chat: %s", ctx->chat_text);
    }

    if (is_cmd_out != NULL) {
        *is_cmd_out = is_cmd;
    }

    doubao_send_json_event(s->ws, DB_EVENT_FINISH_SESSION, ctx->session_id, "{}");
    vTaskDelay(pdMS_TO_TICKS(100));
    ctx->session_started = false;

    if (ctx->tts_ringbuf != NULL) {
        vRingbufferDelete(ctx->tts_ringbuf);
        ctx->tts_ringbuf = NULL;
    }

    EventBits_t bits = xEventGroupGetBits(ctx->event_group);
    if ((bits & DB_BIT_FAILED) || (bits & DB_BIT_WS_DISCONNECTED)) {
        ESP_LOGE(TAG, "turn failed/disconnected");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* ============================================================
 * 主语音任务
 * ============================================================ */
// FreeRTOS 独立任务，全程负责唤醒词监听 + 人机对话交互，串联前面所有底层函数：
// doubao_session_open / doubao_start_one_session / doubao_run_one_turn_reuse 都在本任务内部调用。
// 程序逻辑分两大模式：待机唤醒模式、活跃对话模式，靠 interaction_active 布尔变量切换。
static void voice_cloud_task(void *arg)
{
    (void)arg;

    doubao_session_t session;
    memset(&session, 0, sizeof(session));

    bool interaction_active = false;

    while (!s_doubao_stop_req) {
        if (!session.active) {
            // lcd_status_post("CLOUD");

            esp_err_t open_ret = doubao_session_open(&session);
            if (open_ret != ESP_OK) {
                ESP_LOGE(TAG,
                         "session open failed: %s",
                         esp_err_to_name(open_ret));
                lcd_status_post("ERROR");
                doubao_session_close(&session);
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            interaction_active = false;
            // lcd_status_post("STANDBY");
        }

        if (!interaction_active) {
            uint8_t *wake_pcm = NULL;
            uint32_t wake_len = 0;
            char wake_asr[256] = {0};

            lcd_image_post(&img_car_listening);

            esp_err_t rec_ret = record_mic_pcm_vad(
                LELE_WAKE_RECORD_MS,
                &wake_pcm,
                &wake_len
            );

            if (rec_ret != ESP_OK || wake_pcm == NULL || wake_len == 0) {
                free(wake_pcm);
                vTaskDelay(pdMS_TO_TICKS(LELE_STANDBY_DELAY_MS));
                continue;
            }

            lcd_image_post(&img_car_think);

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

            ESP_LOGI(TAG, "Wake ASR: %s", wake_asr);

            if (text_has_wake_word(wake_asr)) {
                interaction_active = true;
                lcd_image_post(&img_car_think);
                play_local_pcm_file(LELE_WAKE_ACK_PCM);
                lcd_status_post("ACTIVE");
                vTaskDelay(pdMS_TO_TICKS(200));
            } else {
                lcd_status_post("STANDBY");
            }

            continue;
        }

        uint8_t *mic_pcm = NULL;
        uint32_t mic_len = 0;
        char asr_text[256] = {0};
        bool is_cmd = false;

        lcd_image_post(&img_car_listening);

        esp_err_t rec_ret = record_mic_pcm_vad(
            LELE_ACTIVE_RECORD_MS,
            &mic_pcm,
            &mic_len
        );

        if (rec_ret != ESP_OK || mic_pcm == NULL || mic_len == 0) {
            free(mic_pcm);
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        lcd_image_post(&img_car_think);
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

        ESP_LOGI(TAG,
                 "active turn ret=%s asr=%s is_cmd=%d",
                 esp_err_to_name(turn_ret),
                 asr_text,
                 is_cmd ? 1 : 0);

        if (turn_ret != ESP_OK) {
            lcd_status_post("ERROR");
            doubao_session_close(&session);
            interaction_active = false;
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }

        if (text_has_sleep_word(asr_text)) {
            lcd_status_post("SLEEP");
            play_local_pcm_file(LELE_SLEEP_ACK_PCM);
            command_router_handle_voice_text("停止");
            interaction_active = false;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        lcd_status_post(is_cmd ? "CMD" : "READY");
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    doubao_session_close(&session);

    s_doubao_started = false;
    s_doubao_task_handle = NULL;

    lcd_status_post("VOICE OFF");

    vTaskDelete(NULL);
}

/* ============================================================
 * 对外 API
 * ============================================================ */
//语音云端模块对外统一启动入口
esp_err_t doubao_client_start(void)
{
    if (s_doubao_started) {
        ESP_LOGW(TAG, "doubao client already started");
        return ESP_OK;
    }

    s_doubao_stop_req = false;

    BaseType_t ret = xTaskCreate(
        voice_cloud_task,
        "voice_cloud_task",
        16384,
        NULL,
        5,
        &s_doubao_task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "voice_cloud_task create failed");
        s_doubao_task_handle = NULL;
        return ESP_FAIL;
    }

    s_doubao_started = true;

    ESP_LOGI(TAG, "doubao client started");

    return ESP_OK;
}

bool doubao_client_is_started(void)
{
    return s_doubao_started;
}

void doubao_client_stop(void)
{
    if (!s_doubao_started) {
        return;
    }

    s_doubao_stop_req = true;
}