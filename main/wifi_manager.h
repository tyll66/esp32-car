#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"


#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"

#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "app_config.h"
#include "app_types.h"
#include "lcd_driver.h"


#include "doubao_client.h"
#include "command.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 WiFi APSTA 模式
 *
 * 功能：
 * 1. STA 连接手机热点，用于访问互联网和豆包 API
 * 2. SoftAP 开启热点，等待另一个 ESP32 连接
 * 3. 启动 UDP Server，接收另一个 ESP32 发来的数据
 *
 * @param remote_msg_queue 接收 UDP 数据后投递到这个队列
 */
esp_err_t wifi_manager_init_apsta(QueueHandle_t remote_msg_queue);

/**
 * @brief 等待 STA 连接手机热点并获取 IP
 *
 * @param timeout_ticks FreeRTOS tick 超时时间，例如 portMAX_DELAY
 */
esp_err_t wifi_manager_wait_sta_connected(TickType_t timeout_ticks);

/**
 * @brief 判断 STA 是否已连接手机热点
 */
bool wifi_manager_is_sta_connected(void);

/**
 * @brief 判断 UDP Server 是否已经启动
 */
bool wifi_manager_is_udp_server_started(void);

/**
 * @brief 获取 STA IP 字符串
 *
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 *
 * 示例输出：
 * 192.168.43.100
 */
esp_err_t wifi_manager_get_sta_ip_str(char *buf, size_t buf_size);

/**
 * @brief 获取 SoftAP IP 字符串
 *
 * 默认一般是：
 * 192.168.4.1
 */
esp_err_t wifi_manager_get_ap_ip_str(char *buf, size_t buf_size);

/**
 * @brief 停止 UDP Server
 */
void wifi_manager_stop_udp_server(void);

/**
 * @brief WiFi 反初始化
 *
 * 一般项目运行中不需要调用。
 */
esp_err_t wifi_manager_deinit(void);

#ifdef __cplusplus
}
#endif