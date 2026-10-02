/* Extra game language (Brazilian Portuguese).
 *
 * IGI.exe keeps its languages in a table of 16-byte names at 0x53331C:
 * ENGLISH, GERMAN, FRENCH, ITALIAN, SPANISH, USA (index 0..5). The index is
 * stored in the player config (GOGameLang(<NAME>); in pc/config.qvm, parsed as
 * a script constant) and the name picks the data directory
 * "LOCAL:language/<NAME>/" (the "LANGUAGE:" alias: movies, messages, objectives,
 * computer, readme). The menu system loads menusystem.res and missions.res of
 * every language and looks its texts up as "LOCAL:<NAME>/<key>".
 *
 * The port adds PORTUGUESE as index 6 when language/portuguese/menusystem.res
 * exists (tools/install_language.sh builds that directory and adds the entry to
 * the language screen of menusystem/mainmenu.qvm):
 *   - 0x406310 (number of languages) and 0x406320 (name of language i) are
 *     replaced (PATCHES table in tools/recomp/gen.py);
 *   - the two places that index the table inline (the LANGUAGE: alias at
 *     0x405B6E and the config writer at 0x405D6A) only pass the name to
 *     sprintf, so the sprintf entry hook swaps the out-of-table pointer;
 *   - the script constant PORTUGUESE = 6 is registered next to the others
 *     (0x4B8810(name, value), called for each name by 0x4035A3).
 * Without the data, index 6 (e.g. from a config written with it) falls back to
 * English. */
#include "recomp.h"
#include "runtime.h"

#include <string.h>

#define RET(esp, eax) (((uint64_t)(esp) << 32) | (uint32_t)(eax))

#define LANG_TABLE      0x0053331Cu   /* char name[6][16] */
#define LANG_COUNT      6u
#define LANG_EXTRA      6u            /* index of the added language */
#define LANG_PAST_TABLE (LANG_TABLE + 16u * LANG_EXTRA)
#define LANG_USA_NAME   (LANG_TABLE + 16u * 5u)

#define FMT_LANGUAGE_DIR 0x0053831Cu  /* "LOCAL:language/%s/" */
#define FMT_CONFIG_LINE  0x00538380u  /* "%s(%s);\n" */
#define STR_GOGAMELANG   0x005332A8u  /* "GOGameLang" */

#define GAME_FILE_EXISTS     0x004B5B60u   /* int exists(const char *path) */
#define GAME_REGISTER_CONST  0x004B8810u   /* void register(const char *name, int value) */

static const char extra_name[] = "PORTUGUESE";
static const char extra_probe[] = "LOCAL:language/portuguese/menusystem.res";

static uint32_t extra_name_ptr;
static int extra_state = -1;   /* -1 unknown, 0 missing, 1 installed */

static int expected_exe(void)
{
    return memcmp(g_mem + LANG_TABLE, "ENGLISH", 8) == 0 && memcmp(g_mem + LANG_USA_NAME, "USA", 4) == 0 &&
           memcmp(g_mem + FMT_LANGUAGE_DIR, "LOCAL:language/%s/", 19) == 0;
}

static uint32_t name_ptr(void)
{
    if (!extra_name_ptr)
        extra_name_ptr = scratch_strdup(extra_name);
    return extra_name_ptr;
}

/* Whether the extra language's data is installed (checked once, through the
 * game's own file lookup so that its path aliases apply). */
static int extra_available(Ctx *c, uint32_t esp)
{
    if (extra_state < 0) {
        if (!expected_exe()) {
            extra_state = 0;
            return 0;
        }
        const uint32_t path = scratch_strdup(extra_probe);
        const uint32_t saved_esp = c->r[4];
        c->r[4] = esp - 64u;   /* below the caller's frame */
        extra_state = guest_call(c, GAME_FILE_EXISTS, 1, &path) != 0;
        c->r[4] = saved_esp;
        rt_log("language: %s %s", extra_name, extra_state ? "installed" : "not installed");
    }
    return extra_state;
}

/* Name of language `index` as the game's table would give it. */
static uint32_t language_name(Ctx *c, uint32_t esp, uint32_t index)
{
    if (index == LANG_EXTRA)
        return extra_available(c, esp) ? name_ptr() : LANG_TABLE;
    return LANG_TABLE + 16u * index;
}

/* 0x406310: int language_count(void) { return 6; } */
int rt_lang_count(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)eax;
    c->r[1] = ecx;
    *ret = RET(esp + 4u, LANG_COUNT + (uint32_t)extra_available(c, esp));
    return 1;
}

/* 0x406320: const char *language_name(int i) { return table[i]; } */
int rt_lang_name(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)eax;
    c->r[1] = ecx;
    *ret = RET(esp + 4u, language_name(c, esp, RD32(esp + 4u)));
    return 1;
}

void igi_language_probe(Ctx *c, uint32_t func, uint32_t sp)
{
    switch (func) {
    case 0x004A53B3u:   /* sprintf(buf, fmt, ...) */
        if (RD32(sp + 8u) == FMT_LANGUAGE_DIR && RD32(sp + 12u) == LANG_PAST_TABLE)
            WR32(sp + 12u, language_name(c, sp, LANG_EXTRA));
        else if (RD32(sp + 8u) == FMT_CONFIG_LINE && RD32(sp + 12u) == STR_GOGAMELANG &&
                 RD32(sp + 16u) == LANG_PAST_TABLE)
            WR32(sp + 16u, language_name(c, sp, LANG_EXTRA));
        break;
    case GAME_REGISTER_CONST:   /* register(name, value): add ours after USA */
        if (RD32(sp + 4u) == LANG_USA_NAME && RD32(sp + 8u) == 5u && expected_exe()) {
            const uint32_t args[2] = { name_ptr(), LANG_EXTRA };
            guest_call(c, GAME_REGISTER_CONST, 2, args);
        }
        break;
    default:
        break;
    }
}
