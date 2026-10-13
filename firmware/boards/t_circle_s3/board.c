// SPDX-License-Identifier: Apache-2.0
// LilyGO T-Circle-S3: PDM mic and MAX98357A (board_audio.c), CST816D touch and BOOT
// (board_touch.c), GC9D01 panel (panel.c). Calls, the screen and BOOT holds are shared
// (voice/call.c, ui/display.c, main.c).
#include "board.h"
#include "board_audio.h"
#include "board_input.h"
#include "call.h"
#include "display.h"

#include "esp_log.h"

static const char *TAG = "main";

void board_init(void)
{
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
    call_run_ui("tap the screen to call, tap again to hang up; hold BOOT 3 s to pair, 10 s to reset");
}
