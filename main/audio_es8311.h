#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_codec_dev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 ES8311 音频模块
 *
 * 初始化内容：
 * 1. I2C master bus
 * 2. I2S TX/RX channel
 * 3. esp_codec_dev data interface
 * 4. esp_codec_dev control interface
 * 5. ES8311 codec
 * 6. 打开 codec，设置采样率、位宽、通道数、音量、输入增益
 */
esp_err_t audio_es8311_init(void);

/**
 * @brief 判断 ES8311 音频模块是否已经初始化
 */
bool audio_es8311_is_inited(void);

/**
 * @brief 获取底层 esp_codec_dev 句柄
 *
 * 一般不建议其他模块直接操作它，除非你确实需要调用 esp_codec_dev 原生 API。
 */
esp_codec_dev_handle_t audio_es8311_get_codec(void);

/**
 * @brief 设置扬声器输出音量
 *
 * @param volume 音量，通常 0.0 ~ 100.0
 */
esp_err_t audio_es8311_set_out_vol(float volume);

/**
 * @brief 设置麦克风输入增益
 *
 * @param gain 增益，具体范围由 codec 驱动决定。你原工程使用 30.0。
 */
esp_err_t audio_es8311_set_in_gain(float gain);

/**
 * @brief 写原始 PCM 数据到 ES8311
 *
 * 注意：
 * 当前底层配置为 stereo / 16bit / 16kHz。
 * 如果你手里是 mono PCM，优先用 audio_es8311_write_mono_pcm16()。
 */
esp_err_t audio_es8311_write(const void *data, size_t bytes);

/**
 * @brief 从 ES8311 读取原始 PCM 数据
 *
 * 注意：
 * 当前底层配置为 stereo / 16bit / 16kHz。
 * 如果你想直接拿 mono PCM，优先用 audio_es8311_read_mono_left_pcm16()。
 */
esp_err_t audio_es8311_read(void *data, size_t bytes);

/**
 * @brief 播放 stereo PCM
 *
 * @param stereo_pcm stereo interleaved PCM，格式：L R L R ...
 * @param samples_per_channel 每个声道的采样点数
 */
esp_err_t audio_es8311_write_stereo_pcm16(const int16_t *stereo_pcm,
                                          size_t samples_per_channel);

/**
 * @brief 读取 stereo PCM
 *
 * @param stereo_pcm stereo interleaved PCM，格式：L R L R ...
 * @param samples_per_channel 每个声道要读取的采样点数
 */
esp_err_t audio_es8311_read_stereo_pcm16(int16_t *stereo_pcm,
                                         size_t samples_per_channel);

/**
 * @brief 播放 mono PCM
 *
 * 会自动把 mono 转成 stereo：
 * mono[i] -> stereo[2i], stereo[2i + 1]
 *
 * @param mono_pcm mono PCM
 * @param samples mono 采样点数
 */
esp_err_t audio_es8311_write_mono_pcm16(const int16_t *mono_pcm,
                                        size_t samples);

/**
 * @brief 读取 mono PCM
 *
 * 实际从 ES8311 读取 stereo，然后取左声道作为 mono。
 *
 * @param mono_pcm 输出 mono PCM buffer
 * @param samples 需要读取的 mono 采样点数
 */
esp_err_t audio_es8311_read_mono_left_pcm16(int16_t *mono_pcm,
                                            size_t samples);

/**
 * @brief 播放一段静音
 *
 * 常用于 TTS 播放前后，避免喇叭残留爆音。
 */
esp_err_t audio_es8311_play_silence_ms(uint32_t ms);

/**
 * @brief mono 转 stereo
 */
void audio_es8311_mono_to_stereo(const int16_t *mono,
                                 int16_t *stereo,
                                 size_t samples);

/**
 * @brief stereo 取左声道转 mono
 */
void audio_es8311_stereo_to_mono_left(const int16_t *stereo,
                                      int16_t *mono,
                                      size_t samples);

#ifdef __cplusplus
}
#endif