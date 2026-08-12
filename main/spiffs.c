#include "spiffs.h"

#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_spiffs.h"

#include "app_config.h"

static const char *TAG = "SPIFFS_STORAGE";

#ifndef SPIFFS_BASE_PATH
#define SPIFFS_BASE_PATH "/spiffs"
#endif

#ifndef SPIFFS_LABEL
#define SPIFFS_LABEL "storage"
#endif

#ifndef SPIFFS_MAX_FILES
#define SPIFFS_MAX_FILES 8
#endif

#ifndef SPIFFS_FORMAT_IF_MOUNT_FAILED
#define SPIFFS_FORMAT_IF_MOUNT_FAILED false
#endif

#ifndef LELE_WAKE_ACK_PCM
#define LELE_WAKE_ACK_PCM "/spiffs/awake.pcm"
#endif

#ifndef LELE_SLEEP_ACK_PCM
#define LELE_SLEEP_ACK_PCM "/spiffs/leave.pcm"
#endif

static bool s_spiffs_mounted = false;

static bool spiffs_storage_path_valid(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return false;
    }

    /*
     * 防止误删非 SPIFFS 路径。
     * 本项目里 SPIFFS 文件都应该位于 /spiffs 下。
     */
    //判断路径是否合法
    if (strncmp(path, SPIFFS_BASE_PATH, strlen(SPIFFS_BASE_PATH)) != 0) {
        ESP_LOGW(TAG, "path is not under %s: %s", SPIFFS_BASE_PATH, path);
        return false;
    }

    return true;
}

esp_err_t spiffs_storage_init(void)
{
    if (s_spiffs_mounted) {
        ESP_LOGW(TAG, "SPIFFS already mounted");
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE_PATH,
        .partition_label = SPIFFS_LABEL,
        .max_files = SPIFFS_MAX_FILES,
        .format_if_mount_failed = SPIFFS_FORMAT_IF_MOUNT_FAILED,
    };

    ESP_LOGI(TAG,
             "mount SPIFFS: base=%s label=%s max_files=%d format_if_failed=%d",
             SPIFFS_BASE_PATH,
             SPIFFS_LABEL,
             SPIFFS_MAX_FILES,
             SPIFFS_FORMAT_IF_MOUNT_FAILED ? 1 : 0);

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_ERR_INVALID_STATE) {
        /*
         * 说明已经被别的地方挂载过。
         * 对模块化项目来说，这里按成功处理更稳。
         */
        ESP_LOGW(TAG, "SPIFFS already registered by other module");
        s_spiffs_mounted = true;
        return spiffs_storage_print_info();
    }

    ESP_RETURN_ON_ERROR(ret, TAG, "esp_vfs_spiffs_register failed");

    s_spiffs_mounted = true;

    ESP_RETURN_ON_ERROR(
        spiffs_storage_print_info(),
        TAG,
        "spiffs_storage_print_info failed"
    );

    spiffs_storage_check_prompt_files();

    return ESP_OK;
}

esp_err_t spiffs_storage_deinit(void)
{
    if (!s_spiffs_mounted) {
        ESP_LOGW(TAG, "SPIFFS not mounted");
        return ESP_OK;
    }

    esp_err_t ret = esp_vfs_spiffs_unregister(SPIFFS_LABEL);
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_vfs_spiffs_unregister failed");

    s_spiffs_mounted = false;

    ESP_LOGI(TAG, "SPIFFS unmounted");

    return ESP_OK;
}

bool spiffs_storage_is_mounted(void)
{
    return s_spiffs_mounted;
}

esp_err_t spiffs_storage_get_info(size_t *total, size_t *used)
{
    ESP_RETURN_ON_FALSE(
        s_spiffs_mounted,
        ESP_ERR_INVALID_STATE,
        TAG,
        "SPIFFS not mounted"
    );

    size_t total_local = 0;
    size_t used_local = 0;

    esp_err_t ret = esp_spiffs_info(SPIFFS_LABEL, &total_local, &used_local);
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_spiffs_info failed");

    if (total != NULL) {
        *total = total_local;
    }

    if (used != NULL) {
        *used = used_local;
    }

    return ESP_OK;
}

esp_err_t spiffs_storage_print_info(void)
{
    size_t total = 0;
    size_t used = 0;

    esp_err_t ret = spiffs_storage_get_info(&total, &used);
    ESP_RETURN_ON_ERROR(ret, TAG, "spiffs_storage_get_info failed");

    ESP_LOGI(TAG,
             "SPIFFS mounted: total=%u used=%u free=%u",
             (unsigned int)total,
             (unsigned int)used,
             (unsigned int)(total >= used ? total - used : 0));

    return ESP_OK;
}

