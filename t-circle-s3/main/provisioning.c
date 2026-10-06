// BLE provisioning over NimBLE. Contract: docs/ble-provisioning.md.
//
// Every characteristic needs an encrypted, authenticated link: LE Secure Connections with a
// passkey the device shows on its screen (IO capability "display only"). Bonds live in RAM for
// the current pairing session only, and each session advertises from a fresh random static
// address, so a phone never meets a stale bond after a factory reset.
//
// GATT callbacks run on the NimBLE host task and only validate and queue. The prov task does
// the slow work (Wi-Fi scan and connect, NVS) and owns all state below.

#include "provisioning.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "device_config.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "wifi.h"

void ble_store_config_init(void);

#define MODEL "t-circle-s3"
#define SCAN_MAX 20
#define WIFI_TRIAL_TIMEOUT_US (30LL * 1000000)
// After the credentials are saved, BLE stays up until the app disconnects, at most this long.
#define SAVED_LINGER_US (30LL * 1000000)
// A provisioned device leaves pairing mode after this long without a connection.
#define PAIRING_MODE_TIMEOUT_US (5LL * 60 * 1000000)
#define CHR_VALUE_MAX 512

// Application ATT errors (0x80-0x9F), see docs/ble-provisioning.md.
#define ERR_BAD_JSON 0x80
#define ERR_MISSING_FIELD 0x81
#define ERR_BAD_VALUE 0x82
#define ERR_BUSY 0x83

// f2d2xxxx-eb8e-4a64-9ec7-ab0cbb8107c8, little-endian.
#define COMMA_UUID(x)                                                                                   \
    BLE_UUID128_INIT(0xc8, 0x07, 0x81, 0xbb, 0x0c, 0xab, 0xc7, 0x9e, 0x64, 0x4a, 0x8e, 0xeb, (x) & 0xff, \
                     ((x) >> 8) & 0xff, 0xd2, 0xf2)

static const ble_uuid128_t UUID_SERVICE = COMMA_UUID(0x0001);
static const ble_uuid128_t UUID_WIFI_SCAN = COMMA_UUID(0x0002);
static const ble_uuid128_t UUID_WIFI_CREDENTIALS = COMMA_UUID(0x0003);
static const ble_uuid128_t UUID_CREDENTIALS = COMMA_UUID(0x0004);
static const ble_uuid128_t UUID_STATUS = COMMA_UUID(0x0005);
static const ble_uuid128_t UUID_INFO = COMMA_UUID(0x0006);

typedef enum {
    EV_SCAN,
    EV_WIFI,
    EV_CREDS,
    EV_CONNECTED,
    EV_DISCONNECTED,
    EV_PASSKEY,
    EV_PAIRED,
    EV_PAIR_FAILED,
    EV_STOP,
    EV_REDRAW,
    EV_CONNECTOR,
} event_type_t;

typedef struct {
    char ssid[33];
    char password[65];
} wifi_creds_t;

typedef struct {
    event_type_t type;
    uint32_t value;
    void *data;  // wifi_creds_t or device_config_t, owned by the receiver
} event_t;

static const char *TAG = "prov";

static QueueHandle_t s_events;
static atomic_bool s_active;
static atomic_bool s_task_running;
static char s_name[16];   // Comma-XXXX
static char s_mac[18];
static uint8_t s_own_addr_type;

// Owned by the host task (connection bookkeeping) and read elsewhere.
static atomic_int s_conn = BLE_HS_CONN_HANDLE_NONE;
static atomic_bool s_secure;
static atomic_bool s_sub_status;
static atomic_bool s_sub_scan;
static atomic_bool s_saved;
static uint16_t s_handle_scan;
static uint16_t s_handle_status;

// Last status JSON, served by reads of `status`.
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_status[256] = "{\"state\":\"idle\"}";
static char s_connector_state[24];
static char s_connector_device_id[64];

