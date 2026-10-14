/* See runtime/include/vita_menu.h.
 *
 * The port's Vita options rows use the game's own menu widgets, which read and
 * write their value by calling a native "script function" by name. This module
 * registers those functions for the port.
 *
 * A registered function's pointer is a host thunk address (runtime: thunk_register,
 * dispatched by rt_dispatch); the interpreter CALLs it with the VM calling
 * convention the game's own handlers use (confirmed by decoding them):
 *
 *     void handler(void *argFrame, ValueCell *retOut);   // __cdecl
 *
 *   - args: 0x4B8A20(argFrame, i) reads arg i as an int, 0x4B8A50(argFrame, i)
 *     as a double;
 *   - return: a 24-byte ValueCell at retOut, value as a double at +0x0C and the
 *     VM "number" type tag 0x567C74 at +0x14 (a void return is a zeroed cell
 *     with that tag).
 *
 * Each row reads a 0-based choice index and writes it back; the labels per
 * choice come from menusystem.res (added by tools/vita_menu/build_menu.py), so
 * the menu text is translated like the rest of the game. The settings
 * themselves (and igi.cfg) are owned by shim/src/vita_settings.c. */
#include "vita_menu.h"

#include "recomp.h"
#include "runtime.h"
#include "vita_settings.h"

#include <string.h>

#define DEFINE_GAME_OPTIONS 0x00402FF0u
#define GAME_REGISTER_FUNC  0x004B8890u   /* register(name, fnptr, 0, argCount) */
#define GAME_ARG_INT        0x004B8A20u   /* int  arg(argFrame, i) */
#define GAME_ARG_FLOAT      0x004B8A50u   /* float arg(argFrame, i) (double in st0) */
#define GAME_ADD_LISTITEM   0x0041F2D0u   /* AddListBoxItem(const char *name, int id) */
#define VM_NUMBER_TAG       0x00567C74u   /* ValueCell type tag for a number */
/* The 24-byte ValueCell as the game's own handlers build it (decoded from
 * Config_GameOptionsInputGetInvertMouse): the value is stored BOTH as an int at
 * +0x00 (read by the integer evaluator 0x4B9120, e.g. for a ToggleBox's isOn)
 * AND as a double at +0x08 (read by the float evaluator 0x4B9660, e.g. for a
 * ListBox's nActiveID), with the "number" type descriptor at +0x10. */
#define CELL_INT_OFF        0x00u
#define CELL_VALUE_OFF      0x08u
#define CELL_TAG_OFF        0x10u
#define CELL_SIZE           0x18u

/* A row is a toggle (two states, a ToggleBox: bool get/set) or a list (a few
 * named values, a ListBox: a Fill that names each value and an integer index
 * get/set). The value labels shown in a ListBox are the port's own English
 * short labels (16:9, Drag, Zoom, x1.0, ...); the row's title is a menu text
 * (menusystem.res) so it is translated like the rest of the game. */
typedef enum RowKind { ROW_TOGGLE, ROW_LIST } RowKind;

/* Expected bytes at the two addresses the port uses, so it only runs on the
 * executable they were found in (DefineGameOptions: "sub esp, 0x500"; the
 * registrar: "mov eax, [esp+0x10]"). */
static int expected_exe(void)
{
    return RD32(DEFINE_GAME_OPTIONS) == 0x0500EC81u /* 81 EC 00 05 00 00 */ &&
           RD32(GAME_REGISTER_FUNC) == 0x1024448Bu; /* 8B 44 24 10 ... */
}

/* ---- the rows: each is a (setting, kind) pair registered under a name ---- */

typedef struct Row {
    VitaSetting setting;
    RowKind kind;
    const char *get_name, *set_name, *fill_name;   /* fill_name NULL for a toggle */
} Row;

static const Row rows[] = {
    { VS_ASPECT,           ROW_LIST,   "Vita_GetAspect",      "Vita_SetAspect",      "Vita_FillAspect" },
    { VS_TOUCH_MODE,       ROW_LIST,   "Vita_GetTouchAim",    "Vita_SetTouchAim",    "Vita_FillTouchAim" },
    { VS_ONSCREEN_BUTTONS, ROW_TOGGLE, "Vita_GetButtons",     "Vita_SetButtons",     NULL },
    { VS_REAR,             ROW_LIST,   "Vita_GetRear",        "Vita_SetRear",        "Vita_FillRear" },
    { VS_FRONT_TOUCH,      ROW_TOGGLE, "Vita_GetFrontTouch",  "Vita_SetFrontTouch",  NULL },
    { VS_SENSITIVITY,      ROW_LIST,   "Vita_GetSensitivity", "Vita_SetSensitivity", "Vita_FillSensitivity" },
    { VS_OVERLAY,          ROW_TOGGLE, "Vita_GetOverlay",     "Vita_SetOverlay",     NULL },
    { VS_UPDATE_CHECK,     ROW_TOGGLE, "Vita_GetUpdateCheck", "Vita_SetUpdateCheck", NULL },
};
#define N_ROWS (int)(sizeof(rows) / sizeof(rows[0]))

/* thunk address -> which row and which operation. */
static struct {
    uint32_t get_addr, set_addr, fill_addr;
} row_thunk[N_ROWS];

/* Guest copies of each list value's English label, for AddListBoxItem. */
#define MAX_CHOICES 8
static uint32_t choice_label_ptr[N_ROWS][MAX_CHOICES];

static int registered;

/* Return from the handler. The evaluator (0x4B9660, native-call case) reads the
 * result as *(double *)(eax + 8), so the handler must return the cell pointer
 * in eax; it is cdecl, so only the return address is popped (the interpreter
 * cleans the args it pushed). */
static void handler_return(Ctx *c, uint32_t ret_cell)
{
    c->r[0] = ret_cell;
    c->r[4] += 4u;
}

/* Fill the cell at `ret` with `value`, stored as both an int (+0x00) and a
 * double (+0x08) so either evaluator reads it, with the number type tag, as the
 * game's own handlers do. */
static void write_number(uint32_t ret, double value)
{
    if (!ret)
        return;
    for (uint32_t i = 0; i < CELL_SIZE; i++)
        g_mem[ret + i] = 0;
    WR32(ret + CELL_INT_OFF, (uint32_t)(int32_t)value);
    WRF64(ret + CELL_VALUE_OFF, value);
    WR32(ret + CELL_TAG_OFF, VM_NUMBER_TAG);
}

/* The handler ABI, confirmed by decoding the game's Config_* handlers: the
 * interpreter CALLs the handler with the return-value cell pointer as the first
 * stack argument ([esp+4]) and the argument frame (read with 0x4B8A20/0x4B8A50)
 * at [esp+0xC]. The cell is a 24-byte number; args are coerced by those two
 * readers. The handler is cdecl (the interpreter cleans the args). */
#define RET_CELL(c) RD32((c)->r[4] + 4u)
#define ARG_FRAME(c) RD32((c)->r[4] + 0x0Cu)

static void do_get(Ctx *c, int r)
{
    uint32_t ret = RET_CELL(c);
    write_number(ret, (double)vita_setting_get(rows[r].setting));
    handler_return(c, ret);
}

static void do_set(Ctx *c, int r)
{
    uint32_t ret = RET_CELL(c), arg_frame = ARG_FRAME(c);
    /* A ListBox passes the item id (an int); a ToggleBox passes a bool. Both
     * come through the int reader (the game's own ToggleBox setters read a bool
     * the same way). */
    const uint32_t args[2] = { arg_frame, 0u };
    int value = (int)guest_call(c, GAME_ARG_INT, 2, args);
    vita_setting_set(rows[r].setting, value);
    write_number(ret, 0.0);
    /* guest_call restores esp; our own frame is intact. */
    handler_return(c, ret);
}

/* A ListBox Fill: name each choice (id = its index) through the game's
 * AddListBoxItem, so the widget shows the current value as text. */
static void do_fill(Ctx *c, int r)
{
    uint32_t ret = RET_CELL(c);
    int n = vita_setting_count(rows[r].setting);
    for (int i = 0; i < n && i < MAX_CHOICES; i++) {
        const uint32_t args[2] = { choice_label_ptr[r][i], (uint32_t)i };
        guest_call(c, GAME_ADD_LISTITEM, 2, args);
    }
    write_number(ret, 0.0);
    handler_return(c, ret);
}

/* One dispatcher: find the calling thunk (g_current_thunk) in the table. */
static void dispatch(Ctx *c)
{
    uint32_t t = g_current_thunk;
    for (int r = 0; r < N_ROWS; r++) {
        if (t == row_thunk[r].get_addr) {
            do_get(c, r);
            return;
        }
        if (t == row_thunk[r].set_addr) {
            do_set(c, r);
            return;
        }
        if (t == row_thunk[r].fill_addr) {
            do_fill(c, r);
            return;
        }
    }
    rt_log("vita_menu: unknown thunk %08X", t);
    handler_return(c, RET_CELL(c));
}

/* Register one script function (name -> thunk) with the game's registrar. */
static uint32_t register_func(Ctx *c, uint32_t sp, const char *name, uint32_t argc)
{
    uint32_t thunk = thunk_register_unique(dispatch, name);
    uint32_t name_ptr = scratch_strdup(name);
    const uint32_t args[4] = { name_ptr, thunk, 0u, argc };
    const uint32_t saved = c->r[4];
    c->r[4] = sp - 64u;   /* below the caller's frame */
    guest_call(c, GAME_REGISTER_FUNC, 4, args);
    c->r[4] = saved;
    return thunk;
}

void vita_menu_probe(Ctx *c, uint32_t func, uint32_t sp)
{
    if (func != DEFINE_GAME_OPTIONS || registered)
        return;
    if (!expected_exe()) {
        rt_log("vita_menu: unexpected executable, options rows not registered");
        registered = 1;
        return;
    }
    registered = 1;
    vita_settings_init();
    for (int r = 0; r < N_ROWS; r++) {
        row_thunk[r].get_addr = register_func(c, sp, rows[r].get_name, 0u);
        row_thunk[r].set_addr = register_func(c, sp, rows[r].set_name, 1u);
        if (rows[r].kind == ROW_LIST) {
            row_thunk[r].fill_addr = register_func(c, sp, rows[r].fill_name, 0u);
            int n = vita_setting_count(rows[r].setting);
            for (int i = 0; i < n && i < MAX_CHOICES; i++)
                /* The value as the ListBox shows it: the port's English label. */
                choice_label_ptr[r][i] = scratch_strdup(vita_setting_choice_label(rows[r].setting, i));
        }
    }
    rt_log("vita_menu: %d option rows registered", N_ROWS);
}
