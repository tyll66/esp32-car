#include "wifi_manager.h"


LV_IMAGE_DECLARE(img_car_wifi);
static const char *TAG = "WIFI_MANAGER";

/* ================= 默认配置兜底 ================= */

#ifndef WIFI_STA_SSID
#define WIFI_STA_SSID "ty"
#endif

#ifndef WIFI_STA_PASS
#define WIFI_STA_PASS "12345678"
#endif

#ifndef WIFI_AP_SSID
#define WIFI_AP_SSID "DeskCar_Master"
#endif

#ifndef WIFI_AP_PASS
#define WIFI_AP_PASS "12345678"
#endif

#ifndef WIFI_AP_CHANNEL
#define WIFI_AP_CHANNEL 6
#endif

#ifndef WIFI_AP_MAX_CONN
#define WIFI_AP_MAX_CONN 2
#endif

#ifndef WIFI_REMOTE_UDP_PORT
#define WIFI_REMOTE_UDP_PORT 3333
#endif

#ifndef WIFI_UDP_TASK_STACK_SIZE
#define WIFI_UDP_TASK_STACK_SIZE 4096
#endif

#ifndef WIFI_UDP_TASK_PRIORITY
#define WIFI_UDP_TASK_PRIORITY 5
#endif

#ifndef WIFI_STA_CONNECT_TIMEOUT_MS
#define WIFI_STA_CONNECT_TIMEOUT_MS 15000
#endif

/* ================= 内部状态 ================= */

#define WIFI_STA_CONNECTED_BIT BIT0
#define WIFI_STA_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

static QueueHandle_t s_remote_msg_queue = NULL;

static TaskHandle_t s_udp_server_task_handle = NULL;
static volatile bool s_udp_server_stop_req = false;

static bool s_wifi_inited = false;
static bool s_sta_connected = false;
static bool s_udp_server_started = false;

typedef enum {
    REMOTE_MODE_EVT_CONNECTED = 1,
    REMOTE_MODE_EVT_DISCONNECTED,
} remote_mode_evt_t;

static QueueHandle_t s_remote_mode_queue = NULL;
static TaskHandle_t s_remote_mode_task_handle = NULL;

static volatile int s_remote_client_count = 0;
static volatile bool s_remote_control_mode = false;


static int s_udp_sock = -1;

/* ================= 工具函数 ================= */

static esp_err_t wifi_manager_nvs_init_once(void)
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

static esp_err_t wifi_manager_event_loop_create_once(void)
{
    esp_err_t ret = esp_event_loop_create_default();

    if (ret == ESP_ERR_INVALID_STATE) {
        /*
         * 说明默认 event loop 已经创建过。
         * 模块化工程里这很常见，按成功处理。
         */
        ESP_LOGW(TAG, "default event loop already created");
        return ESP_OK;
    }

    return ret;
}

static void wifi_manager_copy_str_to_wifi_field(uint8_t *dst,
                                                size_t dst_size,
                                                const char *src)
{
    if (dst == NULL || dst_size == 0 || src == NULL) {
        return;
    }

    memset(dst, 0, dst_size);
    strncpy((char *)dst, src, dst_size - 1);
}



static void wifi_post_remote_mode_event(remote_mode_evt_t evt)
{
    if (s_remote_mode_queue == NULL) {
        return;
    }

    xQueueSend(s_remote_mode_queue, &evt, 0);
}