static void post(event_type_t type, uint32_t value, void *data)
{
    const event_t ev = {.type = type, .value = value, .data = data};
    if (!s_events || xQueueSend(s_events, &ev, 0) != pdTRUE) {
        free(data);
    }
}

// ---- GATT ---------------------------------------------------------------------------------------

static int read_flat(struct ble_gatt_access_ctxt *ctxt, char *buf, size_t size)
{
    const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len >= size) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint16_t got = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, (uint16_t)(size - 1), &got) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    buf[got] = '\0';
    return 0;
}

// Copy a JSON string field. Returns false if present but not a string or too long.
static bool take_string(const cJSON *obj, const char *key, char *dst, size_t size, bool *present)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    *present = false;
    dst[0] = '\0';
    if (!item || cJSON_IsNull(item)) {
        return true;
    }
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= size) {
        return false;
    }
    strlcpy(dst, item->valuestring, size);
    *present = dst[0] != '\0';
    return true;
}

static int on_wifi_credentials(const char *json)
{
    cJSON *obj = cJSON_Parse(json);
    if (!cJSON_IsObject(obj)) {
        cJSON_Delete(obj);
        return ERR_BAD_JSON;
    }
    wifi_creds_t *creds = calloc(1, sizeof(*creds));
    if (!creds) {
        cJSON_Delete(obj);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    bool has_ssid, has_password;
    const bool ok = take_string(obj, "ssid", creds->ssid, sizeof(creds->ssid), &has_ssid) &&
                    take_string(obj, "password", creds->password, sizeof(creds->password), &has_password);
    cJSON_Delete(obj);
    const size_t pass_len = strlen(creds->password);
    int rc = 0;
    if (!ok) {
        rc = ERR_BAD_VALUE;
    } else if (!has_ssid) {
        rc = ERR_MISSING_FIELD;
    } else if (pass_len > 0 && pass_len < 8) {
        rc = ERR_BAD_VALUE;  // WPA2 needs 8-63 characters (64 is a hex PSK)
    }
    if (rc) {
        memset(creds, 0, sizeof(*creds));
        free(creds);
        return rc;
    }
    post(EV_WIFI, 0, creds);
    return 0;
}

static bool server_ok(const char *s)
{
    return strncmp(s, "https://", 8) == 0 || strncmp(s, "http://", 7) == 0;
}

static bool connect_url_ok(const char *s)
{
    return strncmp(s, "wss://", 6) == 0 || strncmp(s, "ws://", 5) == 0;
}

static int on_credentials(const char *json)
{
    cJSON *obj = cJSON_Parse(json);
    if (!cJSON_IsObject(obj)) {
        cJSON_Delete(obj);
        return ERR_BAD_JSON;
    }
    device_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        cJSON_Delete(obj);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    bool server, connect_url, ws, group, token, voice, name;
    const bool ok = take_string(obj, "server", cfg->server, sizeof(cfg->server), &server) &&
                    take_string(obj, "connect_url", cfg->connect_url, sizeof(cfg->connect_url), &connect_url) &&
                    take_string(obj, "workspace_id", cfg->workspace_id, sizeof(cfg->workspace_id), &ws) &&
                    take_string(obj, "group_id", cfg->group_id, sizeof(cfg->group_id), &group) &&
                    take_string(obj, "connector_token", cfg->connector_token, sizeof(cfg->connector_token),
                                &token) &&
                    take_string(obj, "voice_key", cfg->voice_key, sizeof(cfg->voice_key), &voice) &&
                    take_string(obj, "device_name", cfg->device_name, sizeof(cfg->device_name), &name);
    cJSON_Delete(obj);
    int rc = 0;
    if (!ok) {
        rc = ERR_BAD_VALUE;
    } else if (!server || !connect_url || !ws || !group || !token || !voice || !name) {
        rc = ERR_MISSING_FIELD;
    } else if (!server_ok(cfg->server) || !connect_url_ok(cfg->connect_url) ||
               strncmp(cfg->voice_key, "salix_vk_", 9) != 0 || strncmp(cfg->connector_token, "salix_conn_", 11) != 0) {
        rc = ERR_BAD_VALUE;
    }
    if (rc) {
        memset(cfg, 0, sizeof(*cfg));
        free(cfg);
        return rc;
    }
    post(EV_CREDS, 0, cfg);
    return 0;
}

static int info_json(char *out, size_t size)
{
    const esp_app_desc_t *app = esp_app_get_description();
    return snprintf(out, size,
                    "{\"fw_version\":\"%s\",\"mac\":\"%s\",\"model\":\"" MODEL "\",\"name\":\"%s\","
                    "\"provisioned\":%s}",
                    app->version, s_mac, s_name, device_config_provisioned() ? "true" : "false");
}

static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    const ble_uuid_t *uuid = ctxt->chr->uuid;
    char buf[CHR_VALUE_MAX + 1];

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        int n;
        if (ble_uuid_cmp(uuid, &UUID_INFO.u) == 0) {
            n = info_json(buf, sizeof(buf));
        } else if (ble_uuid_cmp(uuid, &UUID_STATUS.u) == 0) {
            taskENTER_CRITICAL(&s_status_mux);
            n = (int)strlcpy(buf, s_status, sizeof(buf));
            taskEXIT_CRITICAL(&s_status_mux);
        } else {
            return BLE_ATT_ERR_READ_NOT_PERMITTED;
        }
        return os_mbuf_append(ctxt->om, buf, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (ble_uuid_cmp(uuid, &UUID_WIFI_SCAN.u) == 0) {
        post(EV_SCAN, 0, NULL);
        return 0;
    }
    int rc = read_flat(ctxt, buf, sizeof(buf));
    if (rc) {
        return rc;
    }
    if (ble_uuid_cmp(uuid, &UUID_WIFI_CREDENTIALS.u) == 0) {
        rc = on_wifi_credentials(buf);
    } else if (ble_uuid_cmp(uuid, &UUID_CREDENTIALS.u) == 0) {
        rc = on_credentials(buf);
    } else {
        rc = BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    memset(buf, 0, sizeof(buf));  // may hold a password or token
    return rc;
}

#define SECURE_READ (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN)
#define SECURE_WRITE (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN)

static const struct ble_gatt_svc_def SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SERVICE.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &UUID_WIFI_SCAN.u,
                    .access_cb = gatt_access,
                    .flags = SECURE_WRITE | BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &s_handle_scan,
                },
                {
                    .uuid = &UUID_WIFI_CREDENTIALS.u,
                    .access_cb = gatt_access,
                    .flags = SECURE_WRITE,
                },
                {
                    .uuid = &UUID_CREDENTIALS.u,
                    .access_cb = gatt_access,
                    .flags = SECURE_WRITE,
                },
                {
                    .uuid = &UUID_STATUS.u,
                    .access_cb = gatt_access,
                    .flags = SECURE_READ | BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &s_handle_status,
                },
                {
                    .uuid = &UUID_INFO.u,
                    .access_cb = gatt_access,
                    .flags = SECURE_READ,
                },
                {0},
            },
    },
    {0},
};

