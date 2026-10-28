// SPDX-License-Identifier: Apache-2.0
// On-screen settings (settings.h). Each page is a ui_view_t, rebuilt on every pass of the UI loop
// together with its touch targets; display.c redraws only when the view has changed. Everything runs
// in the UI task except the Wi-Fi scan, which blocks for a few seconds and gets a short-lived task.
// A password typed here stays in one buffer that is wiped when the trial starts, on Cancel and on
// close. It is never logged, and drawn only while "Show" is on.

#include "settings.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_audio.h"
#include "connector.h"
#include "device_config.h"
#include "display.h"
#include "memory_caps.h"
#include "provisioning.h"
#include "speaker_volume.h"
#include "ui_view.h"
#include "wifi.h"
#include "wifi_switch.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Closed (back to the logo) after this long without a touch, except while a network is joined.
#define IDLE_CLOSE_US (60LL * 1000000)
// "Turn Wi-Fi off" needs a second tap within this time.
#define CONFIRM_US (4LL * 1000000)
#define HINT_US (3LL * 1000000)
// A key that was hit lights up, with the character large above it, for this long.
#define KEY_FLASH_US (350LL * 1000)
#define LINK_REFRESH_US (2LL * 1000000)
#define SCAN_MAX 16
#define ROWS_PER_PAGE 4
#define PASSWORD_MAX 64
// A tap this close outside a target still hits it; the nearest target wins.
#define HIT_SLOP 6

// Gray levels (0-15).
#define L_TITLE 10
#define L_TEXT 15
#define L_DIM 8
#define L_BOX 2
#define L_BUTTON 3
#define L_STRONG 5  // the main action, the current network, a pending confirmation

static const char *TAG = "settings";

typedef enum {
    PAGE_MAIN,
    PAGE_WIFI,
    PAGE_NETWORKS,
    PAGE_PASSWORD,
    PAGE_JOINING,
    PAGE_JOINED,
    PAGE_BLUETOOTH,
    PAGE_VOLUME,
} page_t;

typedef enum { KB_LOWER, KB_UPPER, KB_SYMBOLS } keyboard_t;
typedef enum { SCAN_IDLE, SCAN_RUNNING, SCAN_DONE, SCAN_FAILED } scan_state_t;

typedef enum {
    HIT_WIFI = 1,
    HIT_BLUETOOTH,
    HIT_DONE,
    HIT_BACK,
    HIT_CHOOSE,
    HIT_TOGGLE,
    HIT_NETWORK,  // arg: index into aps
    HIT_UP,
    HIT_DOWN,
    HIT_RESCAN,
    HIT_KEY,      // arg: the character
    HIT_CASE,
    HIT_SYMBOLS,
    HIT_SPACE,
    HIT_SHOW,
    HIT_DELETE,
    HIT_JOIN,
    HIT_CANCEL,
    HIT_PAIR,
    HIT_OK,
    HIT_VOLUME,
    HIT_VOLUME_DOWN,
    HIT_VOLUME_UP,
} hit_id_t;

typedef struct {
    int16_t x, y, w, h;
    uint8_t id;
    uint8_t arg;
} hit_t;

typedef struct {
    ui_view_t view;
    hit_t hits[UI_VIEW_ITEMS];
    int hit_count;
    page_t page;
    int64_t touched_us;
    int64_t confirm_off_until;
    int64_t hint_until;
    // The link, read every LINK_REFRESH_US.
    int64_t link_us;
    bool link_up;
    char link_ssid[33];
    int8_t link_rssi;
    char ip[16];
    char stored[33];    // the stored network's name
    char previous[33];  // the stored network when the last trial started
    // Scan results: written by the scan task while s_scan is SCAN_RUNNING, read once SCAN_DONE.
    wifi_ap_t aps[SCAN_MAX];
    size_t ap_count;
    int ap_first;  // first row of the list on screen
    // The network being joined and its password.
    char chosen[33];
    bool chosen_secure;
    char password[PASSWORD_MAX + 1];
    keyboard_t keyboard;
    bool show_password;
    char flash_key;
    int64_t flash_until;
} settings_t;

// Allocated on first open and kept: a scan task may still write into it after a close.
static settings_t *S;
static bool s_open;
static atomic_int s_scan = SCAN_IDLE;

static const char *const KEY_ROWS[3][4] = {
    {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"},
    {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
    {"!@#$%^&*()", "-_=+[]{}\\|", ";:'\",.<>/?", "`~"},
};

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) {
        *v++ = 0;
    }
}

