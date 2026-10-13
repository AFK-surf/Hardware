// SPDX-License-Identifier: Apache-2.0
// Run the real UI loop with a virtual clock and input; assert every 10 ms is still polled offline.
#include "../../voice/call.c"
#include <assert.h>
#include <setjmp.h>

static jmp_buf stopped;
static int64_t clock_us, stop_us, wifi_at, tap2_at, hold_at, provision_at;
static int polls, controls, calls, waits, timeouts, checks, ends, logos;
static bool voice_up;
#define CHECK(c) do { ++checks; assert(c); } while (0)
int64_t esp_timer_get_time(void) { return clock_us; }
void vTaskDelay(unsigned ms) { clock_us += ms * 1000LL; if (clock_us >= stop_us) longjmp(stopped, 1); }
board_input_event_t board_input_poll(void) {
    ++polls;
    return clock_us == 10000 || (tap2_at && clock_us == tap2_at) ? BOARD_INPUT_TAP : BOARD_INPUT_NONE;
}
bool board_input_boot_held(void) { return hold_at && clock_us >= hold_at; }
void app_controls_init(bool held) { (void)held; }
bool app_poll(int64_t now, bool in_call, bool held) { (void)now; (void)in_call; ++controls; return held; }
bool device_config_provisioned(void) { return true; }
bool provisioning_active(void) { return provision_at && clock_us >= provision_at; }
void provisioning_stop(void) { provision_at = 0; }
bool wifi_is_up(void) { return wifi_at && clock_us >= wifi_at; }
esp_err_t wifi_wait(uint32_t ms) { (void)ms; assert(!"UI must never block waiting for Wi-Fi"); return ESP_FAIL; }
bool comma_voice_can_start(void) { return !voice_up; }
bool comma_voice_ending(void) { return false; }
comma_voice_state_t comma_voice_state(void) { return voice_up ? COMMA_VOICE_UP : COMMA_VOICE_IDLE; }
comma_voice_end_t comma_voice_last_end(void) { return COMMA_VOICE_END_NORMAL; }
int comma_voice_close_code(void) { return 0; }
void comma_voice_credentials_changed(void) {}
esp_err_t comma_voice_start(void) { CHECK(wifi_is_up() && !provisioning_active()); ++calls; voice_up = true; return ESP_OK; }
void comma_voice_end(void) { ++ends; voice_up = false; }
void display_show_card(const char *label, const char *big, const char *body) {
    (void)big; (void)body;
    if (!strcmp(label, "Waiting for Wi-Fi")) ++waits;
    if (!strcmp(label, "No Wi-Fi")) ++timeouts;
}
void display_show_logo(void) { ++logos; }
void display_wake(void) {}
void display_keep_awake(bool awake) { (void)awake; }
bool playback_ringing(void) { return false; }
bool playback_speaking(void) { return false; }
void playback_init(void) {}
void playback_tone(playback_tone_t t) { (void)t; }
esp_err_t voice_clean_init(void) { return ESP_OK; }
esp_err_t temperature_sensor_install(const temperature_sensor_config_t *c, temperature_sensor_handle_t *h) {
    (void)c; *h = NULL; return ESP_FAIL;
}
esp_err_t temperature_sensor_enable(temperature_sensor_handle_t h) { (void)h; return ESP_FAIL; }
esp_err_t temperature_sensor_get_celsius(temperature_sensor_handle_t h, float *c) { (void)h; (void)c; return ESP_FAIL; }
static void run(int64_t duration) {
    clock_us = 0; stop_us = duration; polls = controls = calls = waits = timeouts = ends = logos = 0; voice_up = false;
    if (!setjmp(stopped)) call_run_ui("host UI regression");
    CHECK(polls == duration / 10000 && controls == polls);
}
int main(void) {
    // Offline for 21 s: the UI polls continuously and expires the pending dial exactly once.
    run(21000000); CHECK(waits == 1 && timeouts == 1 && calls == 0);
    // Wi-Fi recovers during the wait: dial once, then keep polling.
    wifi_at = 2000000; run(3000000); CHECK(calls == 1 && timeouts == 0);
    // A second tap cancels; a later reconnect cannot initiate an unwanted call.
    tap2_at = 1000000; run(3000000); CHECK(calls == 0 && waits == 1);
    // A BOOT hold cancels the pending call and is polled even while offline.
    tap2_at = 0; hold_at = 500000; run(3000000); CHECK(calls == 0 && timeouts == 0 && logos == 1);
    // Pairing taking over cancels the dial, without starting a voice session alongside BLE.
    hold_at = 0; provision_at = 500000; run(3000000); CHECK(calls == 0 && timeouts == 0);
    // Normal online tap and hang-up still work.
    provision_at = 0; wifi_at = 1; tap2_at = 1000000; run(2000000); CHECK(calls == 1 && ends == 1 && waits == 0);
    printf("call_ui: %d checks passed\n", checks);
}
