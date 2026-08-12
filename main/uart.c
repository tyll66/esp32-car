#include "uart.h"
#include "command.h"

static const char *TAG = "UART_STM32";

#ifndef STM32_UART_PORT
#define STM32_UART_PORT UART_NUM_1
#endif

#ifndef STM32_UART_TX_GPIO
#define STM32_UART_TX_GPIO GPIO_NUM_43
#endif

#ifndef STM32_UART_RX_GPIO
#define STM32_UART_RX_GPIO GPIO_NUM_44
#endif

#ifndef STM32_UART_BAUD
#define STM32_UART_BAUD 115200
#endif

#ifndef STM32_UART_RX_BUF_SIZE
#define STM32_UART_RX_BUF_SIZE 1024
#endif

#ifndef STM32_UART_TX_BUF_SIZE
#define STM32_UART_TX_BUF_SIZE 1024
#endif

#ifndef STM32_UART_APPEND_NEWLINE_WHEN_FORWARD
#define STM32_UART_APPEND_NEWLINE_WHEN_FORWARD 1
#endif

static bool s_uart_inited = false;

static TaskHandle_t s_forward_task_handle = NULL;
static volatile bool s_forward_task_stop_req = false;

bool uart_stm32_is_inited(void)
{
    return s_uart_inited;
}

