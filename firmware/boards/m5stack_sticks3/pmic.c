/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * M5PM1 power chip, M5Stack StickS3. Adapted for Comma from muse-gadget-sdk's
 * components/muse/boards/board_m5stack_sticks3.c: the register map, the retries and the init
 * sequence are that file's. The wake handshake and the I2C_CFG, ID and voltage register layout are
 * from M5Stack's M5PM1 library (MIT, (c) 2025 M5Stack Technology): facts only, no code copied.
 * The readback log is ours.
 *
 * The PMIC may be asleep when we boot: it sleeps after an I2C idle timeout (I2C_CFG, which it
 * keeps across ESP resets, as set by whatever firmware ran last) and wakes on a falling edge on SDA.
 * Until woken it NACKs its address. So: a START on the bus (an address probe), 10 ms, then talk;
 * if it still does not answer, 800 ms and again, then at 400 kHz (the speed may also have been left
 * there). Once it answers, I2C_CFG goes to 0: no sleep, 100 kHz.
 *
 * The PMIC owns the rails. Its GPIO2 (L3B) powers the LCD and the audio side and connects the
 * codec to I2C; its GPIO3 enables the speaker amp (AW8737), whose enable has a pull-down, so the
 * GPIO must be push-pull (open drain, the PMIC's default, leaves the speaker silent). The PMIC
 * keeps its state across ESP resets, so every bit is set here on every boot, never assumed.
 */

#include "pmic.h"

#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PMIC_ADDR 0x6E
#define PMIC_HZ 100000
#define PMIC_HZ_FAST 400000
#define PMIC_DEVICE_ID 0x00    // ID, model, hardware and firmware revision: 0x00..0x03
#define PMIC_I2C_CFG 0x09      // bit 4: 400 kHz; bits 3..0: sleep after this many idle seconds, 0 = never
#define PMIC_WDT_CNT 0x0A      // watchdog: resets the board after this many seconds unfed; 0 = off
#define PMIC_PWR_SRC 0x04      // what powers the board, bits 2..0 (M5's header and library disagree on the codes)
#define PMIC_WAKE_SRC 0x05     // why it last powered up or reset (bits, write 0 to clear); see log_power_state
#define PMIC_PWR_CFG 0x06      // bit 3: 5 V out to Grove and IR; bit 4: the green LED
#define PMIC_PWR_5V_OUT 0x08
#define PMIC_PWR_LED 0x10
#define PMIC_GPIO_DIR 0x10     // 1 = output
#define PMIC_GPIO_OUT 0x11
#define PMIC_GPIO_IN 0x12
#define PMIC_GPIO_OD 0x13      // 1 = open drain, the default
#define PMIC_L3B (1u << 2)     // LCD and audio power
#define PMIC_SPK (1u << 3)     // amp enable
#define PMIC_VBAT_MV 0x22      // 12-bit little-endian millivolts
#define PMIC_VIN_MV 0x24       // USB

static const char *TAG = "pmic";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_pmic;

// The PMIC is a microcontroller and now and then misses a transfer; retry.
static esp_err_t pmic_read(uint8_t reg, uint8_t *buf, size_t n)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3 && err != ESP_OK; i++) {
        err = i2c_master_transmit_receive(s_pmic, &reg, 1, buf, n, 50);
    }
    return err;
}

static esp_err_t pmic_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = {reg, val};
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3 && err != ESP_OK; i++) {
        err = i2c_master_transmit(s_pmic, buf, sizeof(buf), 50);
    }
    return err;
}

static esp_err_t pmic_update(uint8_t reg, uint8_t mask, bool on)
{
    uint8_t v;
    ESP_RETURN_ON_ERROR(pmic_read(reg, &v, 1), TAG, "read 0x%02x", reg);
    const uint8_t want = on ? (uint8_t)(v | mask) : (uint8_t)(v & ~mask);
    return want == v ? ESP_OK : pmic_write(reg, want);
}

static int pmic_mv(uint8_t reg)
{
    uint8_t b[2];
    return pmic_read(reg, b, sizeof(b)) == ESP_OK ? (b[0] | b[1] << 8) & 0x0fff : -1;
}

static esp_err_t pmic_attach(uint32_t hz)
{
    if (s_pmic) {
        i2c_master_bus_rm_device(s_pmic);
        s_pmic = NULL;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PMIC_ADDR,
        .scl_speed_hz = hz,
    };
    return i2c_master_bus_add_device(s_bus, &cfg, &s_pmic);
}