static void notify(uint16_t handle, const char *json)
{
    const int conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE || !s_secure) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, strlen(json));
    if (!om) {
        ESP_LOGW(TAG, "notify: no mbuf");
        return;
    }
    const int rc = ble_gatts_notify_custom(conn, handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify failed: %d", rc);
    }
}

// ---- GAP ----------------------------------------------------------------------------------------

static int on_gap(struct ble_gap_event *event, void *arg);

static void advertise(void)
{
    if (!s_active || s_saved || s_conn != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &UUID_SERVICE;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields: %d", rc);
        return;
    }
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (const uint8_t *)s_name;
    rsp.name_len = strlen(s_name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan response fields: %d", rc);
        return;
    }
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, on_gap, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as %s", s_name);
}

static int on_gap(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            advertise();
            return 0;
        }
        s_conn = event->connect.conn_handle;
        s_secure = false;
        s_sub_status = false;
        s_sub_scan = false;
        ESP_LOGI(TAG, "connected, asking for pairing");
        // Ask the phone to pair now, so the passkey dialog comes up right away.
        ble_gap_security_initiate(event->connect.conn_handle);
        post(EV_CONNECTED, 0, NULL);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected, reason 0x%x", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_secure = false;
        s_sub_status = false;
        s_sub_scan = false;
        post(EV_DISCONNECTED, 0, NULL);
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io io = {.action = BLE_SM_IOACT_DISP};
            io.passkey = esp_random() % 1000000;
            post(EV_PASSKEY, io.passkey, NULL);
            const int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
            if (rc != 0) {
                ESP_LOGE(TAG, "passkey inject: %d", rc);
            }
        } else {
            ESP_LOGW(TAG, "unexpected passkey action %d", event->passkey.params.action);
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0 && ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0 &&
            desc.sec_state.encrypted && desc.sec_state.authenticated && desc.sec_state.key_size == 16) {
            s_secure = true;
            ESP_LOGI(TAG, "paired (LE Secure Connections, passkey)");
            post(EV_PAIRED, 0, NULL);
        } else {
            ESP_LOGW(TAG, "pairing failed, status %d", event->enc_change.status);
            post(EV_PAIR_FAILED, 0, NULL);
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_AUTH_FAIL);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // Same phone pairing again within this session: drop the old keys and continue.
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_handle_status) {
            s_sub_status = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == s_handle_scan) {
            s_sub_scan = event->subscribe.cur_notify;
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void on_sync(void)
{
    // A fresh random static address each session, so a phone holding a bond from an earlier
    // session (say, before a factory reset) sees a new device instead of failing to encrypt.
    ble_addr_t addr;
    int rc = ble_hs_id_gen_rnd(0, &addr);
    if (rc == 0) {
        rc = ble_hs_id_set_rnd(addr.val);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "random address: %d", rc);
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "address type: %d", rc);
        return;
    }
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset, reason %d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t ble_up(void)
{
    // Wi-Fi and BLE share the radio; Wi-Fi must allow modem sleep while BLE runs.
    wifi_set_power_save(true);
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble init: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(SERVICES);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(SERVICES);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT services: %d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }
    ble_svc_gap_device_name_set(s_name);
    ble_store_config_init();
    // RAM store only (CONFIG_BT_NIMBLE_NVS_PERSIST off), cleared anyway: bonds last one session.
    ble_store_clear();
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

static void ble_down(void)
{
    const int conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ble_gap_adv_stop();
    if (nimble_port_stop() == 0) {
        nimble_port_deinit();
    } else {
        ESP_LOGE(TAG, "nimble stop failed");
    }
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_secure = false;
    wifi_set_power_save(false);
    ESP_LOGI(TAG, "BLE off");
}

// ---- State --------------------------------------------------------------------------------------

typedef enum {
    SCREEN_ADVERTISING,
    SCREEN_PASSKEY,
    SCREEN_PAIRED,
    SCREEN_WIFI_CONNECTING,
    SCREEN_WIFI_FAILED,
    SCREEN_WIFI_UP,
    SCREEN_SAVED,
    SCREEN_PAIR_FAILED,
} screen_t;

typedef struct {
    screen_t screen;
    uint32_t passkey;
    const char *error;  // static string
    wifi_creds_t wifi;  // being tried, or tried last
    bool wifi_pending;  // wifi holds a network the app sent this session
    wifi_result_t last_result;
    int64_t wifi_since_us;
    device_config_t *creds;  // waiting for Wi-Fi
    int64_t saved_us;
    int64_t idle_since_us;   // no connection since
    bool was_provisioned;
} session_t;

static session_t s;

static const char *wifi_error(wifi_result_t result)
{
    switch (result) {
    case WIFI_RESULT_WRONG_PASSWORD: return "wrong_password";
    case WIFI_RESULT_NOT_FOUND: return "not_found";
    default: return "failed";
    }
}