static void wifi_remote_mode_task(void *arg)
{
    (void)arg;

    remote_mode_evt_t evt;

    while (1) {
        if (xQueueReceive(s_remote_mode_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (evt == REMOTE_MODE_EVT_CONNECTED) {
            if (!s_remote_control_mode) {
                s_remote_control_mode = true;

                ESP_LOGW(TAG, "Enter remote control mode");

                lcd_status_post("REMOTE");

                /*
                 * 进入遥控模式时，先给 STM32 发停止。
                 * 避免语音任务刚好留下前进/后退状态。
                 */
                command_router_send_cmd(COMMAND_ROUTER_CMD_STOP);

                /*
                 * 停止豆包语音任务。
                 * 注意：doubao_client_stop() 是非阻塞的，只是请求停止。
                 * 语音任务会在当前一轮结束后退出。
                 */
                doubao_client_stop();
            }

            continue;
        }

        if (evt == REMOTE_MODE_EVT_DISCONNECTED) {
            /*
             * 只有所有从机都断开，才恢复豆包。
             * 如果后面允许多个 ESP32 从机连接，这个判断很重要。
             */
            if (s_remote_client_count > 0) {
                ESP_LOGI(TAG,
                         "remote client still exists, count=%d",
                         s_remote_client_count);
                continue;
            }

            if (s_remote_control_mode) {
                ESP_LOGW(TAG, "Exit remote control mode");

                s_remote_control_mode = false;

                lcd_status_post("VOICE WAIT");

                /*
                 * 退出遥控模式时，也先发停止。
                 */
                command_router_send_cmd(COMMAND_ROUTER_CMD_STOP);

                /*
                 * 等待旧的豆包任务真正退出。
                 * 因为 doubao_client_stop() 只是设置停止标志。
                 */
                while (doubao_client_is_started()) {
                    /*
                     * 如果等待过程中从机又重新连上了，
                     * 不要恢复豆包，重新保持遥控模式。
                     */
                    if (s_remote_client_count > 0) {
                        ESP_LOGW(TAG, "remote reconnected while waiting voice stop");
                        s_remote_control_mode = true;
                        lcd_status_post("REMOTE");
                        break;
                    }

                    vTaskDelay(pdMS_TO_TICKS(200));
                }

                /*
                 * 确认当前没有从机连接，才重新启动豆包。
                 */
                if (s_remote_client_count <= 0 && !s_remote_control_mode) {
                    ESP_LOGW(TAG, "Restart Doubao voice interaction");

                    esp_err_t ret = doubao_client_start();
                    if (ret == ESP_OK) {
                        lcd_status_post("VOICE");
                    } else {
                        ESP_LOGE(TAG,
                                 "doubao restart failed: %s",
                                 esp_err_to_name(ret));
                        lcd_status_post("VOICE ERR");
                    }
                }
            }

            continue;
        }
    }
}


/* ================= WiFi 事件处理 ================= */

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA start, connecting to hotspot...");
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;

        s_sta_connected = false;

        ESP_LOGW(TAG,
                 "STA disconnected, reason=%d, reconnecting...",
                 disconn ? disconn->reason : -1);

        if (s_wifi_event_group != NULL) {
            xEventGroupClearBits(s_wifi_event_group, WIFI_STA_CONNECTED_BIT);
        }

        lcd_image_post(&img_car_wifi);

        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        s_sta_connected = true;

        ESP_LOGI(TAG,
                 "STA got IP: " IPSTR,
                 IP2STR(&event->ip_info.ip));

        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_STA_CONNECTED_BIT);
        }

        lcd_status_post("NET OK");
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "SoftAP started, SSID=%s", WIFI_AP_SSID);
        lcd_status_post("AP ON");
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        ESP_LOGW(TAG, "SoftAP stopped");
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;

        s_remote_client_count++;

        ESP_LOGI(TAG,
                 "Remote ESP32 connected, AID=%d",
                 event ? event->aid : -1);

        lcd_status_post("ESP32 IN");
        //进入遥控模式
        wifi_post_remote_mode_event(REMOTE_MODE_EVT_CONNECTED);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;

        if (s_remote_client_count > 0) {
        s_remote_client_count--;
        }

        ESP_LOGW(TAG,
                 "Remote ESP32 disconnected, AID=%d",
                 event ? event->aid : -1);

        lcd_status_post("ESP32 OUT");
        
        if (s_remote_client_count <= 0) {
            wifi_post_remote_mode_event(REMOTE_MODE_EVT_DISCONNECTED);
        }

        return;
    }
}

