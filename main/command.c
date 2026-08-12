#include "command.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_err.h"

#include "uart.h"
#include "lcd_driver.h"

LV_IMAGE_DECLARE(img_car_stop);
LV_IMAGE_DECLARE(img_car_forward);
LV_IMAGE_DECLARE(img_car_backward);
LV_IMAGE_DECLARE(img_car_left);
LV_IMAGE_DECLARE(img_car_right);
LV_IMAGE_DECLARE(img_car_patrol);
LV_IMAGE_DECLARE(img_car_hand_up);
LV_IMAGE_DECLARE(img_car_hand_down);
LV_IMAGE_DECLARE(img_car_dance);

static const char *TAG = "CMD_ROUTER";

#define CMD_TEXT_MAX_LEN 64

static bool str_is_empty(const char *s)
{
    return s == NULL || s[0] == '\0';
}

static bool text_has(const char *text, const char *key)
{
    if (text == NULL || key == NULL) {
        return false;
    }

    return strstr(text, key) != NULL;
}

static bool text_has_any3(const char *text,
                          const char *a,
                          const char *b,
                          const char *c)
{
    if (str_is_empty(text)) {
        return false;
    }

    if (a != NULL && strstr(text, a) != NULL) {
        return true;
    }

    if (b != NULL && strstr(text, b) != NULL) {
        return true;
    }

    if (c != NULL && strstr(text, c) != NULL) {
        return true;
    }

    return false;
}

static void trim_ascii_inplace(char *s)
{
    if (s == NULL) {
        return;
    }

    size_t len = strlen(s);

    while (len > 0) {
        char c = s[len - 1];

        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            s[len - 1] = '\0';
            len--;
        } else {
            break;
        }
    }

    char *start = s;

    while (*start == ' ' ||
           *start == '\t' ||
           *start == '\r' ||
           *start == '\n') {
        start++;
    }

    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
}

static void ascii_to_lower_inplace(char *s)
{
    if (s == NULL) {
        return;
    }

    while (*s) {
        *s = (char)tolower((unsigned char)*s);
        s++;
    }
}

const char *command_router_cmd_to_code(command_router_cmd_t cmd)
{
    switch (cmd) {
    case COMMAND_ROUTER_CMD_STOP:
        return "S";

    case COMMAND_ROUTER_CMD_FORWARD:
        return "F";

    case COMMAND_ROUTER_CMD_BACKWARD:
        return "B";

    case COMMAND_ROUTER_CMD_LEFT:
        return "L";

    case COMMAND_ROUTER_CMD_RIGHT:
        return "R";

    case COMMAND_ROUTER_CMD_PATROL:
        return "P";

    case COMMAND_ROUTER_CMD_HAND_UP:
        return "H";

    case COMMAND_ROUTER_CMD_HAND_DOWN:
        return "D";

    case COMMAND_ROUTER_CMD_DANCE:
        return "W";

    case COMMAND_ROUTER_CMD_RD:
        return "RD";

    case COMMAND_ROUTER_CMD_RO:
        return "RO";

    case COMMAND_ROUTER_CMD_LD:
        return "LD";

    case COMMAND_ROUTER_CMD_LO:
        return "LO";

    case COMMAND_ROUTER_CMD_NONE:
    default:
        return NULL;
    }
}

static const lv_image_dsc_t *
command_router_cmd_to_lcd_image(command_router_cmd_t cmd)
{
    switch (cmd) {
    case COMMAND_ROUTER_CMD_STOP:
        return &img_car_stop;

    case COMMAND_ROUTER_CMD_FORWARD:
        return &img_car_forward;

    case COMMAND_ROUTER_CMD_BACKWARD:
        return &img_car_backward;

    case COMMAND_ROUTER_CMD_LEFT:
        return &img_car_left;

    case COMMAND_ROUTER_CMD_RIGHT:
        return &img_car_right;

    case COMMAND_ROUTER_CMD_PATROL:
        return &img_car_patrol;

    case COMMAND_ROUTER_CMD_HAND_UP:
        return &img_car_hand_up;

    case COMMAND_ROUTER_CMD_HAND_DOWN:
        return &img_car_hand_down;

    case COMMAND_ROUTER_CMD_DANCE:
        return &img_car_dance;

    case COMMAND_ROUTER_CMD_NONE:
    default:
        return &img_car_stop;
    }
}