bool spiffs_storage_file_exists(const char *path)
{
    if (!s_spiffs_mounted) {
        ESP_LOGW(TAG, "SPIFFS not mounted");
        return false;
    }

    if (!spiffs_storage_path_valid(path)) {
        return false;
    }

    struct stat st;
    return stat(path, &st) == 0;
}

esp_err_t spiffs_storage_get_file_size(const char *path, size_t *size_out)
{
    ESP_RETURN_ON_FALSE(
        s_spiffs_mounted,
        ESP_ERR_INVALID_STATE,
        TAG,
        "SPIFFS not mounted"
    );

    ESP_RETURN_ON_FALSE(
        spiffs_storage_path_valid(path),
        ESP_ERR_INVALID_ARG,
        TAG,
        "invalid path"
    );

    ESP_RETURN_ON_FALSE(
        size_out != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "size_out is NULL"
    );

    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGW(TAG, "file not found: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    *size_out = (size_t)st.st_size;

    return ESP_OK;
}

esp_err_t spiffs_storage_remove_file(const char *path)
{
    ESP_RETURN_ON_FALSE(
        s_spiffs_mounted,
        ESP_ERR_INVALID_STATE,
        TAG,
        "SPIFFS not mounted"
    );

    ESP_RETURN_ON_FALSE(
        spiffs_storage_path_valid(path),
        ESP_ERR_INVALID_ARG,
        TAG,
        "invalid path"
    );

    if (!spiffs_storage_file_exists(path)) {
        ESP_LOGI(TAG, "file not exist, skip remove: %s", path);
        return ESP_OK;
    }

    if (remove(path) != 0) {
        ESP_LOGE(TAG, "remove file failed: %s", path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "file removed: %s", path);

    return ESP_OK;
}

esp_err_t spiffs_storage_gc(size_t size_to_gc)
{
    ESP_RETURN_ON_FALSE(
        s_spiffs_mounted,
        ESP_ERR_INVALID_STATE,
        TAG,
        "SPIFFS not mounted"
    );

    if (size_to_gc == 0) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "SPIFFS GC start, size=%u", (unsigned int)size_to_gc);

    esp_err_t ret = esp_spiffs_gc(SPIFFS_LABEL, size_to_gc);
    if (ret != ESP_OK) {
        /*
         * GC 失败不一定代表不能继续写，可能只是暂时无法回收到目标大小。
         * 这里返回错误，让上层自行决定是否继续。
         */
        ESP_LOGW(TAG, "esp_spiffs_gc failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SPIFFS GC done");

    return ESP_OK;
}

esp_err_t spiffs_storage_prepare_temp_file(const char *path, size_t gc_size)
{
    ESP_RETURN_ON_FALSE(
        s_spiffs_mounted,
        ESP_ERR_INVALID_STATE,
        TAG,
        "SPIFFS not mounted"
    );

    ESP_RETURN_ON_FALSE(
        spiffs_storage_path_valid(path),
        ESP_ERR_INVALID_ARG,
        TAG,
        "invalid temp file path"
    );

    ESP_LOGI(TAG, "prepare temp file: %s", path);

    esp_err_t ret = spiffs_storage_remove_file(path);
    ESP_RETURN_ON_ERROR(ret, TAG, "remove temp file failed");

    if (gc_size > 0) {
        ret = spiffs_storage_gc(gc_size);
        if (ret != ESP_OK) {
            /*
             * 不直接失败。
             * 因为有时候 GC 没达到目标值，但剩余空间仍可能够用。
             */
            ESP_LOGW(TAG, "GC before temp file not fully successful: %s",
                     esp_err_to_name(ret));
        }
    }

    spiffs_storage_print_info();

    return ESP_OK;
}

esp_err_t spiffs_storage_check_prompt_files(void)
{
    if (!s_spiffs_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *files[] = {
        LELE_WAKE_ACK_PCM,
        LELE_SLEEP_ACK_PCM,
    };

    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        const char *path = files[i];

        if (spiffs_storage_file_exists(path)) {
            size_t size = 0;
            esp_err_t ret = spiffs_storage_get_file_size(path, &size);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "prompt file OK: %s, size=%u",
                         path,
                         (unsigned int)size);
            } else {
                ESP_LOGI(TAG, "prompt file OK: %s", path);
            }
        } else {
            ESP_LOGW(TAG, "prompt file missing: %s", path);
        }
    }

    return ESP_OK;
}