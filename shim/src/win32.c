/* Win32 environment setup shared by the API shims. */
#include "platform.h"
#include "shim.h"

void kernel32_init(void);

void win32_init(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    platform_init();
    kernel32_init();
}
