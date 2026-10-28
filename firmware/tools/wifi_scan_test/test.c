// SPDX-License-Identifier: Apache-2.0
// The scan policy behind wifi_scan() (main/wifi_scan_retry.h) against a fake driver: a connect
// attempt in flight is settled before the scan, a refused or aborted scan is tried again, and a
// scan that never completes is an error, never an empty list.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#define ESP_ERR_WIFI_STATE 0x3007
#define ESP_ERR_TIMEOUT 0x107
#include "wifi_scan_retry.h"

static int checks;
#define CHECK(c) do { ++checks; assert(c); } while (0)

// The fake driver: a connect attempt in flight refuses or aborts every scan until it is settled.
typedef struct {
    bool connecting;     // an attempt is in flight
    bool refuse;         // a scan meanwhile is refused (ESP_ERR_WIFI_STATE) rather than aborted
    bool reconnects;     // a pending reconnect starts a new attempt right after each settle
    esp_err_t hard;      // when set, every scan fails with this error
    int settles, runs, completed;
} fake_t;

static void settle(void *ctx)
{
    fake_t *f = ctx;
    ++f->settles;
    f->connecting = f->reconnects;
}

static bool aborted_last;

static esp_err_t run(void *ctx)
{
    fake_t *f = ctx;
    ++f->runs;
    aborted_last = false;
    if (f->hard != ESP_OK) {
        return f->hard;
    }
    if (f->connecting) {
        if (f->refuse) {
            return ESP_ERR_WIFI_STATE;
        }
        aborted_last = true;
        return ESP_OK;
    }
    ++f->completed;
    return ESP_OK;
}

static bool aborted(void *ctx)
{
    (void)ctx;
    return aborted_last;
}

static esp_err_t scan(fake_t *f, bool reconnecting)
{
    const wifi_scan_ops_t ops = {.settle = settle, .run = run, .aborted = aborted, .ctx = f};
    return wifi_scan_with_retry(&ops, reconnecting);
}

int main(void)
{
    // Connected: one scan, nothing to settle.
    fake_t f = {0};
    CHECK(scan(&f, false) == ESP_OK && f.settles == 0 && f.runs == 1 && f.completed == 1);

    // The stored network is out of range and being retried: the attempt in flight is settled
    // first, so the first scan completes. This is the case that showed "No networks" before.
    f = (fake_t){.connecting = true};
    CHECK(scan(&f, true) == ESP_OK && f.settles == 1 && f.runs == 1 && f.completed == 1);
    f = (fake_t){.connecting = true, .refuse = true};
    CHECK(scan(&f, true) == ESP_OK && f.settles == 1 && f.completed == 1);

    // An attempt the caller did not know about aborts the first scan; the retry settles it.
    f = (fake_t){.connecting = true};
    CHECK(scan(&f, false) == ESP_OK && f.runs == 2 && f.settles == 1 && f.completed == 1);
    // Refused instead of aborted: the same.
    f = (fake_t){.connecting = true, .refuse = true};
    CHECK(scan(&f, false) == ESP_OK && f.runs == 2 && f.completed == 1);

    // Attempts that keep starting again: every try fails, and the caller gets an error to show,
    // never ESP_OK with an empty list.
    f = (fake_t){.connecting = true, .reconnects = true};
    CHECK(scan(&f, true) == ESP_FAIL && f.runs == WIFI_SCAN_TRIES && f.completed == 0);
    f = (fake_t){.connecting = true, .reconnects = true, .refuse = true};
    CHECK(scan(&f, true) == ESP_ERR_WIFI_STATE && f.runs == WIFI_SCAN_TRIES && f.completed == 0);

    // Any other error is returned at once, without retrying.
    f = (fake_t){.hard = ESP_ERR_TIMEOUT};
    CHECK(scan(&f, false) == ESP_ERR_TIMEOUT && f.runs == 1);
    f = (fake_t){.hard = ESP_ERR_NO_MEM};
    CHECK(scan(&f, true) == ESP_ERR_NO_MEM && f.runs == 1 && f.settles == 1);

    printf("wifi_scan: %d checks passed\n", checks);
    return 0;
}
