// SPDX-License-Identifier: Apache-2.0
// How wifi_scan() runs a scan: a pure policy over three operations, so it can be tested on a host.
//
// The driver refuses a scan, or aborts one, while a connect attempt is in flight. That happens
// when the stored network is out of range and is being retried every few seconds. Such a scan
// must not be reported as "no networks": the attempt in flight is settled first, a refused or
// aborted scan is tried again, and if every try fails the caller gets an error to show.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define WIFI_SCAN_TRIES 3

typedef struct {
    // Cancels a connect attempt in flight and waits until the station is idle.
    void (*settle)(void *ctx);
    // Runs one scan to completion. ESP_ERR_WIFI_STATE means the driver refused it.
    esp_err_t (*run)(void *ctx);
    // Whether the scan that just completed was aborted (its done event reported a failure).
    bool (*aborted)(void *ctx);
    void *ctx;
} wifi_scan_ops_t;

// Returns ESP_OK once a scan completes; its results are then in the driver. Otherwise the error of
// the last try: ESP_ERR_WIFI_STATE if the driver kept refusing, ESP_FAIL if scans kept aborting,
// or any other error at once.
static inline esp_err_t wifi_scan_with_retry(const wifi_scan_ops_t *ops, bool reconnecting)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < WIFI_SCAN_TRIES; ++i) {
        if (reconnecting || i > 0) {
            ops->settle(ops->ctx);
        }
        err = ops->run(ops->ctx);
        if (err == ESP_OK) {
            if (!ops->aborted(ops->ctx)) {
                return ESP_OK;
            }
            err = ESP_FAIL;
        } else if (err != ESP_ERR_WIFI_STATE) {
            return err;
        }
    }
    return err;
}
