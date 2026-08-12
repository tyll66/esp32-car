#include "audio_es8311.h"


static const char *TAG = "AUDIO_ES8311";

/*
 * 内部句柄。
 * 不建议在其他模块里直接 extern 这些变量。
 */
static i2s_chan_handle_t s_i2s_tx_chan = NULL;
static i2s_chan_handle_t s_i2s_rx_chan = NULL;
static esp_codec_dev_handle_t s_codec_dev = NULL;
static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;

static bool s_audio_inited = false;

static void audio_i2c_scan(i2c_master_bus_handle_t bus_handle);

static esp_err_t audio_es8311_raw_i2c_test(
    i2c_master_bus_handle_t bus_handle);
/*
 * 内部处理块大小。
 * 512 samples 在 16kHz 下约 32ms。
 */
#define AUDIO_ES8311_CHUNK_SAMPLES 512

static esp_err_t audio_i2c_init(void)
{
    if (s_i2c_bus_handle != NULL) {
        ESP_LOGW(TAG, "I2C already initialized");
        return ESP_OK;
    }

    i2c_master_bus_config_t i2c_bus_conf = {
        .i2c_port = ES8311_I2C_PORT,
        .sda_io_num = ES8311_I2C_SDA,
        .scl_io_num = ES8311_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_RETURN_ON_ERROR(
        i2c_new_master_bus(&i2c_bus_conf, &s_i2c_bus_handle),
        TAG,
        "i2c_new_master_bus failed"
    );

    ESP_LOGI(TAG,
             "I2C master bus init done: SDA GPIO%d, SCL GPIO%d",
             ES8311_I2C_SDA,
             ES8311_I2C_SCL);

    return ESP_OK;
}

static esp_err_t audio_i2s_init(void)
{
    if (s_i2s_tx_chan != NULL || s_i2s_rx_chan != NULL) {
        ESP_LOGW(TAG, "I2S already initialized");
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(
            AUDIO_I2S_PORT,
            I2S_ROLE_MASTER
        );

    ESP_RETURN_ON_ERROR(
        i2s_new_channel(
            &chan_cfg,
            &s_i2s_tx_chan,
            &s_i2s_rx_chan
        ),
        TAG,
        "i2s_new_channel failed"
    );

    i2s_std_config_t std_cfg = {
        .clk_cfg =
            I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),

        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_16BIT,
                I2S_SLOT_MODE_STEREO
            ),

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

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(
            s_i2s_tx_chan,
            &std_cfg
        ),
        TAG,
        "i2s_channel_init_std_mode TX failed"
    );

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(
            s_i2s_rx_chan,
            &std_cfg
        ),
        TAG,
        "i2s_channel_init_std_mode RX failed"
    );

    /*
     * 按照之前可行版本：
     * 初始化完成后立即启用 TX 和 RX。
     */
    ESP_RETURN_ON_ERROR(
        i2s_channel_enable(s_i2s_tx_chan),
        TAG,
        "i2s_channel_enable TX failed"
    );

    ESP_RETURN_ON_ERROR(
        i2s_channel_enable(s_i2s_rx_chan),
        TAG,
        "i2s_channel_enable RX failed"
    );

    ESP_LOGI(TAG, "I2S init done");

    ESP_LOGI(
        TAG,
        "MCLK GPIO%d, BCLK GPIO%d, LRCK GPIO%d, DOUT GPIO%d, DIN GPIO%d",
        AUDIO_MCLK_GPIO,
        AUDIO_BCLK_GPIO,
        AUDIO_LRCK_GPIO,
        AUDIO_DOUT_GPIO,
        AUDIO_DIN_GPIO
    );

    return ESP_OK;
}