esp_err_t uart_stm32_init(void)
{
    if (s_uart_inited) {
        ESP_LOGW(TAG, "UART already initialized");
        return ESP_OK;
    }

    uart_config_t uart_cfg = {
        .baud_rate = STM32_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_LOGI(TAG,
             "UART init: port=%d TX=GPIO%d RX=GPIO%d baud=%d",
             STM32_UART_PORT,
             STM32_UART_TX_GPIO,
             STM32_UART_RX_GPIO,
             STM32_UART_BAUD);

    esp_err_t ret = uart_driver_install(
        STM32_UART_PORT,
        STM32_UART_RX_BUF_SIZE,
        STM32_UART_TX_BUF_SIZE,
        0,
        NULL,
        0
    );

    ESP_RETURN_ON_ERROR(ret, TAG, "uart_driver_install failed");

    ret = uart_param_config(STM32_UART_PORT, &uart_cfg);
    if (ret != ESP_OK) {
        uart_driver_delete(STM32_UART_PORT);
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(
        STM32_UART_PORT,
        STM32_UART_TX_GPIO,
        STM32_UART_RX_GPIO,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE
    );

    if (ret != ESP_OK) {
        uart_driver_delete(STM32_UART_PORT);
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_flush_input(STM32_UART_PORT);
    if (ret != ESP_OK) {
        uart_driver_delete(STM32_UART_PORT);
        ESP_LOGE(TAG, "uart_flush_input failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_uart_inited = true;

    ESP_LOGI(TAG, "UART init done");

    return ESP_OK;
}

esp_err_t uart_stm32_deinit(void)
{
    if (!s_uart_inited) {
        ESP_LOGW(TAG, "UART not initialized");
        return ESP_OK;
    }

    uart_stm32_stop_forward_task();

    /*
     * 给转发任务一点退出时间。
     */
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_err_t ret = uart_driver_delete(STM32_UART_PORT);
    ESP_RETURN_ON_ERROR(ret, TAG, "uart_driver_delete failed");

    s_uart_inited = false;

    ESP_LOGI(TAG, "UART deinit done");

    return ESP_OK;
}

esp_err_t uart_stm32_send_bytes(const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(
        s_uart_inited,
        ESP_ERR_INVALID_STATE,
        TAG,
        "UART not initialized"
    );

    ESP_RETURN_ON_FALSE(
        data != NULL && len > 0,
        ESP_ERR_INVALID_ARG,
        TAG,
        "invalid send data"
    );

    int written = uart_write_bytes(
        STM32_UART_PORT,
        (const char *)data,
        len
    );

    if (written < 0) {
        ESP_LOGE(TAG, "uart_write_bytes failed");
        return ESP_FAIL;
    }

    if ((size_t)written != len) {
        ESP_LOGW(TAG, "UART write incomplete: written=%d want=%u",
                 written,
                 (unsigned int)len);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t uart_stm32_send_line(const char *line)
{
    ESP_RETURN_ON_FALSE(
        line != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "line is NULL"
    );

    size_t len = strlen(line);
    ESP_RETURN_ON_FALSE(
        len > 0,
        ESP_ERR_INVALID_ARG,
        TAG,
        "line is empty"
    );

    ESP_LOGI(TAG, "TX STM32: %s", line);

    return uart_stm32_send_bytes((const uint8_t *)line, len);
}

esp_err_t uart_stm32_send_cmd_code(const char *code)
{
    ESP_RETURN_ON_FALSE(
        code != NULL && code[0] != '\0',
        ESP_ERR_INVALID_ARG,
        TAG,
        "invalid cmd code"
    );

    char line[16] = {0};

    int written = snprintf(line, sizeof(line), "%s\n", code);
    ESP_RETURN_ON_FALSE(
        written > 0 && written < (int)sizeof(line),
        ESP_FAIL,
        TAG,
        "cmd code too long"
    );

    return uart_stm32_send_line(line);
}

int uart_stm32_read_bytes(uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (!s_uart_inited) {
        ESP_LOGW(TAG, "UART not initialized");
        return -1;
    }

    if (data == NULL || len == 0) {
        ESP_LOGW(TAG, "invalid read buffer");
        return -1;
    }

    int read_len = uart_read_bytes(
        STM32_UART_PORT,
        data,
        len,
        pdMS_TO_TICKS(timeout_ms)
    );

    return read_len;
}

esp_err_t uart_stm32_flush_input(void)
{
    ESP_RETURN_ON_FALSE(
        s_uart_inited,
        ESP_ERR_INVALID_STATE,
        TAG,
        "UART not initialized"
    );

    return uart_flush_input(STM32_UART_PORT);
}

static bool uart_msg_need_append_newline(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return false;
    }

    uint8_t last = data[len - 1];

    return !(last == '\n' || last == '\r');
}

static void uart_forward_task(void *arg)
{
    QueueHandle_t q = (QueueHandle_t)arg;
    remote_msg_t msg;

    ESP_LOGI(TAG, "UART forward task started");

    while (!s_forward_task_stop_req) {
        if (q == NULL) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        memset(&msg, 0, sizeof(msg));

        if (xQueueReceive(
                q,
                &msg,
                pdMS_TO_TICKS(200)
            ) != pdTRUE) {
            continue;
        }

        if (msg.len == 0) {
            continue;
        }

        if (msg.len >= REMOTE_MSG_MAX_LEN) {
            msg.len = REMOTE_MSG_MAX_LEN - 1;
        }

        msg.data[msg.len] = '\0';

        ESP_LOGI(
            TAG,
            "Handle WiFi command: len=%u data=%.*s",
            (unsigned int)msg.len,
            (int)msg.len,
            (const char *)msg.data
        );

        bool handled = command_router_handle_remote_bytes(
            msg.data,
            msg.len
        );

        if (!handled) {
            ESP_LOGW(
                TAG,
                "Remote command parse failed: %.*s",
                (int)msg.len,
                (const char *)msg.data
            );

            lcd_status_post("BAD CMD");
        }
    }

    ESP_LOGI(TAG, "UART forward task stopped");

    s_forward_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t uart_stm32_start_forward_task(QueueHandle_t remote_msg_queue)
{
    ESP_RETURN_ON_FALSE(
        s_uart_inited,
        ESP_ERR_INVALID_STATE,
        TAG,
        "UART not initialized"
    );

    ESP_RETURN_ON_FALSE(
        remote_msg_queue != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "remote_msg_queue is NULL"
    );

    if (s_forward_task_handle != NULL) {
        ESP_LOGW(TAG, "UART forward task already started");
        return ESP_OK;
    }

    s_forward_task_stop_req = false;

    BaseType_t ret = xTaskCreate(
        uart_forward_task,
        "uart_forward_task",
        4096,
        remote_msg_queue,
        6,
        &s_forward_task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "create uart_forward_task failed");
        s_forward_task_handle = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "UART forward task create done");

    return ESP_OK;
}

void uart_stm32_stop_forward_task(void)
{
    if (s_forward_task_handle == NULL) {
        return;
    }

    s_forward_task_stop_req = true;
}