/* Game-specific fixes driven by recompiler entry hooks. */
#ifndef IGI_FIXES_H
#define IGI_FIXES_H

#include <stdint.h>

/* True while the 2D queue is drawing the cutscene window that must stay hidden. */
int igi_skip_untextured_2d(void);
/* Called once per presented frame. */
void igi_end_frame(void);

/* State field of the UI window rendered last (5/2/6/0 menus, 16 in-engine
 * cutscene, 9 gameplay), or IGI_UI_NONE when none was drawn in the last frames. */
#define IGI_UI_NONE 0xFFFFFFFFu
uint32_t igi_ui_state(void);

#endif
