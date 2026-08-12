#include "time_sync.h"



static const char *TAG = "TIME_SYNC";

#ifndef TIME_SYNC_SNTP_SERVER
#define TIME_SYNC_SNTP_SERVER "ntp.aliyun.com"
#endif

#ifndef TIME_SYNC_TIMEOUT_MS
#define TIME_SYNC_TIMEOUT_MS 15000
#endif

/*
 * POSIX TZ 格式：
 * 中国 / 新加坡 / UTC+8 可以写 CST-8。
 * 注意这里不是 UTC-8，而是 POSIX 规则反着写。
 */
#ifndef TIME_SYNC_TIMEZONE
#define TIME_SYNC_TIMEZONE "CST-8"
#endif

/*
 * 2024-01-01 00:00:00 UTC
 * 如果系统时间小于这个值，基本说明 SNTP 还没有同步成功。
 */
#ifndef TIME_SYNC_VALID_UNIX_SEC
#define TIME_SYNC_VALID_UNIX_SEC 1704067200LL
#endif

static bool s_time_sync_inited = false;
static bool s_time_sync_synced = false;

static void time_sync_set_timezone(void)
{
    setenv("TZ", TIME_SYNC_TIMEZONE, 1);
    tzset();

    ESP_LOGI(TAG, "Timezone set: %s", TIME_SYNC_TIMEZONE);
}

int64_t time_sync_now_sec(void)
{
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);

    return (int64_t)tv.tv_sec;
}

int64_t time_sync_now_ms(void)
{
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);

    return ((int64_t)tv.tv_sec * 1000LL) + ((int64_t)tv.tv_usec / 1000LL);
}

bool time_sync_is_time_valid(void)
{
    int64_t now_sec = time_sync_now_sec();

    return now_sec >= TIME_SYNC_VALID_UNIX_SEC;
}

bool time_sync_is_inited(void)
{
    return s_time_sync_inited;
}

bool time_sync_is_synced(void)
{
    return s_time_sync_synced && time_sync_is_time_valid();
}

esp_err_t time_sync_format_now(char *buf, size_t buf_size)
{
    ESP_RETURN_ON_FALSE(
        buf != NULL && buf_size > 0,
        ESP_ERR_INVALID_ARG,
        TAG,
        "invalid buffer"
    );

    time_t now = 0;
    struct tm timeinfo = {0};

    time(&now);
    localtime_r(&now, &timeinfo);

    int written = snprintf(
        buf,
        buf_size,
        "%04d-%02d-%02d %02d:%02d:%02d",
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
    );

    ESP_RETURN_ON_FALSE(
        written > 0 && written < (int)buf_size,
        ESP_FAIL,
        TAG,
        "format time failed"
    );

    return ESP_OK;
}

void time_sync_print_now(void)
{
    char time_buf[32] = {0};

    esp_err_t ret = time_sync_format_now(time_buf, sizeof(time_buf));

    if (ret == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Current local time: %s, unix_sec=%lld, unix_ms=%lld, valid=%d",
            time_buf,
            (long long)time_sync_now_sec(),
            (long long)time_sync_now_ms(),
            time_sync_is_time_valid() ? 1 : 0
        );
    } else {
        ESP_LOGW(
            TAG,
            "Current unix time: sec=%lld, ms=%lld, valid=%d",
            (long long)time_sync_now_sec(),
            (long long)time_sync_now_ms(),
            time_sync_is_time_valid() ? 1 : 0
        );
    }
}

esp_err_t time_sync_wait(uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        timeout_ms = TIME_SYNC_TIMEOUT_MS;
    }

    ESP_LOGI(TAG, "Waiting for SNTP sync, timeout=%lu ms", (unsigned long)timeout_ms);

    esp_err_t ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SNTP sync wait failed or timeout: %s", esp_err_to_name(ret));

        if (time_sync_is_time_valid()) {
            ESP_LOGW(TAG, "System time already valid, continue");
            s_time_sync_synced = true;
            time_sync_print_now();
            return ESP_OK;
        }

        s_time_sync_synced = false;
        time_sync_print_now();

        return ret;
    }

    if (!time_sync_is_time_valid()) {
        ESP_LOGW(TAG, "SNTP returned OK, but system time still invalid");
        s_time_sync_synced = false;
        time_sync_print_now();
        return ESP_FAIL;
    }

    s_time_sync_synced = true;

    ESP_LOGI(TAG, "SNTP sync success");
    time_sync_print_now();

    return ESP_OK;
}

esp_err_t time_sync_init(void)
{
    time_sync_set_timezone();

    if (s_time_sync_inited) {
        ESP_LOGW(TAG, "SNTP already initialized");

        if (time_sync_is_time_valid()) {
            s_time_sync_synced = true;
            time_sync_print_now();
            return ESP_OK;
        }

        return time_sync_wait(TIME_SYNC_TIMEOUT_MS);
    }

    ESP_LOGI(TAG, "SNTP init start, server=%s", TIME_SYNC_SNTP_SERVER);

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(TIME_SYNC_SNTP_SERVER);

    esp_err_t ret = esp_netif_sntp_init(&config);

    if (ret == ESP_ERR_INVALID_STATE) {
        /*
         * SNTP 已经被其他地方初始化过。
         * 模块化工程中这不算严重错误，按已初始化处理。
         */
        ESP_LOGW(TAG, "SNTP already initialized by another module");
        s_time_sync_inited = true;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(ret));
        return ret;
    } else {
        s_time_sync_inited = true;
    }

    ret = time_sync_wait(TIME_SYNC_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "time sync failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SNTP init done");

    return ESP_OK;
}

esp_err_t time_sync_deinit(void)
{
    if (!s_time_sync_inited) {
        ESP_LOGW(TAG, "SNTP not initialized");
        return ESP_OK;
    }

    esp_netif_sntp_deinit();

    s_time_sync_inited = false;
    s_time_sync_synced = false;

    ESP_LOGI(TAG, "SNTP deinit done");

    return ESP_OK;
}