static void wipe_password(void)
{
    wipe(S->password, sizeof(S->password));
    S->show_password = false;
}

bool settings_active(void)
{
    return s_open;
}

// ---- State ------------------------------------------------------------------------------------

static void load_stored(void)
{
    device_config_t *cfg = calloc(1, sizeof(*cfg));
    S->stored[0] = '\0';
    if (cfg && device_config_get(cfg)) {
        strlcpy(S->stored, cfg->wifi_ssid, sizeof(S->stored));
    }
    if (cfg) {
        wipe(cfg, sizeof(*cfg));
        free(cfg);
    }
}

static void refresh_link(int64_t now, bool force)
{
    if (!force && now - S->link_us < LINK_REFRESH_US) {
        return;
    }
    S->link_us = now;
    S->link_up = wifi_link_info(S->link_ssid, sizeof(S->link_ssid), &S->link_rssi);
    if (!S->link_up) {
        S->link_ssid[0] = '\0';
    }
    wifi_ip_string(S->ip, sizeof(S->ip));
}

static void scan_task(void *arg)
{
    (void)arg;
    size_t n = 0;
    const esp_err_t err = wifi_scan(S->aps, SCAN_MAX, &n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "scan: %u networks", (unsigned)n);
    }
    S->ap_count = err == ESP_OK ? n : 0;
    s_scan = err == ESP_OK ? SCAN_DONE : SCAN_FAILED;
    vTaskDelete(NULL);
}

static void start_scan(void)
{
    if (s_scan == SCAN_RUNNING) {
        return;
    }
    S->ap_first = 0;
    s_scan = SCAN_RUNNING;
    if (xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "scan task create failed");
        s_scan = SCAN_FAILED;
    }
}

static void go(page_t page)
{
    S->page = page;
    S->confirm_off_until = 0;
    S->hint_until = 0;
}

void settings_open(int64_t now)
{
    if (!S) {
        S = heap_caps_calloc(1, sizeof(*S), COMMA_BUFFER_CAPS);
        if (!S) {
            ESP_LOGE(TAG, "no memory for the settings");
            return;
        }
    }
    go(PAGE_MAIN);
    S->touched_us = now;
    wipe_password();
    load_stored();
    refresh_link(now, true);
    s_open = true;
    ESP_LOGI(TAG, "open");
}

void settings_close(void)
{
    if (!s_open) {
        return;
    }
    s_open = false;
    wipe_password();
    wifi_switch_reset();  // a trial still running keeps going; wifi_switch_poll() runs in call.c
    ESP_LOGI(TAG, "closed");
}

static void close_to_logo(void)
{
    settings_close();
    display_show_logo();
}

// ---- Actions ----------------------------------------------------------------------------------

static void back(void)
{
    switch (S->page) {
    case PAGE_MAIN:
        close_to_logo();
        break;
    case PAGE_WIFI:
    case PAGE_BLUETOOTH:
    case PAGE_VOLUME:
        go(PAGE_MAIN);
        break;
    case PAGE_NETWORKS:
        go(PAGE_WIFI);
        break;
    case PAGE_PASSWORD:
        wipe_password();
        go(PAGE_NETWORKS);
        break;
    case PAGE_JOINING:
        wifi_switch_cancel();
        go(PAGE_JOINED);
        break;
    case PAGE_JOINED:
        wifi_switch_reset();
        go(PAGE_WIFI);
        break;
    }
}

static void join(int64_t now)
{
    if (S->chosen_secure && strlen(S->password) < 8) {
        S->hint_until = now + HINT_US;
        return;
    }
    strlcpy(S->previous, S->stored, sizeof(S->previous));
    const esp_err_t err = wifi_switch_start(S->chosen, S->chosen_secure ? S->password : "");
    wipe_password();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "join did not start: %s", esp_err_to_name(err));
    }
    go(err == ESP_OK ? PAGE_JOINING : PAGE_JOINED);
}

static void choose(size_t i, int64_t now)
{
    const wifi_ap_t *ap = &S->aps[i];
    if (S->link_up && strcmp(ap->ssid, S->link_ssid) == 0) {
        go(PAGE_WIFI);  // already on it
        return;
    }
    strlcpy(S->chosen, ap->ssid, sizeof(S->chosen));
    S->chosen_secure = ap->secure;
    wipe_password();
    S->keyboard = KB_LOWER;
    if (!ap->secure) {
        join(now);
    } else {
        go(PAGE_PASSWORD);
    }
}