static const char *command_router_cmd_to_lcd_text(command_router_cmd_t cmd)
{
    switch (cmd) {
    case COMMAND_ROUTER_CMD_STOP:
        return "STOP";

    case COMMAND_ROUTER_CMD_FORWARD:
        return "FORWARD";

    case COMMAND_ROUTER_CMD_BACKWARD:
        return "BACK";

    case COMMAND_ROUTER_CMD_LEFT:
        return "LEFT";

    case COMMAND_ROUTER_CMD_RIGHT:
        return "RIGHT";

    case COMMAND_ROUTER_CMD_PATROL:
        return "PATROL";

    case COMMAND_ROUTER_CMD_HAND_UP:
        return "HAND UP";

    case COMMAND_ROUTER_CMD_HAND_DOWN:
        return "HAND DOWN";

    case COMMAND_ROUTER_CMD_DANCE:
        return "DANCE";

    case COMMAND_ROUTER_CMD_RD:
        return "RD";

    case COMMAND_ROUTER_CMD_RO:
        return "RO";

    case COMMAND_ROUTER_CMD_LD:
        return "LD";

    case COMMAND_ROUTER_CMD_LO:
        return "LO";

    case COMMAND_ROUTER_CMD_NONE:
    default:
        return "CMD?";
    }
}

esp_err_t command_router_send_cmd(command_router_cmd_t cmd)
{
    const char *code = command_router_cmd_to_code(cmd);

    if (code == NULL) {
        ESP_LOGW(TAG, "invalid command: %d", (int)cmd);
        return ESP_ERR_INVALID_ARG;
    }

    char line[16] = {0};

    snprintf(
        line,
        sizeof(line),
        "cmd:%s\n",
        code
    );

    ESP_LOGI(
        TAG,
        "send to STM32: cmd:%s",
        code
    );

    esp_err_t ret = uart_stm32_send_line(line);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "send to STM32 failed: %s",
            esp_err_to_name(ret)
        );

        lcd_status_post("SEND ERROR");
        return ret;
    }

    /*
     * 确认串口发送成功后，再更新LCD。
     */
    const lv_image_dsc_t *image =
    command_router_cmd_to_lcd_image(cmd);

    if (image != NULL) {
        lcd_image_post(image);
    } else {
        ESP_LOGW(
            TAG,
            "No LCD image for command: %d",
            (int)cmd
        );
    }

    return ESP_OK;
}

command_router_cmd_t command_router_parse_voice_text(const char *text)
{
    if (str_is_empty(text)) {
        return COMMAND_ROUTER_CMD_NONE;
    }

    /*
     * 停止类优先级最高。
     * 避免“不要前进”“别往左转”这类句子被误判成前进/左转。
     */
    if (text_has_any3(text, "停止", "停下", "别动") ||
        text_has_any3(text, "停车", "停住", "不要动") ||
        text_has_any3(text, "不要走", "别走", "暂停")) {
        return COMMAND_ROUTER_CMD_STOP;
    }

    /*
     * 新增动作：举手 / 放下 / 跳舞
     */
    if (text_has_any3(text, "举手", "抬手", "举起手") ||
        text_has_any3(text, "把手举起来", "手举起来", "举一下手")) {
        return COMMAND_ROUTER_CMD_HAND_UP;
    }

    if (text_has_any3(text, "放下", "放手", "手放下") ||
        text_has_any3(text, "把手放下", "胳膊放下", "手臂放下")) {
        return COMMAND_ROUTER_CMD_HAND_DOWN;
    }

    if (text_has_any3(text, "跳舞", "跳个舞", "跳一段") ||
        text_has_any3(text, "舞蹈", "扭一扭", "动起来")) {
        return COMMAND_ROUTER_CMD_DANCE;
    }

    if (text_has_any3(text, "前进", "向前", "往前") ||
        text_has_any3(text, "前走", "往前走", "向前走") ||
        text_has_any3(text, "走", "出发", "开始走")) {
        return COMMAND_ROUTER_CMD_FORWARD;
    }

    if (text_has_any3(text, "后退", "倒车", "向后") ||
        text_has_any3(text, "往后", "往后退", "向后退") ||
        text_has_any3(text, "退后", "倒退", NULL)) {
        return COMMAND_ROUTER_CMD_BACKWARD;
    }

    if (text_has_any3(text, "左转", "向左", "往左") ||
        text_has_any3(text, "左拐", "转左", NULL)) {
        return COMMAND_ROUTER_CMD_LEFT;
    }

    if (text_has_any3(text, "右转", "向右", "往右") ||
        text_has_any3(text, "右拐", "转右", NULL)) {
        return COMMAND_ROUTER_CMD_RIGHT;
    }

    if (text_has_any3(text, "右拐", "转右", NULL)) {
        return COMMAND_ROUTER_CMD_RIGHT;
    }

    if (text_has_any3(text, "巡逻", "开始巡逻", "自动巡逻") ||
        text_has_any3(text, "巡航", "自动走", NULL)) {
        return COMMAND_ROUTER_CMD_PATROL;
    }

    return COMMAND_ROUTER_CMD_NONE;
}

