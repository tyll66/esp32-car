#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"


#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_netif_sntp.h"

#include "freertos/FreeRTOS.h"

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 SNTP 时间同步
 *
 * 前置条件：
 * 1. WiFi STA 已经连接手机热点
 * 2. 已经成功获取 IP
 *
 * 主要作用：
 * 1. 设置时区
 * 2. 初始化 SNTP
 * 3. 等待网络时间同步
 * 4. 检查系统时间是否有效
 */
esp_err_t time_sync_init(void);

/**
 * @brief 反初始化 SNTP
 *
 * 一般项目运行期间不需要调用。
 */
esp_err_t time_sync_deinit(void);

/**
 * @brief 等待 SNTP 时间同步
 *
 * @param timeout_ms 超时时间，单位 ms。传 0 使用默认超时时间。
 */
esp_err_t time_sync_wait(uint32_t timeout_ms);

/**
 * @brief 判断系统时间是否有效
 *
 * 默认认为 Unix 秒大于 2024-01-01 00:00:00 UTC 即为有效。
 */
bool time_sync_is_time_valid(void);

/**
 * @brief 判断 time_sync_init() 是否已经调用过
 */
bool time_sync_is_inited(void);

/**
 * @brief 判断时间是否已经同步成功
 */
bool time_sync_is_synced(void);

/**
 * @brief 获取当前 Unix 秒
 */
int64_t time_sync_now_sec(void);

/**
 * @brief 获取当前 Unix 毫秒
 */
int64_t time_sync_now_ms(void);

/**
 * @brief 打印当前时间
 */
void time_sync_print_now(void);

/**
 * @brief 格式化当前本地时间
 *
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 *
 * 输出示例：
 * 2026-07-08 16:35:20
 */
esp_err_t time_sync_format_now(char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif