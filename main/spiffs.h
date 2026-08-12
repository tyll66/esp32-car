#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化并挂载 SPIFFS
 *
 * 默认使用 app_config.h 中的：
 * SPIFFS_BASE_PATH
 * SPIFFS_LABEL
 * SPIFFS_MAX_FILES
 * SPIFFS_FORMAT_IF_MOUNT_FAILED
 */
esp_err_t spiffs_storage_init(void);

/**
 * @brief 反初始化 SPIFFS
 *
 * 会调用 esp_vfs_spiffs_unregister()
 */
esp_err_t spiffs_storage_deinit(void);

/**
 * @brief 判断 SPIFFS 是否已经挂载
 */
bool spiffs_storage_is_mounted(void);

/**
 * @brief 获取 SPIFFS 容量信息
 *
 * @param total 输出总容量
 * @param used 输出已用容量
 */
esp_err_t spiffs_storage_get_info(size_t *total, size_t *used);

/**
 * @brief 打印 SPIFFS 容量信息
 */
esp_err_t spiffs_storage_print_info(void);

/**
 * @brief 判断文件是否存在
 *
 * @param path 文件路径，例如 "/spiffs/awake.pcm"
 */
bool spiffs_storage_file_exists(const char *path);

/**
 * @brief 获取文件大小
 *
 * @param path 文件路径
 * @param size_out 输出文件大小
 */
esp_err_t spiffs_storage_get_file_size(const char *path, size_t *size_out);

/**
 * @brief 删除文件
 *
 * 如果文件不存在，返回 ESP_OK。
 */
esp_err_t spiffs_storage_remove_file(const char *path);

/**
 * @brief 主动触发 SPIFFS GC
 *
 * 适合写入大文件前调用，比如云端 TTS 临时 PCM 文件。
 *
 * @param size_to_gc 希望回收出的空间大小，单位 byte
 */
esp_err_t spiffs_storage_gc(size_t size_to_gc);

/**
 * @brief 准备豆包云端 TTS 临时缓存文件
 *
 * 做三件事：
 * 1. 删除旧的临时 PCM 文件
 * 2. 触发 SPIFFS GC
 * 3. 打印当前 SPIFFS 容量信息
 *
 * @param path 临时文件路径，例如 "/spiffs/cloud_tts_stream.pcm"
 * @param gc_size 写入前希望回收的空间大小
 */
esp_err_t spiffs_storage_prepare_temp_file(const char *path, size_t gc_size);

/**
 * @brief 检查本地提示音文件
 *
 * 用于检查：
 * /spiffs/awake.pcm
 * /spiffs/leave.pcm
 *
 * 文件不存在不会直接失败，只打印 warning。
 */
esp_err_t spiffs_storage_check_prompt_files(void);

#ifdef __cplusplus
}
#endif