/* ================= UDP Server ================= */

static void wifi_udp_server_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "UDP server task start");

    s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_udp_sock < 0) {
        ESP_LOGE(TAG, "socket create failed, errno=%d", errno);
        s_udp_server_started = false;
        s_udp_server_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in listen_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(WIFI_REMOTE_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    int ret = bind(s_udp_sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr));
    if (ret < 0) {
        ESP_LOGE(TAG, "UDP bind failed, errno=%d", errno);
        close(s_udp_sock);
        s_udp_sock = -1;
        s_udp_server_started = false;
        s_udp_server_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    /*
     * 设置 recv 超时，方便 stop 时能退出循环。
     */
    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = 200 * 1000,
    };
    //socket配置
    setsockopt(s_udp_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    s_udp_server_started = true;

    ESP_LOGI(TAG, "UDP server listening on port %d", WIFI_REMOTE_UDP_PORT);
    lcd_status_post("UDP ON");

    while (!s_udp_server_stop_req) {
        remote_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        //ipv4
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        memset(&source_addr, 0, sizeof(source_addr));

        //数据接收
        int len = recvfrom(
            s_udp_sock,
            msg.data,
            REMOTE_MSG_MAX_LEN - 1,
            0,
            (struct sockaddr *)&source_addr,
            &socklen
        );

        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }

            ESP_LOGW(TAG, "recvfrom failed, errno=%d", errno);
            continue;
        }

        if (len == 0) {
            continue;
        }

        if (len >= REMOTE_MSG_MAX_LEN) {
            len = REMOTE_MSG_MAX_LEN - 1;
        }

        msg.len = (size_t)len;
        msg.data[len] = '\0';

        char addr_str[32] = {0};
        //转化为可读ip
        inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str) - 1);

        ESP_LOGI(TAG,
                 "UDP RX from %s:%d, len=%d, data=%.*s",
                 addr_str,
                 ntohs(source_addr.sin_port),
                 len,
                 len,
                 (const char *)msg.data);

        // lcd_status_post("UDP RX");

        if (s_remote_msg_queue != NULL) {
            BaseType_t ok = xQueueSend(s_remote_msg_queue, &msg, pdMS_TO_TICKS(20));
            if (ok != pdTRUE) {
                ESP_LOGW(TAG, "remote_msg_queue full, drop UDP msg");
                lcd_status_post("Q FULL");
            }
        } else {
            ESP_LOGW(TAG, "remote_msg_queue is NULL, drop UDP msg");
        }
    }

    if (s_udp_sock >= 0) {
        close(s_udp_sock);
        s_udp_sock = -1;
    }

    s_udp_server_started = false;
    s_udp_server_task_handle = NULL;

    ESP_LOGI(TAG, "UDP server task stopped");

    vTaskDelete(NULL);
}

