/* avifil32.dll + msvfw32.dll: AVI cutscenes (Indeo Video 5).
 * No decoder yet: opening a movie fails and the game skips it. */
#include "shim.h"

#define AVIERR_FILEOPEN 0x80044067u

SHIM(avifil32, AVIFileInit) { stdcall_ret(c, 0, 0); }
SHIM(avifil32, AVIFileExit) { stdcall_ret(c, 0, 0); }

SHIM(avifil32, AVIFileOpenA)
{
    rt_log("AVIFileOpenA(%s): video playback not supported yet", gstr(ARG(1)));
    WR32(ARG(0), 0);
    stdcall_ret(c, AVIERR_FILEOPEN, 4);
}

SHIM(avifil32, AVIFileRelease) { stdcall_ret(c, 0, 1); }
SHIM(avifil32, AVIStreamRelease) { stdcall_ret(c, 0, 1); }

SHIM(msvfw32, ICLocate) { stdcall_ret(c, 0, 5); }
SHIM(msvfw32, ICClose) { stdcall_ret(c, 0, 1); }
