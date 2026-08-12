#pragma once

#include <stdbool.h>
#include "esp_err.h"


#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"

#include "cJSON.h"

#include "app_config.h"
#include "audio_es8311.h"
#include "lcd_driver.h"
#include "command.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动豆包语音交互任务
 *
 * 前置条件：
 * 1. WiFi STA 已连接互联网
 * 2. SNTP 时间已同步，避免 TLS 证书校验失败
 * 3. ES8311 音频模块已经初始化
 * 4. SPIFFS 已挂载，本地 awake.pcm / leave.pcm 可选存在
 */
esp_err_t doubao_client_start(void);

/**
 * @brief 查询豆包语音任务是否已经
 */
esp_err_t doubao_client_start(void);

/**
 * @brief 查询豆包语音任务是否已经启动
 */
bool doubao_client_is_started(void);

/**
 * @brief 停止豆包语音任务
 *
 * 当前实现为请求停止，任务会在下一轮循环中退出。
 */
void doubao_client_stop(void);

#ifdef __cplusplus
}
#endif