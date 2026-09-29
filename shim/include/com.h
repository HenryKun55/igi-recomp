/* Minimal COM object model for the DirectX shims.
 *
 * A guest-visible object is 16 bytes of guest memory: vtable pointer, host
 * object index, reference count, and one spare word. Its vtable is an array of
 * host thunk addresses, so guest `call [vtbl + n*4]` lands in host C code.
 */
#ifndef IGI_COM_H
#define IGI_COM_H

#include "shim.h"

#define S_OK 0u
#define S_FALSE 1u
#define E_NOINTERFACE 0x80004002u
#define E_NOTIMPL 0x80004001u
#define E_FAIL 0x80004005u
#define E_INVALIDARG 0x80070057u
#define E_OUTOFMEMORY 0x8007000Eu
#define REGDB_E_CLASSNOTREG 0x80040154u

typedef struct ComMethod {
    const char *name;
    GuestFn fn;          /* NULL: registered as a named "not implemented" stub */
} ComMethod;

/* Build a guest vtable. Methods with fn == NULL fail loudly by name when called. */
uint32_t com_vtable(const char *iface, const ComMethod *methods, int count);

/* Create an object; `host` is returned by com_host() for method implementations. */
uint32_t com_new(uint32_t vtable, void *host);
void *com_host(uint32_t obj);
uint32_t com_addref(uint32_t obj);
/* Decrement; returns the new count. The caller frees host data when it reaches 0. */
uint32_t com_release(uint32_t obj);

typedef struct Guid {
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t d4[8];
} Guid;

void guid_read(uint32_t addr, Guid *g);
int guid_eq(const Guid *g, const char *text);   /* text: "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" */
const char *guid_str(const Guid *g);

/* `this` pointer of the COM method being executed. */
#define THIS ARG(0)

#endif
