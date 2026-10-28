// SPDX-License-Identifier: Apache-2.0
#include "serial_console.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_audio.h"
#include "connector.h"
#include "device_config.h"
#include "speaker_volume.h"
#include "wifi.h"
#include "wifi_switch.h"

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"

#define LINE_MAX_BYTES 320  // "comma wifi join" + 2 x 32 hex SSID bytes + 2 x 64 hex password bytes

static const char *TAG = "console";

typedef enum { REQ_NONE = 0, REQ_JOIN, REQ_STATUS, REQ_VOLUME } request_t;

// Written by the reader task while s_request is REQ_NONE, read and cleared by the UI task.
static atomic_int s_request = REQ_NONE;
static char s_ssid[33];
static char s_password[65];
static bool s_watching;  // a switch started from here, not yet reported
static int s_volume_request;  // REQ_VOLUME: the level to set, or 0 to report it

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) {
        *v++ = 0;
    }
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Decode hex into out (NUL-terminated). False if it is not hex or does not fit.
static bool unhex(const char *hex, size_t hex_len, char *out, size_t out_size)
{
    if (hex_len % 2 || hex_len / 2 >= out_size) {
        return false;
    }
    for (size_t i = 0; i < hex_len / 2; ++i) {
        const int hi = hex_value(hex[2 * i]), lo = hex_value(hex[2 * i + 1]);
        if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) {
            return false;
        }
        out[i] = (char)(hi << 4 | lo);
    }
    out[hex_len / 2] = '\0';
    return true;
}

static void handle_line(char *line)
{
    static const char JOIN[] = "comma wifi join ";
    static const char STATUS[] = "comma wifi status";
    static const char VOLUME[] = "comma volume";
    if (strcmp(line, STATUS) == 0) {
        if (s_request == REQ_NONE) s_request = REQ_STATUS;
        return;
    }
    if (strncmp(line, VOLUME, sizeof(VOLUME) - 1) == 0) {
        const char *arg = line + sizeof(VOLUME) - 1;
        int level = 0;
        if (*arg == ' ') {
            char *end = NULL;
            level = (int)strtol(arg + 1, &end, 10);
            if (end == arg + 1 || *end || level < SPEAKER_VOLUME_MIN || level > SPEAKER_VOLUME_MAX) {
                ESP_LOGW(TAG, "volume refused: expected %d-%d", SPEAKER_VOLUME_MIN, SPEAKER_VOLUME_MAX);
                return;
            }
        } else if (*arg) {
            return;
        }
        if (s_request == REQ_NONE) {
            s_volume_request = level;
            s_request = REQ_VOLUME;
        }
        return;
    }
    if (strncmp(line, JOIN, sizeof(JOIN) - 1) != 0) {
        return;  // not for us; the line is not repeated, it may hold anything
    }
    if (s_request != REQ_NONE) {
        ESP_LOGW(TAG, "wifi-join refused: busy");
        return;
    }
    char *ssid_hex = line + sizeof(JOIN) - 1;
    char *pass_hex = strchr(ssid_hex, ' ');
    const size_t ssid_len = pass_hex ? (size_t)(pass_hex - ssid_hex) : strlen(ssid_hex);
    if (pass_hex) {
        ++pass_hex;
    }
    const bool ok = unhex(ssid_hex, ssid_len, s_ssid, sizeof(s_ssid)) && s_ssid[0] &&
                    (!pass_hex || unhex(pass_hex, strlen(pass_hex), s_password, sizeof(s_password)));
    if (!ok) {
        wipe(s_ssid, sizeof(s_ssid));
        wipe(s_password, sizeof(s_password));
        ESP_LOGW(TAG, "wifi-join refused: expected <ssid hex> [<password hex>]");
        return;
    }
    s_request = REQ_JOIN;
}

static void reader_task(void *arg)
{
    (void)arg;
    char line[LINE_MAX_BYTES + 1];
    size_t n = 0;
    bool overflow = false;
    for (;;) {
        uint8_t buf[64];
        const int got = usb_serial_jtag_read_bytes(buf, sizeof(buf), portMAX_DELAY);
        for (int i = 0; i < got; ++i) {
            const char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (n && !overflow) {
                    line[n] = '\0';
                    handle_line(line);
                }
                wipe(line, sizeof(line));
                n = 0;
                overflow = false;
            } else if (n < LINE_MAX_BYTES) {
                line[n++] = c;
            } else {
                overflow = true;
            }
        }
        wipe(buf, sizeof(buf));
    }
}