static void toggle_wifi(int64_t now)
{
    if (wifi_is_off()) {
        device_config_t *cfg = calloc(1, sizeof(*cfg));
        if (cfg && device_config_get(cfg)) {
            ESP_LOGI(TAG, "Wi-Fi on");
            wifi_connect(cfg->wifi_ssid, cfg->wifi_password, true);
        }
        if (cfg) {
            wipe(cfg, sizeof(*cfg));
            free(cfg);
        }
        S->confirm_off_until = 0;
    } else if (now < S->confirm_off_until) {
        ESP_LOGI(TAG, "Wi-Fi off until it is turned on again or the device restarts");
        wifi_turn_off();
        S->confirm_off_until = 0;
    } else {
        S->confirm_off_until = now + CONFIRM_US;
    }
    refresh_link(now, true);
}

static void type(char c)
{
    const size_t len = strlen(S->password);
    if (len < PASSWORD_MAX) {
        S->password[len] = c;
        S->password[len + 1] = '\0';
    }
}

// The network list: the networks found, then a "Scan again" row. ROWS_PER_PAGE rows show at a time,
// from ap_first; a swipe or Up/Down moves them by all but one row.
#define SCROLL_ROWS (ROWS_PER_PAGE - 1)

static int list_rows(void)
{
    return s_scan == SCAN_DONE ? (int)S->ap_count + 1 : 0;
}

static void scroll(int rows)
{
    const int last_first = list_rows() - ROWS_PER_PAGE;
    S->ap_first += rows;
    if (S->ap_first > last_first) {
        S->ap_first = last_first;
    }
    if (S->ap_first < 0) {
        S->ap_first = 0;
    }
}

// One step, stored at once, then a beep at the new level (the settings are not offered during a
// call, so the speaker is free).
static void change_volume(int step)
{
    const int level = board_audio_volume() + step;
    if (level < SPEAKER_VOLUME_MIN || level > SPEAKER_VOLUME_MAX) {
        return;
    }
    board_audio_set_volume(level);
    board_audio_preview_volume();
}

static void on_hit(const hit_t *hit, int64_t now)
{
    switch ((hit_id_t)hit->id) {
    case HIT_WIFI:
        refresh_link(now, true);
        go(PAGE_WIFI);
        break;
    case HIT_BLUETOOTH:
        go(PAGE_BLUETOOTH);
        break;
    case HIT_VOLUME:
        go(PAGE_VOLUME);
        break;
    case HIT_VOLUME_DOWN:
    case HIT_VOLUME_UP:
        change_volume(hit->id == HIT_VOLUME_UP ? 1 : -1);
        break;
    case HIT_DONE:
        close_to_logo();
        break;
    case HIT_BACK:
    case HIT_CANCEL:
    case HIT_OK:
        back();
        break;
    case HIT_CHOOSE:
        start_scan();
        go(PAGE_NETWORKS);
        break;
    case HIT_TOGGLE:
        toggle_wifi(now);
        break;
    case HIT_NETWORK:
        if (s_scan == SCAN_DONE && hit->arg < S->ap_count) {
            choose(hit->arg, now);
        }
        break;
    case HIT_UP:
        scroll(-SCROLL_ROWS);
        break;
    case HIT_DOWN:
        scroll(SCROLL_ROWS);
        break;
    case HIT_RESCAN:
        start_scan();
        break;
    case HIT_KEY:
        type((char)hit->arg);
        S->flash_key = (char)hit->arg;
        S->flash_until = now + KEY_FLASH_US;
        break;
    case HIT_SPACE:
        type(' ');
        break;
    case HIT_CASE:
        S->keyboard = S->keyboard == KB_UPPER ? KB_LOWER : KB_UPPER;
        break;
    case HIT_SYMBOLS:
        S->keyboard = S->keyboard == KB_SYMBOLS ? KB_LOWER : KB_SYMBOLS;
        break;
    case HIT_SHOW:
        S->show_password = !S->show_password;
        break;
    case HIT_DELETE: {
        const size_t len = strlen(S->password);
        if (len) {
            S->password[len - 1] = '\0';
        }
        break;
    }
    case HIT_JOIN:
        join(now);
        break;
    case HIT_PAIR:
        // Same as releasing a 3 s BOOT hold: pairing mode with its passkey, then the app.
        settings_close();
        if (provisioning_start() != ESP_OK) {
            ESP_LOGE(TAG, "pairing mode did not start");
            display_show_logo();
        }
        break;
    }
}