esp_err_t audio_es8311_init(void)
{
    if (s_audio_inited) {
        ESP_LOGW(TAG, "audio_es8311 already initialized");
        return ESP_OK;
    }

    /*
     * 按照之前可行版本的顺序：
     * 1. I2C
     * 2. I2S
     * 3. 启用 I2S
     * 4. 创建 codec
     */
    ESP_RETURN_ON_ERROR(
        audio_i2c_init(),
        TAG,
        "audio_i2c_init failed"
    );
    ESP_RETURN_ON_ERROR(
    audio_es8311_raw_i2c_test(s_i2c_bus_handle),
    TAG,
    "ES8311 raw I2C test failed"
    );

    ESP_RETURN_ON_ERROR(
        audio_i2s_init(),
        TAG,
        "audio_i2s_init failed"
    );

    /*
     * I2S 数据接口。
     */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = AUDIO_I2S_PORT,
        .rx_handle = s_i2s_rx_chan,
        .tx_handle = s_i2s_tx_chan,
    };

    const audio_codec_data_if_t *data_if =
        audio_codec_new_i2s_data(&i2s_cfg);

    ESP_RETURN_ON_FALSE(
        data_if != NULL,
        ESP_FAIL,
        TAG,
        "audio_codec_new_i2s_data failed"
    );

    /*
     * I2C 控制接口。
     * 按照旧版本，不填写 .port。
     */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .bus_handle = s_i2c_bus_handle,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
    };

    const audio_codec_ctrl_if_t *ctrl_if =
        audio_codec_new_i2c_ctrl(&i2c_cfg);

    ESP_RETURN_ON_FALSE(
        ctrl_if != NULL,
        ESP_FAIL,
        TAG,
        "audio_codec_new_i2c_ctrl failed"
    );

    /*
     * GPIO 控制接口。
     */
    const audio_codec_gpio_if_t *gpio_if =
        audio_codec_new_gpio();

    ESP_RETURN_ON_FALSE(
        gpio_if != NULL,
        ESP_FAIL,
        TAG,
        "audio_codec_new_gpio failed"
    );

    /*
     * 完全按照旧版本的 ES8311 最小配置。
     * 不额外填写 digital_mic、invert、mclk_div 等字段。
     */
    es8311_codec_cfg_t es8311_cfg = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .pa_pin = PA_ENABLE_GPIO,
        .use_mclk = true,
    };

    const audio_codec_if_t *codec_if =
        es8311_codec_new(&es8311_cfg);

    ESP_RETURN_ON_FALSE(
        codec_if != NULL,
        ESP_FAIL,
        TAG,
        "es8311_codec_new failed"
    );

    /*
     * 创建统一 codec 设备。
     */
    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec_if,
        .data_if = data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
    };

    s_codec_dev = esp_codec_dev_new(&dev_cfg);

    ESP_RETURN_ON_FALSE(
        s_codec_dev != NULL,
        ESP_FAIL,
        TAG,
        "esp_codec_dev_new failed"
    );

    /*
     * 音频格式：16kHz、16bit、双声道。
     */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .channel = AUDIO_CHANNELS,
        .bits_per_sample = AUDIO_BITS,
    };

    ESP_RETURN_ON_ERROR(
        esp_codec_dev_open(s_codec_dev, &fs),
        TAG,
        "esp_codec_dev_open failed"
    );

    ESP_RETURN_ON_ERROR(
        esp_codec_dev_set_out_vol(s_codec_dev, 80.0),
        TAG,
        "esp_codec_dev_set_out_vol failed"
    );

    ESP_RETURN_ON_ERROR(
        esp_codec_dev_set_in_gain(s_codec_dev, 30.0),
        TAG,
        "esp_codec_dev_set_in_gain failed"
    );

    s_audio_inited = true;

    ESP_LOGI(
        TAG,
        "ES8311 init done: %d Hz, %d bit, %d channel",
        AUDIO_SAMPLE_RATE,
        AUDIO_BITS,
        AUDIO_CHANNELS
    );

    return ESP_OK;
}

bool audio_es8311_is_inited(void)
{
    return s_audio_inited;
}

esp_codec_dev_handle_t audio_es8311_get_codec(void)
{
    return s_codec_dev;
}

esp_err_t audio_es8311_set_out_vol(float volume)
{
    ESP_RETURN_ON_FALSE(
        s_codec_dev != NULL,
        ESP_ERR_INVALID_STATE,
        TAG,
        "codec not initialized"
    );

    return esp_codec_dev_set_out_vol(s_codec_dev, volume);
}

esp_err_t audio_es8311_set_in_gain(float gain)
{
    ESP_RETURN_ON_FALSE(
        s_codec_dev != NULL,
        ESP_ERR_INVALID_STATE,
        TAG,
        "codec not initialized"
    );

    return esp_codec_dev_set_in_gain(s_codec_dev, gain);
}

esp_err_t audio_es8311_write(const void *data, size_t bytes)
{
    ESP_RETURN_ON_FALSE(
        s_codec_dev != NULL,
        ESP_ERR_INVALID_STATE,
        TAG,
        "codec not initialized"
    );

    ESP_RETURN_ON_FALSE(
        data != NULL && bytes > 0,
        ESP_ERR_INVALID_ARG,
        TAG,
        "invalid write buffer"
    );

    return esp_codec_dev_write(s_codec_dev, (void *)data, bytes);
}

