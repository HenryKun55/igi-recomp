/* Gameplay update rate (runtime/src/tick_rate.c). */
#ifndef IGI_TICK_RATE_H
#define IGI_TICK_RATE_H

#include <stdint.h>

/* igi.cfg game_hz / IGI_GAME_HZ (30..240), 0 when the game keeps its 30 Hz. */
uint32_t tick_game_hz(void);
/* Startup, once the executable is loaded: fills the constant slots. */
void tick_rate_init(void);
/* SetRate(hz) called from return address ret: returns the rate to use. */
uint32_t tick_set_rate(uint32_t ret, uint32_t hz);
/* Gameplay runs at a rate other than 30 Hz (the update-rate fixes are on). */
int tick_rate_active(void);

#endif
