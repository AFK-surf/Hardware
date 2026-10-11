#pragma once
#include "sdkconfig.h"
typedef enum { BOARD_PAIRING_OFF, BOARD_PAIRING_ADVERTISING,
               BOARD_PAIRING_CONNECTED, BOARD_PAIRING_SAVED } board_pairing_state_t;
void board_init(void);
void board_start_feedback(void);
void board_run_ui(void *arg);
void board_show_card(const char *label, const char *big, const char *body);
void board_show_idle(void);
void board_wake(void);
void board_pairing_state(board_pairing_state_t state);