static void show(screen_t screen)
{
    s.screen = screen;
    char buf[160];
    switch (screen) {
    case SCREEN_ADVERTISING:
        snprintf(buf, sizeof(buf), "Open the Comma app to set up %s", s_name);
        display_show_card(s.was_provisioned ? "Pairing mode" : "Hello", NULL, buf);
        break;
    case SCREEN_PASSKEY: {
        char code[16];
        snprintf(code, sizeof(code), "%03u %03u", (unsigned)(s.passkey / 1000), (unsigned)(s.passkey % 1000));
        display_show_card("Pairing code", code, "Enter it on your phone");
        break;
    }
    case SCREEN_PAIRED:
        display_show_card("Paired", NULL, "Choose a Wi-Fi network in the app");
        break;
    case SCREEN_WIFI_CONNECTING:
        snprintf(buf, sizeof(buf), "%s", s.wifi.ssid);
        display_show_card("Connecting to", NULL, buf);
        break;
    case SCREEN_WIFI_FAILED: {
        const char *why = "Could not connect. Try again in the app.";
        if (s.last_result == WIFI_RESULT_WRONG_PASSWORD) {
            why = "Wrong password. Try again.";
        } else if (s.last_result == WIFI_RESULT_NOT_FOUND) {
            why = "Network not found. Only 2.4 GHz works.";
        } else if (s.error && strcmp(s.error, "timeout") == 0) {
            why = "No answer from the network. Try again.";
        }
        display_show_card("Wi-Fi failed", NULL, why);
        break;
    }
    case SCREEN_WIFI_UP:
        display_show_card("Wi-Fi connected", NULL, "Finishing setup in the app");
        break;
    case SCREEN_SAVED:
        display_show_card("All set", NULL, device_config_provisioned() ? "Tap to call Comma" : "");
        break;
    case SCREEN_PAIR_FAILED:
        display_show_card("Pairing failed", NULL, "Try again from the app");
        break;
    }
}

static void publish_status(const char *state, const char *error)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return;
    }
    cJSON_AddStringToObject(obj, "state", state);
    if (error) {
        cJSON_AddStringToObject(obj, "error", error);
    }
    if (s.wifi.ssid[0]) {
        cJSON_AddStringToObject(obj, "ssid", s.wifi.ssid);
    }
    char ip[16];
    wifi_ip_string(ip, sizeof(ip));
    if (ip[0]) {
        cJSON_AddStringToObject(obj, "ip", ip);
    }
    if (s_connector_device_id[0]) {
        cJSON_AddStringToObject(obj, "device_id", s_connector_device_id);
    }
    cJSON_AddBoolToObject(obj, "provisioned", device_config_provisioned());
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json) {
        return;
    }
    taskENTER_CRITICAL(&s_status_mux);
    strlcpy(s_status, json, sizeof(s_status));
    taskEXIT_CRITICAL(&s_status_mux);
    ESP_LOGI(TAG, "status %s", json);
    if (s_sub_status) {
        notify(s_handle_status, json);
    }
    free(json);
}

