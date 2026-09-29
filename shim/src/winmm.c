/* winmm.dll: CD audio (MCI) and the volume mixer. Neither exists here; the game
 * falls back to running without CD music and without mixer volume control. */
#include "shim.h"

#define MMSYSERR_NODRIVER 6u
#define MCIERR_DEVICE_OPEN 263u

SHIM(winmm, mciSendCommandA)
{
    rt_log("mciSendCommandA(dev %u, msg %03X): no CD audio device", ARG(0), ARG(1));
    stdcall_ret(c, MCIERR_DEVICE_OPEN, 4);
}

SHIM(winmm, mixerGetNumDevs) { stdcall_ret(c, 0, 0); }
SHIM(winmm, mixerOpen) { stdcall_ret(c, MMSYSERR_NODRIVER, 5); }
SHIM(winmm, mixerClose) { stdcall_ret(c, 0, 1); }
SHIM(winmm, mixerGetDevCapsA) { stdcall_ret(c, MMSYSERR_NODRIVER, 3); }
SHIM(winmm, mixerGetLineInfoA) { stdcall_ret(c, MMSYSERR_NODRIVER, 3); }
SHIM(winmm, mixerGetLineControlsA) { stdcall_ret(c, MMSYSERR_NODRIVER, 3); }
SHIM(winmm, mixerGetControlDetailsA) { stdcall_ret(c, MMSYSERR_NODRIVER, 3); }
SHIM(winmm, mixerSetControlDetails) { stdcall_ret(c, MMSYSERR_NODRIVER, 3); }