command_router_cmd_t command_router_parse_remote_text(const char *text)
{
    if (str_is_empty(text)) {
        return COMMAND_ROUTER_CMD_NONE;
    }

    char buf[CMD_TEXT_MAX_LEN] = {0};

    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    trim_ascii_inplace(buf);

    if (buf[0] == '\0') {
        return COMMAND_ROUTER_CMD_NONE;
    }

    /*
     * 单字符协议。
     * 另一个 ESP32 或上位机最推荐直接发：
     *
     * F = 前进
     * B = 后退
     * L = 左转
     * R = 右转
     * S = 停止
     * P = 巡逻
     * H = 举手
     * D = 放下
     * W = 跳舞
     */
    if (strcmp(buf, "RD") == 0 ||
    strcmp(buf, "rd") == 0) {
    return COMMAND_ROUTER_CMD_RD;
    }

    if (strcmp(buf, "RO") == 0 ||
        strcmp(buf, "ro") == 0) {
        return COMMAND_ROUTER_CMD_RO;
    }

    if (strcmp(buf, "LD") == 0 ||
        strcmp(buf, "ld") == 0) {
        return COMMAND_ROUTER_CMD_LD;
    }

    if (strcmp(buf, "LO") == 0 ||
        strcmp(buf, "lo") == 0) {
        return COMMAND_ROUTER_CMD_LO;
    }
    if (strcmp(buf, "F") == 0 || strcmp(buf, "f") == 0) {
        return COMMAND_ROUTER_CMD_FORWARD;
    }

    if (strcmp(buf, "B") == 0 || strcmp(buf, "b") == 0) {
        return COMMAND_ROUTER_CMD_BACKWARD;
    }

    if (strcmp(buf, "L") == 0 || strcmp(buf, "l") == 0) {
        return COMMAND_ROUTER_CMD_LEFT;
    }

    if (strcmp(buf, "R") == 0 || strcmp(buf, "r") == 0) {
        return COMMAND_ROUTER_CMD_RIGHT;
    }

    if (strcmp(buf, "S") == 0 || strcmp(buf, "s") == 0) {
        return COMMAND_ROUTER_CMD_STOP;
    }

    if (strcmp(buf, "P") == 0 || strcmp(buf, "p") == 0) {
        return COMMAND_ROUTER_CMD_PATROL;
    }

    if (strcmp(buf, "H") == 0 || strcmp(buf, "h") == 0) {
        return COMMAND_ROUTER_CMD_HAND_UP;
    }

    if (strcmp(buf, "D") == 0 || strcmp(buf, "d") == 0) {
        return COMMAND_ROUTER_CMD_HAND_DOWN;
    }

    if (strcmp(buf, "W") == 0 || strcmp(buf, "w") == 0) {
        return COMMAND_ROUTER_CMD_DANCE;
    }

    /*
     * 英文命令。
     */
    ascii_to_lower_inplace(buf);

    if (strcmp(buf, "forward") == 0 ||
        strcmp(buf, "go") == 0 ||
        strcmp(buf, "front") == 0) {
        return COMMAND_ROUTER_CMD_FORWARD;
    }

    if (strcmp(buf, "backward") == 0 ||
        strcmp(buf, "back") == 0 ||
        strcmp(buf, "reverse") == 0) {
        return COMMAND_ROUTER_CMD_BACKWARD;
    }

    if (strcmp(buf, "left") == 0) {
        return COMMAND_ROUTER_CMD_LEFT;
    }

    if (strcmp(buf, "right") == 0) {
        return COMMAND_ROUTER_CMD_RIGHT;
    }

    if (strcmp(buf, "stop") == 0 ||
        strcmp(buf, "halt") == 0) {
        return COMMAND_ROUTER_CMD_STOP;
    }

    if (strcmp(buf, "patrol") == 0 ||
        strcmp(buf, "auto") == 0) {
        return COMMAND_ROUTER_CMD_PATROL;
    }

    if (strcmp(buf, "handup") == 0 ||
        strcmp(buf, "hand_up") == 0 ||
        strcmp(buf, "raisehand") == 0 ||
        strcmp(buf, "raise_hand") == 0 ||
        strcmp(buf, "uphand") == 0) {
        return COMMAND_ROUTER_CMD_HAND_UP;
    }

    if (strcmp(buf, "handdown") == 0 ||
        strcmp(buf, "hand_down") == 0 ||
        strcmp(buf, "putdown") == 0 ||
        strcmp(buf, "put_down") == 0 ||
        strcmp(buf, "downhand") == 0) {
        return COMMAND_ROUTER_CMD_HAND_DOWN;
    }

    if (strcmp(buf, "dance") == 0 ||
        strcmp(buf, "dancing") == 0) {
        return COMMAND_ROUTER_CMD_DANCE;
    }

    /*
     * 中文命令也支持一下。
     */
    if (text_has(text, "停止") ||
        text_has(text, "停下") ||
        text_has(text, "停车")) {
        return COMMAND_ROUTER_CMD_STOP;
    }

    if (text_has(text, "举手") ||
        text_has(text, "抬手") ||
        text_has(text, "举起手") ||
        text_has(text, "手举起来")) {
        return COMMAND_ROUTER_CMD_HAND_UP;
    }

    if (text_has(text, "放下") ||
        text_has(text, "放手") ||
        text_has(text, "手放下") ||
        text_has(text, "手臂放下")) {
        return COMMAND_ROUTER_CMD_HAND_DOWN;
    }

    if (text_has(text, "跳舞") ||
        text_has(text, "跳个舞") ||
        text_has(text, "舞蹈") ||
        text_has(text, "扭一扭")) {
        return COMMAND_ROUTER_CMD_DANCE;
    }

    if (text_has(text, "前进") ||
        text_has(text, "向前") ||
        text_has(text, "往前")) {
        return COMMAND_ROUTER_CMD_FORWARD;
    }

    if (text_has(text, "后退") ||
        text_has(text, "倒车") ||
        text_has(text, "向后")) {
        return COMMAND_ROUTER_CMD_BACKWARD;
    }

    if (text_has(text, "左转") ||
        text_has(text, "向左") ||
        text_has(text, "往左")) {
        return COMMAND_ROUTER_CMD_LEFT;
    }

    if (text_has(text, "右转") ||
        text_has(text, "向右") ||
        text_has(text, "往右")) {
        return COMMAND_ROUTER_CMD_RIGHT;
    }

    if (text_has(text, "巡逻") ||
        text_has(text, "巡航")) {
        return COMMAND_ROUTER_CMD_PATROL;
    }

    return COMMAND_ROUTER_CMD_NONE;
}

