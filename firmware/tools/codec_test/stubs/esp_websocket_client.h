// SPDX-License-Identifier: Apache-2.0
#pragma once
// The fields of esp_websocket_client's data event that ws_frame.c reads.
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    const char *data_ptr;
    int data_len;
    bool fin;
    uint8_t op_code;
    int payload_len;
    int payload_offset;
} esp_websocket_event_data_t;