static void do_scan(void)
{
    wifi_ap_t *aps = calloc(SCAN_MAX, sizeof(*aps));
    if (!aps) {
        notify(s_handle_scan, "{\"error\":\"no_memory\",\"page\":0,\"pages\":1,\"aps\":[]}");
        return;
    }
    size_t count = 0;
    const esp_err_t err = wifi_scan(aps, SCAN_MAX, &count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
        notify(s_handle_scan, err == ESP_ERR_WIFI_STATE ? "{\"error\":\"busy\",\"page\":0,\"pages\":1,\"aps\":[]}"
                                                         : "{\"error\":\"failed\",\"page\":0,\"pages\":1,\"aps\":[]}");
        free(aps);
        return;
    }
    ESP_LOGI(TAG, "scan found %u networks", (unsigned)count);

    // Split into notifications that fit the MTU. Room is left for the page fields.
    const int conn = s_conn;
    const int mtu = conn == BLE_HS_CONN_HANDLE_NONE ? 23 : ble_att_mtu(conn);
    const size_t budget = mtu > 3 + 40 ? (size_t)mtu - 3 - 40 : 0;
    size_t starts[SCAN_MAX + 1];
    size_t pages = 0;
    size_t i = 0;
    while (i < count || pages == 0) {
        starts[pages++] = i;
        cJSON *list = cJSON_CreateArray();
        size_t in_page = 0;
        while (i < count && list) {
            cJSON *ap = cJSON_CreateObject();
            cJSON_AddStringToObject(ap, "ssid", aps[i].ssid);
            cJSON_AddNumberToObject(ap, "rssi", aps[i].rssi);
            cJSON_AddBoolToObject(ap, "secure", aps[i].secure);
            cJSON_AddItemToArray(list, ap);
            char *json = cJSON_PrintUnformatted(list);
            const size_t len = json ? strlen(json) : SIZE_MAX;
            free(json);
            if (len > budget) {
                cJSON_DeleteItemFromArray(list, (int)in_page);
                if (in_page == 0) {
                    ++i;  // one entry alone is too big for this MTU: skip it
                }
                break;
            }
            ++in_page;
            ++i;
        }
        cJSON_Delete(list);
    }
    starts[pages] = count;

    for (size_t p = 0; p < pages; ++p) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "page", (double)p);
        cJSON_AddNumberToObject(obj, "pages", (double)pages);
        cJSON *list = cJSON_AddArrayToObject(obj, "aps");
        for (size_t k = starts[p]; k < starts[p + 1]; ++k) {
            cJSON *ap = cJSON_CreateObject();
            cJSON_AddStringToObject(ap, "ssid", aps[k].ssid);
            cJSON_AddNumberToObject(ap, "rssi", aps[k].rssi);
            cJSON_AddBoolToObject(ap, "secure", aps[k].secure);
            cJSON_AddItemToArray(list, ap);
        }
        char *json = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
        if (json) {
            if (strlen(json) + 3 <= (size_t)mtu) {
                notify(s_handle_scan, json);
            } else {
                ESP_LOGW(TAG, "scan page %u does not fit MTU %d", (unsigned)p, mtu);
            }
            free(json);
        }
    }
    free(aps);
}

static void commit(void)
{
    device_config_t *cfg = s.creds;
    if (s.wifi_pending) {
        strlcpy(cfg->wifi_ssid, s.wifi.ssid, sizeof(cfg->wifi_ssid));
        strlcpy(cfg->wifi_password, s.wifi.password, sizeof(cfg->wifi_password));
    } else {
        // Pairing mode on a provisioned device, credentials only: keep its network.
        device_config_t current;
        device_config_get(&current);
        strlcpy(cfg->wifi_ssid, current.wifi_ssid, sizeof(cfg->wifi_ssid));
        strlcpy(cfg->wifi_password, current.wifi_password, sizeof(cfg->wifi_password));
        memset(&current, 0, sizeof(current));
    }
    const esp_err_t err = device_config_save(cfg);
    memset(cfg, 0, sizeof(*cfg));
    free(cfg);
    s.creds = NULL;
    if (err != ESP_OK) {
        publish_status("wifi_up", "save_failed");
        return;
    }
    wifi_keep_retrying();
    s_saved = true;
    s.saved_us = esp_timer_get_time();
    ble_gap_adv_stop();
    publish_status("saved", NULL);
    show(SCREEN_SAVED);
}

static bool wifi_ready_for_commit(void)
{
    if (s.wifi_pending) {
        return wifi_result() == WIFI_RESULT_UP;
    }
    return s.was_provisioned && wifi_is_up();
}

