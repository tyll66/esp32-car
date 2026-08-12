#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    COMMAND_ROUTER_CMD_NONE = 0,

    COMMAND_ROUTER_CMD_STOP,
    COMMAND_ROUTER_CMD_FORWARD,
    COMMAND_ROUTER_CMD_BACKWARD,
    COMMAND_ROUTER_CMD_LEFT,
    COMMAND_ROUTER_CMD_RIGHT,
    COMMAND_ROUTER_CMD_PATROL,

    COMMAND_ROUTER_CMD_HAND_UP,
    COMMAND_ROUTER_CMD_HAND_DOWN,
    COMMAND_ROUTER_CMD_DANCE,
    COMMAND_ROUTER_CMD_RD,
    COMMAND_ROUTER_CMD_RO,
    COMMAND_ROUTER_CMD_LD,
    COMMAND_ROUTER_CMD_LO,
} command_router_cmd_t;

const char *command_router_cmd_to_code(command_router_cmd_t cmd);

command_router_cmd_t command_router_parse_voice_text(const char *text);
command_router_cmd_t command_router_parse_remote_text(const char *text);

esp_err_t command_router_send_cmd(command_router_cmd_t cmd);

bool command_router_handle_voice_text(const char *text);
bool command_router_handle_remote_text(const char *text);
bool command_router_handle_remote_bytes(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif