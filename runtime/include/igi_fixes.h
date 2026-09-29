/* Game-specific fixes driven by recompiler entry hooks. */
#ifndef IGI_FIXES_H
#define IGI_FIXES_H

#include <stdint.h>

/* True while the 2D queue is drawing the cutscene window that must stay hidden. */
int igi_skip_untextured_2d(void);
/* Called once per presented frame. */
void igi_end_frame(void);

#endif
