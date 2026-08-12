#pragma once

#include <stdint.h>
#include <stddef.h>

#define REMOTE_MSG_MAX_LEN 64

typedef struct {
    uint8_t data[REMOTE_MSG_MAX_LEN];
    size_t len;
} remote_msg_t;