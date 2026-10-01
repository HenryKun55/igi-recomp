/* Update-rate A/B harness (shim/src/tick_test.c, desktop only). */
#ifndef IGI_TICK_TEST_H
#define IGI_TICK_TEST_H

#include <stdint.h>

/* IGI_TICK_TEST=PREFIX set. */
int tick_test_enabled(void);
/* SetRate (0x402820) called from return address ret with the rate it will use. */
void tick_test_set_rate(uint32_t ret, uint32_t hz);
/* Entry of the scheduler (0x402260): drives input and records once per step. */
void tick_test_scheduler(void);
/* File name for a screenshot of the frame being presented, or NULL. */
const char *tick_test_shot_due(void);

#endif
