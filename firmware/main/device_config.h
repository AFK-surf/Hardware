#pragma once

// The device's Wi-Fi and Comma credentials. The Comma app writes them over BLE
// (provisioning.c); they live in NVS namespace "comma". Everything that needs a
// credential (voice, Connector) reads it through device_config_get().
//
// Dev override: a main/secrets.h with COMMA_DEV_SECRETS set to 1 replaces NVS
// entirely (see secrets.h.example). Without it, or with the flag at 0, the
// firmware has no compiled-in secrets.

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char server[128];            // https://salix-staging.cue.surf (voice)
    char connect_url[160];       // wss://<salix host>/v1/connect (Connector), as minted
    char workspace_id[96];
    char group_id[96];
    char connector_token[160];   // salix_conn_...
    char voice_key[160];         // salix_vk_...
    char device_name[64];
} device_config_t;

// Init the NVS partition and load the stored config. Call once, first thing.
esp_err_t device_config_init(void);

// True once Wi-Fi and Comma credentials have been stored (or the dev override is on).
bool device_config_provisioned(void);

// True when the values come from secrets.h, not NVS.
bool device_config_dev_override(void);

// Copy the current config. Returns false (and zeroes *out) when unprovisioned.
bool device_config_get(device_config_t *out);

// Bumped on every save. A client that cached credentials reconnects when it changes.
unsigned device_config_generation(void);

// Store a complete config and mark the device provisioned.
esp_err_t device_config_save(const device_config_t *cfg);

// Erase the whole NVS partition: credentials, Wi-Fi driver state, BLE keys.
// The caller restarts the chip afterwards.
esp_err_t device_config_erase_all(void);