// Pairing mode on a provisioned device: a network the app sent failed, or the app left before
// saving. Go straight back to the stored network so the device is not offline until pairing mode
// times out. Pairing mode itself stays open; the app may connect again and send another network.
static void back_to_stored_wifi(const char *why)
{
    if (!s.wifi_pending || s_saved || !s.was_provisioned) {
        return;
    }
    s.wifi_pending = false;
    device_config_t *cfg = calloc(1, sizeof(*cfg));
    memset(&s.wifi, 0, sizeof(s.wifi));
    if (cfg && device_config_get(cfg)) {
        ESP_LOGI(TAG, "%s: back to the stored network", why);
        strlcpy(s.wifi.ssid, cfg->wifi_ssid, sizeof(s.wifi.ssid));  // later `status` names it
        wifi_connect(cfg->wifi_ssid, cfg->wifi_password, true);
    }
    if (cfg) {
        memset(cfg, 0, sizeof(*cfg));
        free(cfg);
    }
}

static void handle(const event_t *ev)
{
    switch (ev->type) {
    case EV_CONNECTED:
        s.idle_since_us = 0;
        break;
    case EV_DISCONNECTED:
        s.idle_since_us = esp_timer_get_time();
        if (!s_saved) {
            back_to_stored_wifi("app left without saving");
            show(SCREEN_ADVERTISING);
        }
        break;
    case EV_PASSKEY:
        s.passkey = ev->value;
        ESP_LOGI(TAG, "showing passkey");
        show(SCREEN_PASSKEY);
        break;
    case EV_PAIRED:
        show(SCREEN_PAIRED);
        publish_status(s.wifi_pending && wifi_result() == WIFI_RESULT_UP ? "wifi_up" : "idle", NULL);
        break;
    case EV_PAIR_FAILED:
        show(SCREEN_PAIR_FAILED);
        break;
    case EV_SCAN:
        do_scan();
        break;
    case EV_WIFI: {
        wifi_creds_t *creds = ev->data;
        if (s_saved) {
            break;
        }
        s.wifi = *creds;
        s.wifi_pending = true;
        s.error = NULL;
        s.last_result = WIFI_RESULT_CONNECTING;
        s.wifi_since_us = esp_timer_get_time();
        const esp_err_t err = wifi_connect(s.wifi.ssid, s.wifi.password, false);
        if (err != ESP_OK) {
            s.last_result = WIFI_RESULT_FAILED;
            s.error = "failed";
            publish_status("wifi_failed", "failed");
            show(SCREEN_WIFI_FAILED);
            back_to_stored_wifi("wifi_failed");
        } else {
            publish_status("wifi_connecting", NULL);
            show(SCREEN_WIFI_CONNECTING);
        }
        break;
    }
    case EV_CREDS:
        if (s_saved) {
            break;
        }
        if (s.creds) {
            memset(s.creds, 0, sizeof(*s.creds));
            free(s.creds);
        }
        s.creds = ev->data;
        ESP_LOGI(TAG, "credentials received for \"%s\"", s.creds->device_name);
        if (wifi_ready_for_commit()) {
            commit();
        }
        break;
    case EV_REDRAW:
        show(s.screen);
        break;
    case EV_CONNECTOR:
        if (s_connector_state[0]) {
            publish_status(s_connector_state, NULL);
        }
        break;
    case EV_STOP:
        break;
    }
}

// Watch a Wi-Fi trial that the app started.
static void poll_wifi(void)
{
    if (!s.wifi_pending || s_saved || s.last_result != WIFI_RESULT_CONNECTING) {
        return;
    }
    const wifi_result_t result = wifi_result();
    if (result == WIFI_RESULT_UP) {
        s.last_result = result;
        publish_status("wifi_up", NULL);
        if (s.creds) {
            commit();
        } else {
            show(SCREEN_WIFI_UP);
        }
    } else if (result != WIFI_RESULT_CONNECTING) {
        s.last_result = result;
        s.error = wifi_error(result);
        publish_status("wifi_failed", s.error);
        show(SCREEN_WIFI_FAILED);
        back_to_stored_wifi("wifi_failed");
    } else if (esp_timer_get_time() - s.wifi_since_us > WIFI_TRIAL_TIMEOUT_US) {
        wifi_disconnect();
        s.last_result = WIFI_RESULT_FAILED;
        s.error = "timeout";
        publish_status("wifi_failed", "timeout");
        show(SCREEN_WIFI_FAILED);
        back_to_stored_wifi("wifi_failed");
    }
}

