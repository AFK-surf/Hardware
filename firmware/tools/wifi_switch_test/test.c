// SPDX-License-Identifier: Apache-2.0
// The on-screen Wi-Fi switch (main/wifi_switch.c) against fake Wi-Fi and config: a new network is
// stored only once it has an IP and with every credential unchanged; any failure, a timeout, a
// cancel or pairing mode goes back to the stored network; the password does not outlive the trial.
#include "../../main/wifi_switch.c"
#include <assert.h>
#include <stdio.h>

static int checks;
#define CHECK(c) do { ++checks; assert(c); } while (0)

static int64_t clock_us;
static device_config_t stored;
static wifi_result_t result;
static bool pairing, save_fails, kept;
static int connects, saves;
static char last_ssid[33], last_password[65];
static bool last_keep;

int64_t esp_timer_get_time(void) { return clock_us; }
bool provisioning_active(void) { return pairing; }
bool device_config_provisioned(void) { return true; }
bool device_config_get(device_config_t *out) { *out = stored; return true; }
esp_err_t device_config_save(const device_config_t *cfg)
{
    ++saves;
    if (save_fails) return ESP_FAIL;
    stored = *cfg;
    return ESP_OK;
}
esp_err_t wifi_connect(const char *ssid, const char *password, bool keep_retrying)
{
    ++connects;
    strlcpy(last_ssid, ssid, sizeof(last_ssid));
    strlcpy(last_password, password, sizeof(last_password));
    last_keep = keep_retrying;
    result = WIFI_RESULT_CONNECTING;
    return ESP_OK;
}
void wifi_keep_retrying(void) { kept = true; }
wifi_result_t wifi_result(void) { return result; }

static void reset(void)
{
    memset(&stored, 0, sizeof(stored));
    strcpy(stored.wifi_ssid, "home");
    strcpy(stored.wifi_password, "homepass1");
    strcpy(stored.connector_token, "salix_conn_test");
    strcpy(stored.voice_key, "salix_vk_test");
    strcpy(stored.device_name, "Comma-7838");
    clock_us = 1000000;
    result = WIFI_RESULT_UP;
    pairing = save_fails = kept = false;
    connects = saves = 0;
    wifi_switch_reset();
}

static bool password_gone(void)
{
    for (size_t i = 0; i < sizeof(s_password); ++i) {
        if (s_password[i]) return false;
    }
    return true;
}

static bool back_home(void)
{
    return !strcmp(last_ssid, "home") && !strcmp(last_password, "homepass1") && last_keep;
}

int main(void)
{
    // Joined: stored with the new network only; the credentials stay; it keeps retrying from now on.
    reset();
    CHECK(wifi_switch_start("ziroom602", "newpass12") == ESP_OK);
    CHECK(connects == 1 && !last_keep && !strcmp(last_ssid, "ziroom602") && saves == 0);
    CHECK(wifi_switch_poll() == WIFI_SWITCH_TRYING);
    result = WIFI_RESULT_UP;
    CHECK(wifi_switch_poll() == WIFI_SWITCH_SAVED && saves == 1 && kept && connects == 1);
    CHECK(!strcmp(stored.wifi_ssid, "ziroom602") && !strcmp(stored.wifi_password, "newpass12"));
    CHECK(!strcmp(stored.connector_token, "salix_conn_test") && !strcmp(stored.voice_key, "salix_vk_test"));
    CHECK(!strcmp(stored.device_name, "Comma-7838") && password_gone());
    wifi_switch_reset();
    CHECK(wifi_switch_poll() == WIFI_SWITCH_IDLE);

    // Each failure the trial reports goes back to the stored network, which stays stored.
    const wifi_result_t failures[] = {WIFI_RESULT_WRONG_PASSWORD, WIFI_RESULT_NOT_FOUND, WIFI_RESULT_FAILED};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        reset();
        CHECK(wifi_switch_start("ziroom602", "newpass12") == ESP_OK);
        result = failures[i];
        CHECK(wifi_switch_poll() == WIFI_SWITCH_REVERTED && wifi_switch_failure() == failures[i]);
        CHECK(back_home() && saves == 0 && !strcmp(stored.wifi_ssid, "home") && password_gone());
    }

    // A trial that neither joins nor fails is given up after 45 s.
    reset();
    CHECK(wifi_switch_start("weak", "newpass12") == ESP_OK);
    clock_us += 44000000;
    CHECK(wifi_switch_poll() == WIFI_SWITCH_TRYING);
    clock_us += 2000000;
    CHECK(wifi_switch_poll() == WIFI_SWITCH_REVERTED && wifi_switch_failure() == WIFI_RESULT_FAILED && back_home());

    // A new network that cannot be stored is not kept either.
    reset();
    save_fails = true;
    CHECK(wifi_switch_start("ziroom602", "newpass12") == ESP_OK);
    result = WIFI_RESULT_UP;
    CHECK(wifi_switch_poll() == WIFI_SWITCH_REVERTED && back_home() && password_gone());

    // Cancel, and pairing mode taking over, both go back; pairing mode also refuses a new trial.
    reset();
    CHECK(wifi_switch_start("ziroom602", "newpass12") == ESP_OK);
    wifi_switch_cancel();
    CHECK(wifi_switch_poll() == WIFI_SWITCH_REVERTED && wifi_switch_failure() == WIFI_RESULT_IDLE && back_home());
    reset();
    CHECK(wifi_switch_start("ziroom602", "newpass12") == ESP_OK);
    pairing = true;
    CHECK(wifi_switch_poll() == WIFI_SWITCH_REVERTED && back_home());
    reset();
    pairing = true;
    CHECK(wifi_switch_start("ziroom602", "newpass12") == ESP_ERR_INVALID_STATE && connects == 0);

    // One trial at a time; passwords Wi-Fi cannot take are refused before anything changes.
    reset();
    CHECK(wifi_switch_start("a", "newpass12") == ESP_OK);
    CHECK(wifi_switch_start("b", "newpass12") == ESP_ERR_INVALID_STATE && connects == 1);
    wifi_switch_cancel();
    reset();
    CHECK(wifi_switch_start("ziroom602", "short") == ESP_ERR_INVALID_ARG && connects == 0);
    CHECK(wifi_switch_start("", "newpass12") == ESP_ERR_INVALID_ARG && connects == 0);
    // An open network has no password.
    CHECK(wifi_switch_start("cafe", "") == ESP_OK && !last_password[0]);

    printf("wifi_switch: %d checks passed\n", checks);
    return 0;
}