static esp_err_t wifi_manager_start_udp_server(void)
{
    if (s_udp_server_task_handle != NULL) {
        ESP_LOGW(TAG, "UDP server already started");
        return ESP_OK;
    }

    s_udp_server_stop_req = false;

    BaseType_t ret = xTaskCreate(
        wifi_udp_server_task,
        "wifi_udp_server",
        WIFI_UDP_TASK_STACK_SIZE,
        NULL,
        WIFI_UDP_TASK_PRIORITY,
        &s_udp_server_task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "create UDP server task failed");
        s_udp_server_task_handle = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* ================= 对外 API ================= */




esp_err_t wifi_manager_init_apsta(QueueHandle_t remote_msg_queue)
{
    if (s_wifi_inited) {
        ESP_LOGW(TAG, "WiFi manager already initialized");
        return ESP_OK;
    }

    s_remote_msg_queue = remote_msg_queue;

    if (s_remote_mode_queue == NULL) {
        s_remote_mode_queue = xQueueCreate(4, sizeof(remote_mode_evt_t));
        if (s_remote_mode_queue == NULL) {
            ESP_LOGE(TAG, "remote mode queue create failed");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_remote_mode_task_handle == NULL) {
        BaseType_t task_ret = xTaskCreate(
            wifi_remote_mode_task,
            "wifi_remote_mode_task",
            4096,
            NULL,
            5,
            &s_remote_mode_task_handle
        );

        if (task_ret != pdPASS) {
            ESP_LOGE(TAG, "remote mode task create failed");
            return ESP_FAIL;
        }
    }


    ESP_RETURN_ON_FALSE(
        s_remote_msg_queue != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "remote_msg_queue is NULL"
    );

    s_wifi_event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(
        s_wifi_event_group != NULL,
        ESP_ERR_NO_MEM,
        TAG,
        "wifi event group create failed"
    );

    ESP_RETURN_ON_ERROR(
        wifi_manager_nvs_init_once(),
        TAG,
        "NVS init failed"
    );

    esp_err_t ret = esp_netif_init();
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_netif already initialized");
    } else {
        ESP_RETURN_ON_ERROR(ret, TAG, "esp_netif_init failed");
    }

    ESP_RETURN_ON_ERROR(
        wifi_manager_event_loop_create_once(),
        TAG,
        "event loop create failed"
    );

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        ESP_LOGW(TAG, "STA netif may already exist");
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        ESP_LOGW(TAG, "AP netif may already exist");
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_RETURN_ON_ERROR(
        esp_wifi_init(&cfg),
        TAG,
        "esp_wifi_init failed"
    );

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event_handler,
            NULL,
            NULL
        ),
        TAG,
        "register WIFI_EVENT handler failed"
    );

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            wifi_event_handler,
            NULL,
            NULL
        ),
        TAG,
        "register IP_EVENT handler failed"
    );

    wifi_config_t sta_cfg;
    memset(&sta_cfg, 0, sizeof(sta_cfg));

    wifi_manager_copy_str_to_wifi_field(
        sta_cfg.sta.ssid,
        sizeof(sta_cfg.sta.ssid),
        WIFI_STA_SSID
    );

    wifi_manager_copy_str_to_wifi_field(
        sta_cfg.sta.password,
        sizeof(sta_cfg.sta.password),
        WIFI_STA_PASS
    );
    //过滤低加密等级wifi
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    wifi_config_t ap_cfg;
    memset(&ap_cfg, 0, sizeof(ap_cfg));

    wifi_manager_copy_str_to_wifi_field(
        ap_cfg.ap.ssid,
        sizeof(ap_cfg.ap.ssid),
        WIFI_AP_SSID
    );

    wifi_manager_copy_str_to_wifi_field(
        ap_cfg.ap.password,
        sizeof(ap_cfg.ap.password),
        WIFI_AP_PASS
    );

    ap_cfg.ap.ssid_len = strlen(WIFI_AP_SSID);
    ap_cfg.ap.channel = WIFI_AP_CHANNEL;
    ap_cfg.ap.max_connection = WIFI_AP_MAX_CONN;

    if (strlen(WIFI_AP_PASS) == 0) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_RETURN_ON_ERROR(
        esp_wifi_set_mode(WIFI_MODE_APSTA),
        TAG,
        "esp_wifi_set_mode APSTA failed"
    );

    ESP_RETURN_ON_ERROR(
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg),
        TAG,
        "esp_wifi_set_config STA failed"
    );

    ESP_RETURN_ON_ERROR(
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg),
        TAG,
        "esp_wifi_set_config AP failed"
    );
    
    ESP_RETURN_ON_ERROR(
        esp_wifi_start(),
        TAG,
        "esp_wifi_start failed"
    );

    /*
     * 关闭 WiFi 省电，降低 WebSocket 音频流卡顿概率。
     */
    ESP_RETURN_ON_ERROR(
        esp_wifi_set_ps(WIFI_PS_NONE),
        TAG,
        "esp_wifi_set_ps failed"
    );

    ESP_LOGI(TAG, "WiFi APSTA started");
    ESP_LOGI(TAG, "STA connecting to: %s", WIFI_STA_SSID);
    ESP_LOGI(TAG, "SoftAP SSID: %s, password: %s", WIFI_AP_SSID, WIFI_AP_PASS);
    ESP_LOGI(TAG, "UDP server port: %d", WIFI_REMOTE_UDP_PORT);

    ESP_RETURN_ON_ERROR(
        wifi_manager_start_udp_server(),
        TAG,
        "start UDP server failed"
    );

    s_wifi_inited = true;

    return ESP_OK;
}

