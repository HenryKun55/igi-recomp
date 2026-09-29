/* ole32.dll: COM runtime. IGI only instantiates DirectMusic, as part of its
 * DirectX version check, so a bare IDirectMusic object is enough. */
#include "com.h"

SHIM(ole32, CoInitialize) { stdcall_ret(c, S_OK, 1); }
SHIM(ole32, CoUninitialize) { stdcall_ret(c, 0, 0); }

static void dmusic_QueryInterface(Ctx *c)
{
    com_addref(THIS);
    WR32(ARG(2), THIS);
    stdcall_ret(c, S_OK, 3);
}

static void dmusic_AddRef(Ctx *c) { stdcall_ret(c, com_addref(THIS), 1); }
static void dmusic_Release(Ctx *c) { stdcall_ret(c, com_release(THIS), 1); }

static const ComMethod dmusic_methods[] = {
    { "QueryInterface", dmusic_QueryInterface },
    { "AddRef", dmusic_AddRef },
    { "Release", dmusic_Release },
    { "EnumPort", NULL },
    { "CreateMusicBuffer", NULL },
    { "CreatePort", NULL },
    { "EnumMasterClock", NULL },
    { "GetMasterClock", NULL },
    { "SetMasterClock", NULL },
    { "Activate", NULL },
    { "GetDefaultPort", NULL },
    { "SetDirectSound", NULL },
    { "SetExternalMasterClock", NULL },
};

SHIM(ole32, CoCreateInstance)
{
    Guid clsid, iid;
    guid_read(ARG(0), &clsid);
    guid_read(ARG(3), &iid);
    if (guid_eq(&clsid, "636B9F10-0C7D-11D1-95B2-0020AFDC7421")) {   /* CLSID_DirectMusic */
        static uint32_t vtbl;
        if (!vtbl)
            vtbl = com_vtable("IDirectMusic", dmusic_methods, (int)(sizeof(dmusic_methods) / sizeof(dmusic_methods[0])));
        WR32(ARG(4), com_new(vtbl, NULL));
        stdcall_ret(c, S_OK, 5);
        return;
    }
    rt_log("CoCreateInstance(%s, %s): class not available", guid_str(&clsid), guid_str(&iid));
    WR32(ARG(4), 0);
    stdcall_ret(c, REGDB_E_CLASSNOTREG, 5);
}
