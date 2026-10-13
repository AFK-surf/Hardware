// M5Stack StickS3: M5PM1 power chip (pmic.c), ES8311 audio (board_audio.c), KEY1/KEY2
// (board_input.c), ST7789 135x240 LCD (panel.c). Calls, the screen and KEY2 holds are shared
// (voice/call.c, ui/display.c, main.c): KEY1 is the call key, KEY2 the board's BOOT.
#include "board.h"
#include "board_audio.h"
#include "board_input.h"
#include "board_pins.h"
#include "call.h"
#include "display.h"
#include "pmic.h"

#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "main";
static i2c_master_bus_handle_t s_i2c;

i2c_master_bus_handle_t stick_i2c_bus(void)
{
    return s_i2c;
}

static void i2c_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = STICK_I2C_SDA,
        .scl_io_num = STICK_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    const esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        s_i2c = NULL;
    }
}

static const char *reset_reason(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "eFuse";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    default: return "unknown";
    }
}

void board_init(void)
{
    // With the PMIC's own record (pmic.c), says why the board restarted.
    const esp_reset_reason_t why = esp_reset_reason();
    ESP_LOGI(TAG, "reset reason: %s (%d)", reset_reason(why), (int)why);
    // The PMIC first: the LCD and the codec have no power until it is set.
    i2c_init();
    const esp_err_t pmic = pmic_init(s_i2c);
    if (pmic != ESP_OK) {
        ESP_LOGE(TAG, "M5PM1 setup failed: %s; screen and audio may stay off", esp_err_to_name(pmic));
    }
    const esp_err_t audio = board_audio_init();
    if (audio != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(audio));
    }
    call_voice_clean_init();
    board_input_init();
    call_init();
    ESP_ERROR_CHECK(display_init());
}

void board_run_ui(void *arg)
{
    (void)arg;
    call_run_ui("press the front key to call, again to hang up; hold the side key 3 s to pair, 10 s to reset");
}