esp_err_t audio_es8311_read(void *data, size_t bytes)
{
    ESP_RETURN_ON_FALSE(
        s_codec_dev != NULL,
        ESP_ERR_INVALID_STATE,
        TAG,
        "codec not initialized"
    );

    ESP_RETURN_ON_FALSE(
        data != NULL && bytes > 0,
        ESP_ERR_INVALID_ARG,
        TAG,
        "invalid read buffer"
    );

    return esp_codec_dev_read(s_codec_dev, data, bytes);
}

esp_err_t audio_es8311_write_stereo_pcm16(const int16_t *stereo_pcm,
                                          size_t samples_per_channel)
{
    ESP_RETURN_ON_FALSE(
        stereo_pcm != NULL && samples_per_channel > 0,
        ESP_ERR_INVALID_ARG,
        TAG,
        "invalid stereo pcm"
    );

    /*
     * stereo interleaved:
     * 每个采样点包含 L + R 两个 int16_t。
     */
    size_t bytes = samples_per_channel * 2 * sizeof(int16_t);

    return audio_es8311_write(stereo_pcm, bytes);
}

esp_err_t audio_es8311_read_stereo_pcm16(int16_t *stereo_pcm,
                                         size_t samples_per_channel)
{
    ESP_RETURN_ON_FALSE(
        stereo_pcm != NULL && samples_per_channel > 0,
        ESP_ERR_INVALID_ARG,
        TAG,
        "invalid stereo pcm"
    );

    size_t bytes = samples_per_channel * 2 * sizeof(int16_t);

    return audio_es8311_read(stereo_pcm, bytes);
}

//单声道转立体声函数
void audio_es8311_mono_to_stereo(const int16_t *mono,
                                 int16_t *stereo,
                                 size_t samples)
{
    if (mono == NULL || stereo == NULL) {
        return;
    }

    for (size_t i = 0; i < samples; i++) {
        int16_t s = mono[i];

        stereo[i * 2 + 0] = s;
        stereo[i * 2 + 1] = s;
    }
}

void audio_es8311_stereo_to_mono_left(const int16_t *stereo,
                                      int16_t *mono,
                                      size_t samples)
{
    if (stereo == NULL || mono == NULL) {
        return;
    }

    for (size_t i = 0; i < samples; i++) {
        /*
         * 当前 ES8311 I2S 配置为 stereo。
         * stereo[i * 2 + 0] 是左声道。
         * stereo[i * 2 + 1] 是右声道。
         */
        mono[i] = stereo[i * 2 + 0];
    }
}

//播放
esp_err_t audio_es8311_write_mono_pcm16(const int16_t *mono_pcm,
                                        size_t samples)
{
    ESP_RETURN_ON_FALSE(
        mono_pcm != NULL && samples > 0,
        ESP_ERR_INVALID_ARG,
        TAG,
        "invalid mono pcm"
    );

    int16_t *stereo_buf = heap_caps_malloc(
        AUDIO_ES8311_CHUNK_SAMPLES * 2 * sizeof(int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );

    if (stereo_buf == NULL) {
        stereo_buf = malloc(AUDIO_ES8311_CHUNK_SAMPLES * 2 * sizeof(int16_t));
    }

    ESP_RETURN_ON_FALSE(
        stereo_buf != NULL,
        ESP_ERR_NO_MEM,
        TAG,
        "stereo buffer malloc failed"
    );

    size_t pos = 0;
    esp_err_t ret = ESP_OK;

    while (pos < samples) {
        size_t n = samples - pos;

        if (n > AUDIO_ES8311_CHUNK_SAMPLES) {
            n = AUDIO_ES8311_CHUNK_SAMPLES;
        }

        audio_es8311_mono_to_stereo(&mono_pcm[pos], stereo_buf, n);

        ret = audio_es8311_write_stereo_pcm16(stereo_buf, n);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "write mono pcm failed: %s", esp_err_to_name(ret));
            break;
        }

        pos += n;
    }

    free(stereo_buf);

    return ret;
}

