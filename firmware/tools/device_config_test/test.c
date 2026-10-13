// SPDX-License-Identifier: Apache-2.0
// Real configuration/consent code, transactional fake NVS, and actual pthread mutexes.
#include "../../main/device_config.c"
#include <assert.h>
#include <stdio.h>

typedef struct { char key[24], value[192]; } entry_t;
static entry_t stored[2][16], pending[2][16];
static const char *fail_key;
static int fail_commit;
static int checks;
#define CHECK(c) do { ++checks; assert(c); } while (0)

esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { memset(stored, 0, sizeof(stored)); return ESP_OK; }
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) {
    *h = strcmp(ns, "comma") == 0 ? 0 : 1;
    memcpy(pending[*h], stored[*h], sizeof(stored[*h]));
    (void)mode; return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
static entry_t *lookup(entry_t *entries, const char *key, bool create) {
    for (int i = 0; i < 16; ++i) {
        if (!strcmp(entries[i].key, key)) return &entries[i];
        if (!entries[i].key[0] && create) { strlcpy(entries[i].key, key, sizeof(entries[i].key)); return &entries[i]; }
    }
    return NULL;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len) {
    entry_t *e = lookup(stored[h], key, false);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < strlen(e->value) + 1) return ESP_FAIL;
    strlcpy(out, e->value, *len); *len = strlen(e->value) + 1; return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value) {
    if (fail_key && !strcmp(key, fail_key)) return ESP_FAIL;
    entry_t *e = lookup(pending[h], key, true); assert(e);
    strlcpy(e->value, value, sizeof(e->value)); return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out) {
    entry_t *e = lookup(stored[h], key, false);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    *out = (uint8_t)atoi(e->value); return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v) {
    return nvs_set_str(h, key, v ? "1" : "0");
}
esp_err_t nvs_commit(nvs_handle_t h) {
    if (fail_commit && --fail_commit == 0) return ESP_FAIL;
    memcpy(stored[h], pending[h], sizeof(stored[h])); return ESP_OK;
}
static void reboot(void) {
    if (s_lock) { pthread_mutex_destroy(s_lock); free(s_lock); }
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_provisioned = false; s_allow_operations = false; s_generation = 0;
    CHECK(device_config_init() == ESP_OK);
}
static void grant(void) {
    CHECK(device_config_set_operations_allowed(device_config_generation(), true) == ESP_OK);
    CHECK(device_config_operations_allowed(device_config_generation()));
}
int main(void) {
    device_config_t cfg = {.wifi_ssid = "Home", .server = "https://example.test", .connect_url = "wss://example.test/connect",
        .workspace_id = "workspace-a", .group_id = "group-a", .connector_token = "token-a",
        .voice_key = "voice-a", .device_name = "Comma"};
    reboot();
    CHECK(!device_config_operations_allowed(0));
    CHECK(device_config_set_operations_allowed(0, true) == ESP_ERR_INVALID_STATE);
    CHECK(device_config_save(&cfg) == ESP_OK); grant(); reboot();
    CHECK(device_config_operations_allowed(device_config_generation()));
    unsigned old = device_config_generation();
    strcpy(cfg.wifi_ssid, "New Wi-Fi"); strcpy(cfg.device_name, "Kitchen");
    CHECK(device_config_save(&cfg) == ESP_OK);
    CHECK(device_config_operations_allowed(device_config_generation()));
    CHECK(!device_config_operations_allowed(old));
    CHECK(device_config_set_operations_allowed(old, true) == ESP_ERR_INVALID_STATE);
    reboot(); CHECK(device_config_operations_allowed(device_config_generation()));

    char *fields[] = {cfg.server, cfg.connect_url, cfg.workspace_id, cfg.group_id, cfg.connector_token, cfg.voice_key};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        grant(); old = device_config_generation();
        strcat(fields[i], "-new");
        CHECK(device_config_save(&cfg) == ESP_OK);
        CHECK(!device_config_operations_allowed(device_config_generation()));
        CHECK(device_config_set_operations_allowed(old, true) == ESP_ERR_INVALID_STATE);
        reboot(); CHECK(!device_config_operations_allowed(device_config_generation()));
    }
    grant();
    device_config_t before; CHECK(device_config_get(&before));
    strcpy(cfg.connector_token, "replacement-token");
    fail_commit = 1;  // revocation failed: do not replace credentials
    CHECK(device_config_save(&cfg) != ESP_OK);
    device_config_t after; CHECK(device_config_get(&after));
    CHECK(!strcmp(before.connector_token, after.connector_token));
    CHECK(!device_config_operations_allowed(device_config_generation()));
    grant(); fail_key = "conn_token";  // revoke persisted, subsequent config write failed
    CHECK(device_config_save(&cfg) != ESP_OK);
    fail_key = NULL; reboot();
    CHECK(!device_config_provisioned());
    CHECK(!device_config_operations_allowed(device_config_generation()));
    CHECK(device_config_save(&cfg) == ESP_OK);
    CHECK(!device_config_operations_allowed(device_config_generation()));
    grant(); CHECK(device_config_erase_all() == ESP_OK); reboot();
    CHECK(!device_config_provisioned());
    pthread_mutex_destroy(s_lock); free(s_lock);
    printf("device_config: %d checks passed\n", checks);
}
