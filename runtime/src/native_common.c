/* Switches of the hand-decompiled game functions (runtime/include/native.h). */
#include "native.h"

/* IGI_NATIVE=0 / native=0 in igi.cfg (runtime/src/main.c): all decline. */
int g_igi_native = 1;
NativeHook g_native_hook;
uint32_t g_native_decline;