// A START on SDA wakes it; the probe's own NACK while it sleeps is expected.
static void pmic_wake(void)
{
    i2c_master_probe(s_bus, PMIC_ADDR, 10);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static bool pmic_identify(uint8_t id[4])
{
    return pmic_read(PMIC_DEVICE_ID, id, 4) == ESP_OK;
}

// Wake it and find a speed it answers at; then turn its I2C sleep off and set 100 kHz.
static esp_err_t pmic_connect(void)
{
    static const struct {
        uint32_t hz;
        uint32_t wait_ms;
    } tries[] = {{PMIC_HZ, 0}, {PMIC_HZ, 800}, {PMIC_HZ_FAST, 0}};
    uint8_t id[4] = {0};
    size_t i = 0;
    for (; i < sizeof(tries) / sizeof(tries[0]); ++i) {
        ESP_RETURN_ON_ERROR(pmic_attach(tries[i].hz), TAG, "add M5PM1");
        vTaskDelay(pdMS_TO_TICKS(tries[i].wait_ms));
        pmic_wake();
        if (pmic_identify(id)) {
            break;
        }
    }
    ESP_RETURN_ON_FALSE(i < sizeof(tries) / sizeof(tries[0]), ESP_ERR_NOT_FOUND, TAG,
                        "M5PM1 does not answer at 0x%02x (woken, 100 and 400 kHz)", PMIC_ADDR);
    uint8_t i2c_cfg = 0xff;
    pmic_read(PMIC_I2C_CFG, &i2c_cfg, 1);
    ESP_LOGI(TAG, "M5PM1 id 0x%02x model 0x%02x hw 0x%02x fw 0x%02x; answered on try %u at %u kHz, I2C_CFG was 0x%02x",
             id[0], id[1], id[2], id[3], (unsigned)(i + 1), (unsigned)(tries[i].hz / 1000), i2c_cfg);
    // No sleep, 100 kHz. The write is acknowledged at the old speed; the PMIC switches after it.
    ESP_RETURN_ON_ERROR(pmic_write(PMIC_I2C_CFG, 0x00), TAG, "I2C_CFG");
    if (tries[i].hz != PMIC_HZ) {
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_RETURN_ON_ERROR(pmic_attach(PMIC_HZ), TAG, "add M5PM1");
    }
    ESP_RETURN_ON_ERROR(pmic_read(PMIC_I2C_CFG, &i2c_cfg, 1), TAG, "read back I2C_CFG");
    ESP_RETURN_ON_FALSE(i2c_cfg == 0x00, ESP_ERR_INVALID_STATE, TAG, "I2C_CFG reads 0x%02x, want 0x00", i2c_cfg);
    return ESP_OK;
}

// Power source, the PMIC's record of why the board last started (then cleared, so the next boot's
// line is about that boot), and its watchdog, which nothing here feeds: off if another firmware left
// it on.
static void log_power_state(void)
{
    uint8_t src = 0xff, wake = 0xff, wdt = 0xff;
    pmic_read(PMIC_PWR_SRC, &src, 1);
    pmic_read(PMIC_WAKE_SRC, &wake, 1);
    pmic_read(PMIC_WDT_CNT, &wdt, 1);
    // Raw: the power-source codes are unconfirmed, and the voltage registers have read implausibly.
    ESP_LOGI(TAG, "M5PM1 power source 0x%02x; started by 0x%02x (%s%s%s%s%s%s%s); watchdog %u s%s",
             src, wake, (wake & 0x40) ? "Grove 5V in, " : "",
             (wake & 0x20) ? "GPIO, " : "", (wake & 0x10) ? "reset command, " : "",
             (wake & 0x08) ? "reset key, " : "", (wake & 0x04) ? "power key, " : "", (wake & 0x02) ? "USB in, " : "",
             (wake & 0x01) ? "timer" : "", (unsigned)wdt, wdt && wdt != 0xff ? ", turning it off" : "");
    if (wake && wake != 0xff) {
        pmic_write(PMIC_WAKE_SRC, 0x00);
    }
    if (wdt && wdt != 0xff && pmic_write(PMIC_WDT_CNT, 0x00) != ESP_OK) {
        ESP_LOGE(TAG, "M5PM1 watchdog still on");
    }
}

static void log_readback(void)
{
    uint8_t od = 0, dir = 0, out = 0, in = 0, pwr = 0;
    const bool ok = pmic_read(PMIC_GPIO_OD, &od, 1) == ESP_OK && pmic_read(PMIC_GPIO_DIR, &dir, 1) == ESP_OK &&
                    pmic_read(PMIC_GPIO_OUT, &out, 1) == ESP_OK && pmic_read(PMIC_GPIO_IN, &in, 1) == ESP_OK &&
                    pmic_read(PMIC_PWR_CFG, &pwr, 1) == ESP_OK;
    if (!ok) {
        ESP_LOGE(TAG, "M5PM1 readback failed");
        return;
    }
    ESP_LOGI(TAG, "M5PM1: L3B %s, SPK %s (dir 0x%02x out 0x%02x od 0x%02x in 0x%02x pwr 0x%02x), raw battery %d, USB %d",
             (out & PMIC_L3B) && (dir & PMIC_L3B) && !(od & PMIC_L3B) ? "on" : "NOT ON",
             (out & PMIC_SPK) ? "ON" : "off", dir, out, od, in, pwr, pmic_mv(PMIC_VBAT_MV), pmic_mv(PMIC_VIN_MV));
}

esp_err_t pmic_init(i2c_master_bus_handle_t bus)
{
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_STATE, TAG, "no I2C bus");
    s_bus = bus;
    ESP_RETURN_ON_ERROR(pmic_connect(), TAG, "connect");

    // The reference's order: push-pull for L3B and SPK, both outputs, amp off, then the LCD and
    // audio rail on; 5 V out off (nothing here uses it); LED off (over a milliamp around the clock,
    // and the screen shows the state).
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_OD, PMIC_L3B | PMIC_SPK, false), TAG, "push-pull");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_DIR, PMIC_L3B | PMIC_SPK, true), TAG, "direction");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_OUT, PMIC_SPK, false), TAG, "amp off");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_OUT, PMIC_L3B, true), TAG, "LCD and audio power");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_PWR_CFG, PMIC_PWR_5V_OUT, false), TAG, "5 V out off");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_PWR_CFG, PMIC_PWR_LED, false), TAG, "LED off");
    vTaskDelay(pdMS_TO_TICKS(20));
    log_readback();
    log_power_state();
    return ESP_OK;
}

esp_err_t pmic_speaker(bool on)
{
    ESP_RETURN_ON_FALSE(s_pmic, ESP_ERR_INVALID_STATE, TAG, "not set up");
    return pmic_update(PMIC_GPIO_OUT, PMIC_SPK, on);
}