esp_err_t wifi_manager_wait_sta_connected(TickType_t timeout_ticks)
{
    ESP_RETURN_ON_FALSE(
        s_wifi_event_group != NULL,
        ESP_ERR_INVALID_STATE,
        TAG,
        "wifi event group not created"
    );

    ESP_LOGI(TAG, "Waiting STA connected...");

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_STA_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        timeout_ticks
    );

    if (bits & WIFI_STA_CONNECTED_BIT) {
        ESP_LOGI(TAG, "STA connected");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "STA connect timeout");
    return ESP_ERR_TIMEOUT;
}

bool wifi_manager_is_sta_connected(void)
{
    return s_sta_connected;
}

bool wifi_manager_is_udp_server_started(void)
{
    return s_udp_server_started;
}

esp_err_t wifi_manager_get_sta_ip_str(char *buf, size_t buf_size)
{
    ESP_RETURN_ON_FALSE(
        buf != NULL && buf_size > 0,
        ESP_ERR_INVALID_ARG,
        TAG,
        "invalid buffer"
    );

    ESP_RETURN_ON_FALSE(
        s_sta_netif != NULL,
        ESP_ERR_INVALID_STATE,
        TAG,
        "STA netif is NULL"
    );

    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));

    ESP_RETURN_ON_ERROR(
        esp_netif_get_ip_info(s_sta_netif, &ip_info),
        TAG,
        "get STA ip failed"
    );

    snprintf(
        buf,
        buf_size,
        IPSTR,
        IP2STR(&ip_info.ip)
    );

    return ESP_OK;
}

esp_err_t wifi_manager_get_ap_ip_str(char *buf, size_t buf_size)
{
    ESP_RETURN_ON_FALSE(
        buf != NULL && buf_size > 0,
        ESP_ERR_INVALID_ARG,
        TAG,
        "invalid buffer"
    );

    ESP_RETURN_ON_FALSE(
        s_ap_netif != NULL,
        ESP_ERR_INVALID_STATE,
        TAG,
        "AP netif is NULL"
    );

    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));

    ESP_RETURN_ON_ERROR(
        esp_netif_get_ip_info(s_ap_netif, &ip_info),
        TAG,
        "get AP ip failed"
    );

    snprintf(
        buf,
        buf_size,
        IPSTR,
        IP2STR(&ip_info.ip)
    );

    return ESP_OK;
}

void wifi_manager_stop_udp_server(void)
{
    if (s_udp_server_task_handle == NULL) {
        return;
    }

    s_udp_server_stop_req = true;

    if (s_udp_sock >= 0) {
        shutdown(s_udp_sock, SHUT_RDWR);
    }
}

esp_err_t wifi_manager_deinit(void)
{
    if (!s_wifi_inited) {
        ESP_LOGW(TAG, "WiFi manager not initialized");
        return ESP_OK;
    }

    wifi_manager_stop_udp_server();
    vTaskDelay(pdMS_TO_TICKS(300));

    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_wifi_event_group != NULL) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    s_sta_netif = NULL;
    s_ap_netif = NULL;
    s_remote_msg_queue = NULL;

    s_sta_connected = false;
    s_wifi_inited = false;

    ESP_LOGI(TAG, "WiFi manager deinit done");

    return ESP_OK;
}