esp_err_t audio_es8311_read_mono_left_pcm16(int16_t *mono_pcm,
                                            size_t samples)
{
    ESP_RETURN_ON_FALSE(
        mono_pcm != NULL && samples > 0,
        ESP_ERR_INVALID_ARG,
        TAG,
        "invalid mono pcm"
    );

    int16_t *stereo_buf = heap_caps_malloc(
        AUDIO_ES8311_CHUNK_SAMPLES * 2 * sizeof(int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );

    if (stereo_buf == NULL) {
        stereo_buf = malloc(AUDIO_ES8311_CHUNK_SAMPLES * 2 * sizeof(int16_t));
    }

    ESP_RETURN_ON_FALSE(
        stereo_buf != NULL,
        ESP_ERR_NO_MEM,
        TAG,
        "stereo buffer malloc failed"
    );

    size_t pos = 0;
    esp_err_t ret = ESP_OK;

    while (pos < samples) {
        size_t n = samples - pos;

        if (n > AUDIO_ES8311_CHUNK_SAMPLES) {
            n = AUDIO_ES8311_CHUNK_SAMPLES;
        }

        ret = audio_es8311_read_stereo_pcm16(stereo_buf, n);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "read stereo pcm failed: %s", esp_err_to_name(ret));
            break;
        }

        audio_es8311_stereo_to_mono_left(stereo_buf, &mono_pcm[pos], n);

        pos += n;
    }

    free(stereo_buf);

    return ret;
}

esp_err_t audio_es8311_play_silence_ms(uint32_t ms)
{
    if (ms == 0) {
        return ESP_OK;
    }

    size_t total_samples = (size_t)AUDIO_SAMPLE_RATE * ms / 1000;
    if (total_samples == 0) {
        total_samples = 1;
    }

    int16_t *silence_stereo = heap_caps_calloc(
        AUDIO_ES8311_CHUNK_SAMPLES * 2,
        sizeof(int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );

    if (silence_stereo == NULL) {
        silence_stereo = calloc(AUDIO_ES8311_CHUNK_SAMPLES * 2, sizeof(int16_t));
    }

    ESP_RETURN_ON_FALSE(
        silence_stereo != NULL,
        ESP_ERR_NO_MEM,
        TAG,
        "silence buffer malloc failed"
    );

    size_t pos = 0;
    esp_err_t ret = ESP_OK;

    while (pos < total_samples) {
        size_t n = total_samples - pos;

        if (n > AUDIO_ES8311_CHUNK_SAMPLES) {
            n = AUDIO_ES8311_CHUNK_SAMPLES;
        }

        /*
         * silence_stereo 已经全 0。
         */
        ret = audio_es8311_write_stereo_pcm16(silence_stereo, n);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "write silence failed: %s", esp_err_to_name(ret));
            break;
        }

        pos += n;
    }

    free(silence_stereo);

    return ret;
}

static void audio_i2c_scan(i2c_master_bus_handle_t bus_handle)
{
    ESP_LOGI(TAG, "I2C scan start");

    int found = 0;

    for (uint8_t addr = 0x03; addr < 0x78; addr++) {
        esp_err_t ret = i2c_master_probe(bus_handle, addr, 50);

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02X", addr);
            found++;
        }
    }

    if (found == 0) {
        ESP_LOGW(TAG, "No I2C device found");
    }

    ESP_LOGI(TAG, "I2C scan done, found=%d", found);
}

static esp_err_t audio_es8311_raw_i2c_test(
    i2c_master_bus_handle_t bus_handle)
{
    ESP_LOGI(TAG, "ES8311 raw I2C test start");

    /*
     * 新版ESP-IDF I2C驱动使用7位地址。
     * ES8311扫描地址是0x18。
     */
    esp_err_t ret = i2c_master_probe(
        bus_handle,
        0x18,
        1000
    );

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "ES8311 probe 0x18 failed: %s",
            esp_err_to_name(ret)
        );
        return ret;
    }

    ESP_LOGI(TAG, "ES8311 probe 0x18 OK");

    i2c_master_dev_handle_t dev_handle = NULL;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x18,
        .scl_speed_hz = 100000,
    };

    ret = i2c_master_bus_add_device(
        bus_handle,
        &dev_cfg,
        &dev_handle
    );

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "add raw ES8311 device failed: %s",
            esp_err_to_name(ret)
        );
        return ret;
    }

    /*
     * 尝试读取寄存器0x00。
     */
    uint8_t reg_addr = 0x00;
    uint8_t reg_value = 0x00;

    ret = i2c_master_transmit_receive(
        dev_handle,
        &reg_addr,
        1,
        &reg_value,
        1,
        1000
    );

    if (ret == ESP_OK) {
        ESP_LOGI(
            TAG,
            "ES8311 raw read OK: reg[0x00] = 0x%02X",
            reg_value
        );
    } else {
        ESP_LOGE(
            TAG,
            "ES8311 raw read failed: %s",
            esp_err_to_name(ret)
        );
    }

    i2c_master_bus_rm_device(dev_handle);

    return ret;
}