/* Guest-visible COM objects backed by host data. */
#include "com.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#define MAX_OBJECTS 65536u

static void *hosts[MAX_OBJECTS];
static uint32_t next_host = 1;
static uint32_t free_list[MAX_OBJECTS];
static uint32_t free_count;

static void com_not_implemented(Ctx *c)
{
    rt_fatal(c, "COM method not implemented: %s", thunk_name(g_current_thunk));
}

uint32_t com_vtable(const char *iface, const ComMethod *methods, int count)
{
    uint32_t vt = scratch_alloc((uint32_t)count * 4u, 4);
    for (int i = 0; i < count; i++) {
        char *name = malloc(strlen(iface) + strlen(methods[i].name) + 3);
        sprintf(name, "%s::%s", iface, methods[i].name);
        uint32_t t = methods[i].fn ? thunk_register_unique(methods[i].fn, name)
                                   : thunk_register_unique(com_not_implemented, name);
        WR32(vt + (uint32_t)i * 4u, t);
    }
    return vt;
}

uint32_t com_new(uint32_t vtable, void *host)
{
    uint32_t idx = free_count ? free_list[--free_count] : next_host++;
    if (idx >= MAX_OBJECTS)
        rt_fatal(NULL, "too many COM objects");
    hosts[idx] = host;
    uint32_t obj = heap_alloc(16);
    WR32(obj, vtable);
    WR32(obj + 4u, idx);
    WR32(obj + 8u, 1);
    WR32(obj + 12u, 0);
    return obj;
}

void *com_host(uint32_t obj)
{
    uint32_t idx = obj ? RD32(obj + 4u) : 0u;
    return idx < MAX_OBJECTS ? hosts[idx] : NULL;
}

uint32_t com_addref(uint32_t obj)
{
    uint32_t n = RD32(obj + 8u) + 1u;
    WR32(obj + 8u, n);
    return n;
}

uint32_t com_release(uint32_t obj)
{
    uint32_t n = RD32(obj + 8u);
    if (n)
        n--;
    WR32(obj + 8u, n);
    if (n == 0) {
        uint32_t idx = RD32(obj + 4u);
        hosts[idx] = NULL;
        free_list[free_count++] = idx;
        WR32(obj, 0);
        heap_free(obj);
    }
    return n;
}

void guid_read(uint32_t addr, Guid *g)
{
    g->d1 = RD32(addr);
    g->d2 = (uint16_t)RD16(addr + 4u);
    g->d3 = (uint16_t)RD16(addr + 6u);
    memcpy(g->d4, gptr(addr + 8u), 8);
}

const char *guid_str(const Guid *g)
{
    static char buf[4][40];
    static int n;
    char *b = buf[n++ & 3];
    snprintf(b, 40, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", g->d1, g->d2, g->d3,
             g->d4[0], g->d4[1], g->d4[2], g->d4[3], g->d4[4], g->d4[5], g->d4[6], g->d4[7]);
    return b;
}

int guid_eq(const Guid *g, const char *text)
{
    return strcasecmp(guid_str(g), text) == 0;
}