static void prov_task(void *arg)
{
    (void)arg;
    memset(&s, 0, sizeof(s));
    s.was_provisioned = device_config_provisioned();
    s.idle_since_us = esp_timer_get_time();
    s_saved = false;
    s_connector_state[0] = '\0';
    s_connector_device_id[0] = '\0';
    publish_status("idle", NULL);
    show(SCREEN_ADVERTISING);

    bool stop = false;
    while (!stop) {
        event_t ev;
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (ev.type == EV_STOP) {
                stop = s.was_provisioned || s_saved;
            } else {
                handle(&ev);
            }
            if (ev.type == EV_WIFI && ev.data) {
                memset(ev.data, 0, sizeof(wifi_creds_t));
                free(ev.data);
            }
        }
        poll_wifi();
        const int64_t now = esp_timer_get_time();
        if (s_saved && (s_conn == BLE_HS_CONN_HANDLE_NONE || now - s.saved_us > SAVED_LINGER_US)) {
            stop = true;
        }
        if (!s_saved && s.was_provisioned && s.idle_since_us && now - s.idle_since_us > PAIRING_MODE_TIMEOUT_US) {
            ESP_LOGI(TAG, "pairing mode timed out");
            stop = true;
        }
    }

    ble_down();
    if (s.creds) {
        memset(s.creds, 0, sizeof(*s.creds));
        free(s.creds);
        s.creds = NULL;
    }
    // Left pairing mode without saving while trying another network: go back to the stored one.
    back_to_stored_wifi("pairing mode ended");
    memset(&s.wifi, 0, sizeof(s.wifi));
    if (s_saved) {
        vTaskDelay(pdMS_TO_TICKS(2000));  // let "All set" show
    }
    display_show_logo();
    s_active = false;
    s_task_running = false;
    vTaskDelete(NULL);
}

// ---- API ----------------------------------------------------------------------------------------

esp_err_t provisioning_start(void)
{
    if (s_active || s_task_running) {
        return ESP_OK;
    }
    if (!s_events) {
        s_events = xQueueCreate(8, sizeof(event_t));
        if (!s_events) {
            return ESP_ERR_NO_MEM;
        }
    }
    xQueueReset(s_events);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_name, sizeof(s_name), "Comma-%02X%02X", mac[4], mac[5]);
    snprintf(s_mac, sizeof(s_mac), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    s_active = true;
    s_saved = false;
    esp_err_t err = ble_up();
    if (err != ESP_OK) {
        s_active = false;
        wifi_set_power_save(false);
        return err;
    }
    s_task_running = true;
    if (xTaskCreate(prov_task, "prov", 6144, NULL, 4, NULL) != pdPASS) {
        s_task_running = false;
        ble_down();
        s_active = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "provisioning started (%s)", device_config_provisioned() ? "pairing mode" : "unprovisioned");
    return ESP_OK;
}

void provisioning_stop(void)
{
    if (s_active) {
        post(EV_STOP, 0, NULL);
    }
}

bool provisioning_active(void)
{
    return s_active;
}

void provisioning_redraw(void)
{
    if (s_active) {
        post(EV_REDRAW, 0, NULL);
    }
}

void provisioning_set_connector_state(const char *state, const char *device_id)
{
    strlcpy(s_connector_state, state ? state : "", sizeof(s_connector_state));
    if (device_id) {
        strlcpy(s_connector_device_id, device_id, sizeof(s_connector_device_id));
    }
    if (s_active) {
        post(EV_CONNECTOR, 0, NULL);
    }
}