static const hit_t *hit_at(int x, int y)
{
    const hit_t *best = NULL;
    int best_d = 0;
    for (int i = 0; i < S->hit_count; ++i) {
        const hit_t *h = &S->hits[i];
        if (x < h->x - HIT_SLOP || x >= h->x + h->w + HIT_SLOP || y < h->y - HIT_SLOP || y >= h->y + h->h + HIT_SLOP) {
            continue;
        }
        const int d = abs(x - (h->x + h->w / 2)) + abs(y - (h->y + h->h / 2));
        if (!best || d < best_d) {
            best = h;
            best_d = d;
        }
    }
    return best;
}

// ---- Drawing ----------------------------------------------------------------------------------

static void add(int x, int y, int w, int h, uint8_t fill, uint8_t level, uint8_t font, uint8_t align, int radius,
                const char *text)
{
    ui_view_t *v = &S->view;
    if (v->count >= UI_VIEW_ITEMS) {
        return;
    }
    ui_view_item_t *it = &v->items[v->count++];
    it->x = (int16_t)x;
    it->y = (int16_t)y;
    it->w = (int16_t)w;
    it->h = (int16_t)h;
    it->fill = fill;
    it->level = level;
    it->font = font;
    it->align = align;
    it->radius = (uint8_t)radius;
    if (text) {
        strlcpy(it->text, text, sizeof(it->text));
    }
}