void serial_console_start(void)
{
    usb_serial_jtag_driver_config_t cfg = {.rx_buffer_size = 512, .tx_buffer_size = 256};
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB serial driver: %s", esp_err_to_name(err));
        return;
    }
    if (xTaskCreate(reader_task, "console", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "console task create failed");
        return;
    }
    ESP_LOGI(TAG, "ready: comma wifi status | comma wifi join <ssid hex> [<password hex>] | comma volume [1-10]");
}

// Eight hex digits of SHA-256 over the Comma credentials: shows whether they changed, reveals nothing.
static void credential_fingerprint(char *out, size_t len)
{
    snprintf(out, len, "none");
    device_config_t *cfg = calloc(1, sizeof(*cfg));
    if (cfg && device_config_get(cfg) && psa_crypto_init() == PSA_SUCCESS) {
        uint8_t hash[32];
        size_t hash_len = 0;
        // Everything but Wi-Fi: what a switch must leave alone.
        const size_t offset = offsetof(device_config_t, server);
        if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)cfg + offset, sizeof(*cfg) - offset, hash, sizeof(hash),
                             &hash_len) == PSA_SUCCESS) {
            snprintf(out, len, "%02x%02x%02x%02x", hash[0], hash[1], hash[2], hash[3]);
        }
    }
    if (cfg) {
        wipe(cfg, sizeof(*cfg));
        free(cfg);
    }
}

static void report_status(void)
{
    char ssid[33] = "", ip[16] = "", stored[33] = "", fingerprint[12];
    int8_t rssi = 0;
    const bool up = wifi_link_info(ssid, sizeof(ssid), &rssi);
    wifi_ip_string(ip, sizeof(ip));
    device_config_t *cfg = calloc(1, sizeof(*cfg));
    if (cfg && device_config_get(cfg)) {
        strlcpy(stored, cfg->wifi_ssid, sizeof(stored));
    }
    if (cfg) {
        wipe(cfg, sizeof(*cfg));
        free(cfg);
    }
    credential_fingerprint(fingerprint, sizeof(fingerprint));
    ESP_LOGI(TAG, "wifi-status up=%d ssid=\"%s\" rssi=%d ip=%s stored=\"%s\" off=%d comma=%s credentials=%s", up, ssid,
             up ? rssi : 0, ip[0] ? ip : "-", stored, wifi_is_off(), connector_online() ? "online" : "offline",
             fingerprint);
}

void serial_console_poll(void)
{
    const request_t request = (request_t)s_request;
    if (request == REQ_STATUS) {
        report_status();
        s_request = REQ_NONE;
    } else if (request == REQ_VOLUME) {
        if (s_volume_request) {
            board_audio_set_volume(s_volume_request);
            board_audio_preview_volume();  // refused during a call, where the new level is heard
        }
        const speaker_volume_t gain = speaker_volume_gain(board_audio_volume());
        ESP_LOGI(TAG, "volume level=%d dac=%d dB boost=+%d dB (clean up to %d, max %d)", board_audio_volume(),
                 gain.dac_db, gain.boost_db, SPEAKER_VOLUME_CLEAN_MAX, SPEAKER_VOLUME_MAX);
        s_request = REQ_NONE;
    } else if (request == REQ_JOIN) {
        char fingerprint[12];
        credential_fingerprint(fingerprint, sizeof(fingerprint));
        ESP_LOGI(TAG, "wifi-join \"%s\" requested; credentials=%s", s_ssid, fingerprint);
        const esp_err_t err = wifi_switch_start(s_ssid, s_password);
        wipe(s_password, sizeof(s_password));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi-join refused: %s", esp_err_to_name(err));
        }
        s_watching = err == ESP_OK;
        s_request = REQ_NONE;
    }
    if (!s_watching) {
        return;
    }
    const wifi_switch_state_t state = wifi_switch_poll();
    if (state == WIFI_SWITCH_TRYING) {
        return;
    }
    s_watching = false;
    if (state == WIFI_SWITCH_IDLE) {
        ESP_LOGI(TAG, "wifi-join \"%s\" ended; its result was dismissed on the screen", s_ssid);
    } else if (state == WIFI_SWITCH_SAVED) {
        ESP_LOGI(TAG, "wifi-join joined \"%s\"; Comma credentials unchanged", s_ssid);
    } else {
        const wifi_result_t why = wifi_switch_failure();
        ESP_LOGW(TAG, "wifi-join not joined \"%s\": %s; back on the stored network", s_ssid,
                 why == WIFI_RESULT_WRONG_PASSWORD ? "password rejected or signal too weak"
                 : why == WIFI_RESULT_NOT_FOUND    ? "network not found"
                 : why == WIFI_RESULT_IDLE         ? "cancelled"
                                                   : "no answer from the network");
    }
    report_status();
}
