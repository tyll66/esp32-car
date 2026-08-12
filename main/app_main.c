#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "app_types.h"

#include "lcd_driver.h"
#include "audio_es8311.h"
#include "spiffs.h"
#include "uart.h"
#include "wifi_manager.h"
#include "time_sync.h"
#include "doubao_client.h"
#include "command.h"


LV_IMAGE_DECLARE(img_car_wifi);

//1.增加动作：举手，放下，跳舞..
//2.增加逻辑：从机连接到该主机后停止豆包任务
static const char *TAG = "APP_MAIN";

/*
 * 另一个 ESP32 通过 WiFi UDP 发来的遥控数据，
 * 会先进入这个队列，然后由 uart.c 的转发任务发给 STM32。
 */
static QueueHandle_t g_remote_msg_queue = NULL;

#ifndef REMOTE_MSG_QUEUE_LEN
#define REMOTE_MSG_QUEUE_LEN 2
#endif
//nvs初始化，断电存储
static esp_err_t app_nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS invalid, erase and re-init");

        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    return ret;
}

static esp_err_t app_create_queues(void)
{
    if (g_remote_msg_queue != NULL) {
        return ESP_OK;
    }

    g_remote_msg_queue = xQueueCreate(
        REMOTE_MSG_QUEUE_LEN,
        sizeof(remote_msg_t)
    );

    if (g_remote_msg_queue == NULL) {
        ESP_LOGE(TAG, "remote msg queue create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "remote msg queue created, len=%d, item_size=%u",
             REMOTE_MSG_QUEUE_LEN,
             (unsigned int)sizeof(remote_msg_t));

    return ESP_OK;
}

static void app_print_ip_info(void)
{
    char sta_ip[32] = {0};
    char ap_ip[32] = {0};

    if (wifi_manager_get_sta_ip_str(sta_ip, sizeof(sta_ip)) == ESP_OK) {
        ESP_LOGI(TAG, "STA IP: %s", sta_ip);
    } else {
        ESP_LOGW(TAG, "get STA IP failed");
    }

    if (wifi_manager_get_ap_ip_str(ap_ip, sizeof(ap_ip)) == ESP_OK) {
        ESP_LOGI(TAG, "AP IP: %s", ap_ip);
    } else {
        ESP_LOGW(TAG, "get AP IP failed");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Desk Top Car Master Start");
    ESP_LOGI(TAG, "ESP32-S3: Doubao + ES8311 + LVGL + APSTA + UART");
    ESP_LOGI(TAG, "========================================");

    /*
     * 1. NVS 初始化
     *
     * WiFi 依赖 NVS。
     * 后续如果有 NVS 保存配置，也统一依赖这里。
     */
    ESP_LOGI(TAG, "Step 1: NVS init");
    ESP_ERROR_CHECK(app_nvs_init());

    /*
     * 2. 创建队列
     *
     * WiFi UDP 收到另一个 ESP32 的数据后写入该队列；
     * UART 转发任务从该队列取数据并发给 STM32。
     */
    ESP_LOGI(TAG, "Step 2: Create queues");
    ESP_ERROR_CHECK(app_create_queues());

    /*
     * 3. LCD + LVGL 初始化
     *
     * 后续其他模块可以通过 lcd_status_post() 显示状态。
     */
    ESP_LOGI(TAG, "Step 3: LCD init");
    ESP_ERROR_CHECK(lcd_driver_init());
    ESP_ERROR_CHECK(lcd_status_start());
    lcd_status_post("BOOT");
    

    /*
     * 4. ES8311 音频初始化
     *
     * 豆包语音模块依赖：
     * - audio_es8311_read()
     * - audio_es8311_write()
     * - audio_es8311_write_mono_pcm16()
     */
    ESP_LOGI(TAG, "Step 4: ES8311 audio init");
    lcd_status_post("AUDIO");
    ESP_ERROR_CHECK(audio_es8311_init());

    /*
     * 可以额外补一小段静音，减少启动后功放残留噪声。
     */
    audio_es8311_play_silence_ms(100);

    /*
     * 5. SPIFFS 初始化
     *
     * 豆包模块需要：
     * - /spiffs/awake.pcm
     * - /spiffs/leave.pcm
     * - 可选云端 TTS 临时缓存文件
     */
    ESP_LOGI(TAG, "Step 5: SPIFFS init");
    lcd_status_post("SPIFFS");
    ESP_ERROR_CHECK(spiffs_storage_init());

    /*
     * 6. UART 初始化
     *
     * ESP32-S3 通过 UART 把命令发给 STM32 下位机。
     */
    ESP_LOGI(TAG, "Step 6: UART init");
    lcd_status_post("UART");
    ESP_ERROR_CHECK(uart_stm32_init());

    /*
     * 启动 WiFi → UART 转发任务。
     *
     * 另一个 ESP32 发 UDP 到本机 SoftAP 后：
     * wifi_manager.c 收到数据
     *      ↓
     * g_remote_msg_queue
     *      ↓
     * uart.c 转发任务
     *      ↓
     * STM32
     */
    ESP_LOGI(TAG, "Step 7: Start UART forward task");
    ESP_ERROR_CHECK(uart_stm32_start_forward_task(g_remote_msg_queue));

    /*
     * 7. WiFi APSTA 初始化
     *
     * STA：连接手机热点，用于豆包 WebSocket
     * AP ：开启 SoftAP，等待另一个 ESP32 连接
     * UDP：接收另一个 ESP32 的遥控数据
     */
    ESP_LOGI(TAG, "Step 8: WiFi APSTA init");
    lcd_image_post(&img_car_wifi);
    ESP_ERROR_CHECK(wifi_manager_init_apsta(g_remote_msg_queue));

    /*
     * 8. 等待 STA 连上手机热点
     *
     * 豆包 WebSocket 和 SNTP 都需要外网。
     */
    ESP_LOGI(TAG, "Step 9: Wait STA connected");
    lcd_status_post("NET WAIT");
    ESP_ERROR_CHECK(wifi_manager_wait_sta_connected(portMAX_DELAY));

    lcd_status_post("NET OK");
    app_print_ip_info();

    /*
     * 9. SNTP 时间同步
     *
     * WebSocket TLS 证书校验需要正确系统时间。
     */
    ESP_LOGI(TAG, "Step 10: SNTP time sync");
    lcd_status_post("TIME");
    ESP_ERROR_CHECK(time_sync_init());

    /*
     * 10. 启动豆包语音交互任务
     *
     * 前置条件：
     * - WiFi 已联网
     * - 时间已同步
     * - ES8311 已初始化
     * - SPIFFS 已挂载
     */
    ESP_LOGI(TAG, "Step 11: Start Doubao client");
    lcd_status_post("CLOUD");
    ESP_ERROR_CHECK(doubao_client_start());

    lcd_status_post("READY");

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "System init done");
    ESP_LOGI(TAG, "SoftAP waits remote ESP32");
    ESP_LOGI(TAG, "Doubao voice task is running");
    ESP_LOGI(TAG, "========================================");

    /*
     * app_main 可以直接返回，FreeRTOS 任务继续运行。
     * 这里保留一个低频监控循环，方便后续扩展状态检测。
     */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        ESP_LOGI(TAG,
                 "heartbeat: sta=%d udp=%d doubao=%d audio=%d time=%d",
                 wifi_manager_is_sta_connected() ? 1 : 0,
                 wifi_manager_is_udp_server_started() ? 1 : 0,
                 doubao_client_is_started() ? 1 : 0,
                 audio_es8311_is_inited() ? 1 : 0,
                 time_sync_is_synced() ? 1 : 0);
    }
}