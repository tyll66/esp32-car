#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"


#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"

#include "driver/uart.h"

#include "app_config.h"
#include "app_types.h"
#include "lcd_driver.h"
#include "command.h"
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化连接 STM32 的 UART
 *
 * 需要 app_config.h 中定义：
 * STM32_UART_PORT
 * STM32_UART_TX_GPIO
 * STM32_UART_RX_GPIO
 * STM32_UART_BAUD
 */
esp_err_t uart_stm32_init(void);

/**
 * @brief 反初始化 UART
 */
esp_err_t uart_stm32_deinit(void);

/**
 * @brief 判断 UART 是否已经初始化
 */
bool uart_stm32_is_inited(void);

/**
 * @brief 发送原始字节给 STM32
 *
 * @param data 数据指针
 * @param len 数据长度
 */
esp_err_t uart_stm32_send_bytes(const uint8_t *data, size_t len);

/**
 * @brief 发送字符串给 STM32
 *
 * 注意：
 * 这个函数不会自动添加换行。
 * 如果你要发命令，建议直接发 "F\n"、"S\n"。
 */
esp_err_t uart_stm32_send_line(const char *line);

/**
 * @brief 发送单字符命令给 STM32，并自动添加 '\n'
 *
 * 例如：
 * uart_stm32_send_cmd_code("F");
 * 实际发送：
 * F\n
 */
esp_err_t uart_stm32_send_cmd_code(const char *code);

/**
 * @brief 从 STM32 读取数据
 *
 * @param data 接收缓冲区
 * @param len 最大读取长度
 * @param timeout_ms 超时时间，单位 ms
 *
 * @return >=0 实际读取字节数；<0 表示错误
 */
int uart_stm32_read_bytes(uint8_t *data, size_t len, uint32_t timeout_ms);

/**
 * @brief 清空 UART 接收缓冲区
 */
esp_err_t uart_stm32_flush_input(void);

/**
 * @brief 启动 WiFi → UART 转发任务
 *
 * remote_msg_queue 里应该放 remote_msg_t。
 * remote_msg_t 定义在 app_types.h。
 */
esp_err_t uart_stm32_start_forward_task(QueueHandle_t remote_msg_queue);

/**
 * @brief 请求停止 WiFi → UART 转发任务
 */
void uart_stm32_stop_forward_task(void);

#ifdef __cplusplus
}
#endif