static void target(int x, int y, int w, int h, hit_id_t id, uint8_t arg)
{
    if (S->hit_count < UI_VIEW_ITEMS) {
        S->hits[S->hit_count++] = (hit_t){(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (uint8_t)id, arg};
    }
}

// Usable width of rows [y, y + h) of the round glass, with a margin.
static int chord(int y, int h)
{
    const float r = UI_CARD_W / 2.0f;
    const float d = fmaxf(fabsf((float)y - r), fabsf((float)(y + h) - r));
    const int w = d >= r ? 0 : (int)(2.0f * sqrtf(r * r - d * d)) - 24;
    return w > 0 ? w : 0;
}

static void label(int y, int h, uint8_t level, uint8_t font, const char *text)
{
    const int w = chord(y, h);
    add((UI_CARD_W - w) / 2, y, w, h, 0, level, font, UI_VIEW_CENTER, 0, text);
}

static void button(int x, int y, int w, int h, uint8_t fill, const char *text, hit_id_t id, uint8_t arg)
{
    add(x, y, w, h, fill, L_TEXT, UI_VIEW_FONT_MENU, UI_VIEW_CENTER, h / 2, text);
    target(x, y, w, h, id, arg);
}

static bool joining(void)
{
    return wifi_switch_poll() == WIFI_SWITCH_TRYING;
}

static void draw_main(void)
{
    char line[64];
    label(40, 52, L_TITLE, UI_VIEW_FONT_SMALL, "Settings");

    add(58, 96, 350, 84, L_BOX, 0, 0, 0, 28, NULL);
    target(58, 96, 350, 84, HIT_WIFI, 0);
    add(58, 98, 350, 44, 0, L_TEXT, UI_VIEW_FONT_SMALL, UI_VIEW_CENTER, 0, "Wi-Fi");
    if (wifi_is_off()) {
        snprintf(line, sizeof(line), "Off");
    } else if (joining()) {
        snprintf(line, sizeof(line), "Joining %s", S->chosen);
    } else if (S->link_up) {
        snprintf(line, sizeof(line), "%s  %d dBm", S->link_ssid, S->link_rssi);
    } else {
        snprintf(line, sizeof(line), "Connecting to %s", S->stored);
    }
    add(58, 140, 350, 34, 0, L_DIM, UI_VIEW_FONT_MENU, UI_VIEW_CENTER, 0, line);

    add(58, 188, 350, 84, L_BOX, 0, 0, 0, 28, NULL);
    target(58, 188, 350, 84, HIT_BLUETOOTH, 0);
    add(58, 190, 350, 44, 0, L_TEXT, UI_VIEW_FONT_SMALL, UI_VIEW_CENTER, 0, "Bluetooth");
    add(58, 232, 350, 34, 0, L_DIM, UI_VIEW_FONT_MENU, UI_VIEW_CENTER, 0, "Off until pairing mode");

    add(58, 280, 350, 56, L_BOX, 0, 0, 0, 28, NULL);
    target(58, 280, 350, 56, HIT_VOLUME, 0);
    snprintf(line, sizeof(line), "Volume  %d", board_audio_volume());
    add(58, 284, 350, 48, 0, L_TEXT, UI_VIEW_FONT_SMALL, UI_VIEW_CENTER, 0, line);

    label(342, 32, L_DIM, UI_VIEW_FONT_MENU, connector_online() ? "Comma online" : "Comma offline");
    button(153, 380, 160, 50, L_BUTTON, "Done", HIT_DONE, 0);
}

static void draw_wifi(int64_t now)
{
    char line[64];
    const bool off = wifi_is_off();
    label(40, 52, L_TITLE, UI_VIEW_FONT_SMALL, "Wi-Fi");
    add(58, 102, 350, 128, L_BOX, 0, 0, 0, 28, NULL);
    if (off) {
        snprintf(line, sizeof(line), "Off");
    } else if (S->link_up) {
        snprintf(line, sizeof(line), "%s", S->link_ssid);
    } else {
        snprintf(line, sizeof(line), "Not connected");
    }
    add(58, 108, 350, 48, 0, L_TEXT, UI_VIEW_FONT_SMALL, UI_VIEW_CENTER, 0, line);
    if (off) {
        snprintf(line, sizeof(line), "Turn it on to reach Comma");
    } else if (S->link_up) {
        snprintf(line, sizeof(line), "%d dBm  %s", S->link_rssi, S->ip);
    } else {
        snprintf(line, sizeof(line), "Trying %s", S->stored);
    }
    add(58, 154, 350, 34, 0, L_DIM, UI_VIEW_FONT_MENU, UI_VIEW_CENTER, 0, line);
    add(58, 188, 350, 34, 0, L_DIM, UI_VIEW_FONT_MENU, UI_VIEW_CENTER, 0,
        connector_online() ? "Comma online" : "Comma offline");

    button(73, 242, 320, 60, L_BUTTON, "Choose network", HIT_CHOOSE, 0);
    const bool confirming = now < S->confirm_off_until;
    button(73, 310, 320, 60, confirming ? L_STRONG : L_BUTTON,
           off ? "Turn Wi-Fi on" : confirming ? "Tap again to turn off" : "Turn Wi-Fi off", HIT_TOGGLE, 0);
    button(153, 380, 160, 56, L_BUTTON, "Back", HIT_BACK, 0);
}

// Up or Down under the list: dimmed, and no target, at either end.
static void nav_button(int x, int w, const char *text, bool enabled, hit_id_t id)
{
    if (enabled) {
        button(x, 368, w, 56, L_BUTTON, text, id, 0);
    } else {
        add(x, 368, w, 56, L_BOX, L_DIM, UI_VIEW_FONT_MENU, UI_VIEW_CENTER, 28, text);
    }
}

static void draw_networks(void)
{
    char line[64];
    const scan_state_t scan = (scan_state_t)s_scan;
    label(36, 50, L_TITLE, UI_VIEW_FONT_SMALL, "Networks");

    if (scan == SCAN_RUNNING || scan == SCAN_IDLE) {
        label(196, 50, L_DIM, UI_VIEW_FONT_SMALL, "Scanning...");
    } else if (scan == SCAN_FAILED || S->ap_count == 0) {
        label(150, 50, L_TEXT, UI_VIEW_FONT_SMALL, scan == SCAN_FAILED ? "Scan failed" : "No networks");
        label(202, 36, L_DIM, UI_VIEW_FONT_MENU, "Only 2.4 GHz networks show");
        button(73, 256, 320, 60, L_BUTTON, "Scan again", HIT_RESCAN, 0);
    } else {
        scroll(0);  // the list may have shrunk since the last scan
        const int rows = list_rows();
        for (int k = 0; k < ROWS_PER_PAGE; ++k) {
            const int i = S->ap_first + k;
            const int y = 96 + k * 66;
            if (i >= rows) {
                break;
            }
            if (i == (int)S->ap_count) {
                button(58, y, 350, 58, L_BUTTON, "Scan again", HIT_RESCAN, 0);
                continue;
            }
            const wifi_ap_t *ap = &S->aps[i];
            const bool current = S->link_up && strcmp(ap->ssid, S->link_ssid) == 0;
            add(58, y, 350, 58, current ? L_STRONG : L_BOX, 0, 0, 0, 29, NULL);
            // Name (226 px; an open network says so) and signal (66 px) side by side.
            snprintf(line, sizeof(line), ap->secure ? "%s" : "%s (open)", ap->ssid);
            add(58, y, 250, 58, 0, L_TEXT, UI_VIEW_FONT_MENU, UI_VIEW_LEFT, 0, line);
            snprintf(line, sizeof(line), "%d", ap->rssi);
            add(318, y, 90, 58, 0, L_DIM, UI_VIEW_FONT_MENU, UI_VIEW_RIGHT, 0, line);
            target(58, y, 350, 58, HIT_NETWORK, (uint8_t)i);
        }
        if (rows > ROWS_PER_PAGE) {
            // Scroll bar beside the rows: where the visible rows are in the whole list.
            const int track_y = 100, track_h = ROWS_PER_PAGE * 66 - 16;
            const int thumb_h = track_h * ROWS_PER_PAGE / rows;
            const int thumb_y = track_y + (track_h - thumb_h) * S->ap_first / (rows - ROWS_PER_PAGE);
            add(414, track_y, 6, track_h, L_BOX, 0, 0, 0, 3, NULL);
            add(414, thumb_y, 6, thumb_h, L_DIM, 0, 0, 0, 3, NULL);
        }
    }
    const bool listed = scan == SCAN_DONE && S->ap_count > 0;
    nav_button(96, 84, "Up", listed && S->ap_first > 0, HIT_UP);
    button(186, 368, 94, 56, L_BUTTON, "Back", HIT_BACK, 0);
    nav_button(286, 84, "Down", listed && S->ap_first + ROWS_PER_PAGE < list_rows(), HIT_DOWN);
}

// What the password field shows: the end of what was typed when it is too long to fit.
static void password_field(char *out, size_t len)
{
    char shown[PASSWORD_MAX + 1];
    const size_t n = strlen(S->password);
    if (S->show_password) {
        strlcpy(shown, S->password, sizeof(shown));
    } else {
        memset(shown, '*', n);
        shown[n] = '\0';
    }
    const int room = 300 - 2 * 20 - ui_view_text_width(UI_VIEW_FONT_MENU, "...");
    size_t start = 0;
    while (start < n && ui_view_text_width(UI_VIEW_FONT_MENU, shown + start) > room) {
        ++start;
    }
    snprintf(out, len, "%s%s", start ? "..." : "", shown + start);
    wipe(shown, sizeof(shown));
}

static void draw_password(int64_t now)
{
    char field[UI_VIEW_TEXT + 8];
    uint8_t level = L_TEXT;
    if (now < S->hint_until) {
        snprintf(field, sizeof(field), "At least 8 characters");
    } else if (!S->password[0]) {
        snprintf(field, sizeof(field), "Password");
        level = L_DIM;
    } else {
        password_field(field, sizeof(field));
    }
    add(83, 46, 300, 58, L_BOX, level, UI_VIEW_FONT_MENU, UI_VIEW_CENTER, 29, field);
    wipe(field, sizeof(field));

    const bool flashing = now < S->flash_until;
    int flash_x = -1, flash_y = 0;
    for (int r = 0; r < 4; ++r) {
        const char *row = KEY_ROWS[S->keyboard][r];
        const int n = (int)strlen(row);
        const int x0 = (UI_CARD_W - (n * 40 - 2)) / 2;
        const int y = 112 + r * 50;
        for (int c = 0; c < n; ++c) {
            const char key[2] = {row[c], '\0'};
            const bool lit = flashing && row[c] == S->flash_key;
            add(x0 + c * 40, y, 38, 46, lit ? L_STRONG : L_BUTTON, L_TEXT, UI_VIEW_FONT_MENU, UI_VIEW_CENTER, 10, key);
            target(x0 + c * 40, y, 38, 46, HIT_KEY, (uint8_t)row[c]);
            if (lit) {
                flash_x = x0 + c * 40;
                flash_y = y;
            }
        }
    }
    // Shift, symbols, space, show and delete: 374 px, inside the glass at this row.
    button(46, 316, 58, 48, S->keyboard == KB_UPPER ? L_STRONG : L_BUTTON, "Aa", HIT_CASE, 0);
    button(109, 316, 58, 48, S->keyboard == KB_SYMBOLS ? L_STRONG : L_BUTTON, "#+", HIT_SYMBOLS, 0);
    button(172, 316, 92, 48, L_BUTTON, "space", HIT_SPACE, 0);
    button(269, 316, 86, 48, L_BUTTON, S->show_password ? "Hide" : "Show", HIT_SHOW, 0);
    button(360, 316, 60, 48, L_BUTTON, "Del", HIT_DELETE, 0);
    button(108, 372, 120, 50, L_BUTTON, "Cancel", HIT_CANCEL, 0);
    button(238, 372, 120, 50, L_STRONG, "Join", HIT_JOIN, 0);
    if (flash_x >= 0) {
        // The key just hit, enlarged above it (drawn last, over its neighbours), as phones do.
        // Kept inside the glass: near the top the rows are narrow.
        const int by = flash_y - 60;
        const int margin = (UI_CARD_W - (chord(by, 58) + 24)) / 2;
        int bx = flash_x - 13;
        if (bx < margin) bx = margin;
        if (bx > UI_CARD_W - margin - 64) bx = UI_CARD_W - margin - 64;
        const char key[2] = {S->flash_key, '\0'};
        add(bx, by, 64, 58, L_STRONG, L_TEXT, UI_VIEW_FONT_SMALL, UI_VIEW_CENTER, 16, key);
    }
}

static void draw_joining(void)
{
    label(118, 50, L_DIM, UI_VIEW_FONT_SMALL, "Joining");
    label(170, 50, L_TEXT, UI_VIEW_FONT_SMALL, S->chosen);
    label(236, 36, L_DIM, UI_VIEW_FONT_MENU, "If it does not work, the");
    label(270, 36, L_DIM, UI_VIEW_FONT_MENU, "current network comes back");
    button(153, 372, 160, 56, L_BUTTON, "Cancel", HIT_CANCEL, 0);
}

static void draw_joined(void)
{
    char line[64];
    const char *comma = connector_online() ? "Comma online" : "Connecting to Comma...";
    if (wifi_switch_poll() == WIFI_SWITCH_SAVED) {
        label(110, 50, L_TEXT, UI_VIEW_FONT_SMALL, "Connected");
        label(162, 50, L_TEXT, UI_VIEW_FONT_SMALL, S->chosen);
        label(224, 36, L_DIM, UI_VIEW_FONT_MENU, "Comma setup kept");
        label(258, 36, L_DIM, UI_VIEW_FONT_MENU, comma);
    } else {
        const wifi_result_t why = wifi_switch_failure();
        const char *a = "No answer from", *b = "the network";
        if (why == WIFI_RESULT_IDLE) {
            a = "Cancelled";
            b = "";
        } else if (why == WIFI_RESULT_WRONG_PASSWORD) {
            a = "Password rejected, or";
            b = "the signal is too weak";
        } else if (why == WIFI_RESULT_NOT_FOUND) {
            a = "Network not found";
            b = "";
        }
        label(96, 50, L_TEXT, UI_VIEW_FONT_SMALL, "Not joined");
        label(148, 50, L_TEXT, UI_VIEW_FONT_SMALL, S->chosen);
        label(206, 36, L_DIM, UI_VIEW_FONT_MENU, a);
        label(240, 36, L_DIM, UI_VIEW_FONT_MENU, b);
        if (S->link_up) {
            snprintf(line, sizeof(line), "Back on %s", S->link_ssid);
        } else {
            snprintf(line, sizeof(line), "Going back to %s", S->previous);
        }
        label(282, 36, L_TEXT, UI_VIEW_FONT_MENU, line);
        label(316, 36, L_DIM, UI_VIEW_FONT_MENU, comma);
    }
    button(153, 372, 160, 56, L_STRONG, "OK", HIT_OK, 0);
}

static void draw_bluetooth(void)
{
    label(40, 52, L_TITLE, UI_VIEW_FONT_SMALL, "Bluetooth");
    add(58, 102, 350, 140, L_BOX, 0, 0, 0, 28, NULL);
    add(58, 110, 350, 48, 0, L_TEXT, UI_VIEW_FONT_SMALL, UI_VIEW_CENTER, 0, "Off");
    add(58, 160, 350, 34, 0, L_DIM, UI_VIEW_FONT_MENU, UI_VIEW_CENTER, 0, "On only in pairing mode,");
    add(58, 194, 350, 34, 0, L_DIM, UI_VIEW_FONT_MENU, UI_VIEW_CENTER, 0, "to set up with the app");
    button(73, 258, 320, 60, L_STRONG, "Start pairing mode", HIT_PAIR, 0);
    char line[48];
    char name[24];
    provisioning_ble_name(name, sizeof(name));
    snprintf(line, sizeof(line), "Shows as %s", name);
    label(326, 36, L_DIM, UI_VIEW_FONT_MENU, line);
    button(153, 380, 160, 56, L_BUTTON, "Back", HIT_BACK, 0);
}

static void draw_volume(void)
{
    const int level = board_audio_volume();
    const speaker_volume_t gain = speaker_volume_gain(level);
    char line[32];
    label(40, 52, L_TITLE, UI_VIEW_FONT_SMALL, "Volume");

    add(58, 100, 350, 112, L_BOX, 0, 0, 0, 28, NULL);
    snprintf(line, sizeof(line), "%d of %d", level, SPEAKER_VOLUME_MAX);
    add(58, 108, 350, 48, 0, L_TEXT, UI_VIEW_FONT_SMALL, UI_VIEW_CENTER, 0, line);
    if (gain.boost_db > 0) {
        snprintf(line, sizeof(line), "Boost +%d dB, may distort", gain.boost_db);
    } else if (level == SPEAKER_VOLUME_CLEAN_MAX) {
        snprintf(line, sizeof(line), "Loudest without boost");
    } else {
        snprintf(line, sizeof(line), "%d dB", gain.dac_db);
    }
    add(58, 160, 350, 34, 0, L_DIM, UI_VIEW_FONT_MENU, UI_VIEW_CENTER, 0, line);

    // One segment per level; the boost levels are set apart by a brighter fill.
    for (int i = 0; i < SPEAKER_VOLUME_MAX; ++i) {
        const bool on = i < level;
        const uint8_t fill = !on ? L_BOX : i >= SPEAKER_VOLUME_CLEAN_MAX ? L_TEXT : L_TITLE;
        add(84 + i * 30, 224, 26, 24, fill, 0, 0, 0, 6, NULL);
    }

    if (level > SPEAKER_VOLUME_MIN) {
        button(73, 262, 150, 64, L_BUTTON, "-", HIT_VOLUME_DOWN, 0);
    } else {
        add(73, 262, 150, 64, L_BOX, L_DIM, UI_VIEW_FONT_MENU, UI_VIEW_CENTER, 32, "-");
    }
    if (level < SPEAKER_VOLUME_MAX) {
        button(243, 262, 150, 64, L_BUTTON, "+", HIT_VOLUME_UP, 0);
    } else {
        add(243, 262, 150, 64, L_BOX, L_DIM, UI_VIEW_FONT_MENU, UI_VIEW_CENTER, 32, "+");
    }
    label(334, 32, L_DIM, UI_VIEW_FONT_MENU, "A beep plays at each step");
    button(153, 380, 160, 50, L_BUTTON, "Back", HIT_BACK, 0);
}

static void build(int64_t now)
{
    memset(&S->view, 0, sizeof(S->view));
    S->hit_count = 0;
    switch (S->page) {
    case PAGE_MAIN: draw_main(); break;
    case PAGE_WIFI: draw_wifi(now); break;
    case PAGE_NETWORKS: draw_networks(); break;
    case PAGE_PASSWORD: draw_password(now); break;
    case PAGE_JOINING: draw_joining(); break;
    case PAGE_JOINED: draw_joined(); break;
    case PAGE_BLUETOOTH: draw_bluetooth(); break;
    case PAGE_VOLUME: draw_volume(); break;
    }
}

// ---- Loop -------------------------------------------------------------------------------------

void settings_poll(int64_t now, board_input_event_t event, bool back_key, bool paused)
{
    if (!s_open) {
        return;
    }
    if (paused) {
        S->touched_us = now;
        return;
    }
    const bool tap = event == BOARD_INPUT_TAP || event == BOARD_INPUT_PRESS;
    const bool swipe = event == BOARD_INPUT_SWIPE_UP || event == BOARD_INPUT_SWIPE_DOWN;
    if (tap || swipe || back_key) {
        S->touched_us = now;
    }
    if (back_key) {
        back();
    } else if (swipe) {
        // The finger moving up brings up the rows below.
        if (S->page == PAGE_NETWORKS) {
            scroll(event == BOARD_INPUT_SWIPE_UP ? SCROLL_ROWS : -SCROLL_ROWS);
        }
    } else if (tap) {
        int x = 0, y = 0;
        if (board_input_point(&x, &y)) {
            const hit_t *hit = hit_at(x, y);
            if (hit) {
                const hit_t chosen = *hit;
                on_hit(&chosen, now);
            }
        }
    }
    if (!s_open) {
        return;
    }
    if (S->page == PAGE_JOINING) {
        S->touched_us = now;
        if (wifi_switch_poll() != WIFI_SWITCH_TRYING) {
            load_stored();
            refresh_link(now, true);
            go(PAGE_JOINED);
        }
    }
    if (now - S->touched_us > IDLE_CLOSE_US) {
        ESP_LOGI(TAG, "no touch for a minute");
        close_to_logo();
        return;
    }
    refresh_link(now, false);
    build(now);
    display_show_view(&S->view);
}