bool command_router_handle_voice_text(const char *text)
{
    if (str_is_empty(text)) {
        return false;
    }

    command_router_cmd_t cmd = command_router_parse_voice_text(text);

    if (cmd == COMMAND_ROUTER_CMD_NONE) {
        ESP_LOGI(TAG, "voice text is not command: %s", text);
        return false;
    }

    ESP_LOGI(TAG,
             "voice command detected: %s -> %s",
             text,
             command_router_cmd_to_code(cmd));

    esp_err_t ret = command_router_send_cmd(cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "send voice command failed: %s", esp_err_to_name(ret));
        return false;
    }

    return true;
}

bool command_router_handle_remote_text(const char *text)
{
    if (str_is_empty(text)) {
        return false;
    }

    command_router_cmd_t cmd = command_router_parse_remote_text(text);

    if (cmd == COMMAND_ROUTER_CMD_NONE) {
        ESP_LOGW(TAG, "unknown remote command: %s", text);
        lcd_status_post("BAD CMD");
        return false;
    }

    ESP_LOGI(TAG,
             "remote command detected: %s -> %s",
             text,
             command_router_cmd_to_code(cmd));

    esp_err_t ret = command_router_send_cmd(cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "send remote command failed: %s", esp_err_to_name(ret));
        return false;
    }

    return true;
}

bool command_router_handle_remote_bytes(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return false;
    }

    char buf[CMD_TEXT_MAX_LEN] = {0};

    size_t copy_len = len;

    if (copy_len >= sizeof(buf)) {
        copy_len = sizeof(buf) - 1;
    }

    memcpy(buf, data, copy_len);
    buf[copy_len] = '\0';

    trim_ascii_inplace(buf);

    return command_router_handle_remote_text(buf);
}