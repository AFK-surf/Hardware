// Render the firmware's text cards on the host, for checking layout on the round glass.
//
//   cc -std=c11 -O2 -Imain tools/card_preview.c main/ui_card.c main/ui_font.c -lm -o build/card_preview
//   build/card_preview build-preview/cards
//
// Writes one 8-bit PGM per card (gray level = LUT index * 17).

#include <stdio.h>
#include <string.h>

#include "ui_card.h"

static const ui_card_t CARDS[] = {
    {"Hello", "", "Open the Comma app to set up Comma-5D28"},
    {"Pairing mode", "", "Open the Comma app to set up Comma-5D28"},
    {"Pairing code", "482 019", "Enter it on your phone"},
    {"Paired", "", "Choose a Wi-Fi network in the app"},
    {"Connecting to", "", "ToEverything-IOT"},
    {"Connecting to", "", "A really long network name here"},
    {"Wi-Fi failed", "", "Wrong password. Try again."},
    {"Wi-Fi failed", "", "Network not found. Only 2.4 GHz works."},
    {"Wi-Fi failed", "", "No answer from the network. Try again."},
    {"Wi-Fi connected", "", "Finishing setup in the app"},
    {"All set", "", "Tap to call Comma"},
    {"Pairing failed", "", "Try again from the app"},
    {"Let go to pair", "7", "Keep holding to reset"},
    {"Resetting", "", "Erasing Wi-Fi and credentials"},
    {"Key rejected", "", "Hold BOOT 3 s and set up again in the app"},
    {"No Wi-Fi", "", "Still trying to connect"},
};

int main(int argc, char **argv)
{
    const char *prefix = argc > 1 ? argv[1] : "card";
    uint16_t lut[16];
    for (int i = 0; i < 16; ++i) {
        lut[i] = (uint16_t)i;
    }
    static uint16_t fb[UI_CARD_SIZE * UI_CARD_SIZE];
    for (size_t n = 0; n < sizeof(CARDS) / sizeof(CARDS[0]); ++n) {
        ui_card_render(&CARDS[n], fb, lut);
        char path[256];
        snprintf(path, sizeof(path), "%s-%02zu.pgm", prefix, n);
        FILE *f = fopen(path, "wb");
        if (!f) {
            perror(path);
            return 1;
        }
        fprintf(f, "P5\n%d %d\n255\n", UI_CARD_SIZE, UI_CARD_SIZE);
        for (int i = 0; i < UI_CARD_SIZE * UI_CARD_SIZE; ++i) {
            fputc(fb[i] * 17, f);
        }
        fclose(f);
    }
    return 0;
}
