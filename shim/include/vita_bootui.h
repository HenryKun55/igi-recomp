/* PS Vita start-up screen: the port's version and the update check over the boot
 * picture, before the game starts. See vita_bootui.c. */
#ifndef VITA_BOOTUI_H
#define VITA_BOOTUI_H

#include <stdint.h>

/* framebuffer: the 960x544 ABGR display buffer showing the boot picture. Returns when
 * the game can start (or does not return: an update is being installed). */
void vita_bootui_run(uint32_t *framebuffer);

#endif
