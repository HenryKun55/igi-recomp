#!/usr/bin/env python3
"""Add the port's "Vita" options screen to the game's menus.

The port exposes its run-time settings (shim/src/vita_settings.c) through the
game's own menu widgets. This script edits the compiled menu scripts
(menusystem/*.qvm, via tools/qvm/qvm.py) to

  - add a new MenuScreen (id 980) built from the game's ToggleBox and ListBox
    widgets, one row per setting, whose Get/Set/Fill scripts are the Vita_*
    script functions the runtime registers (runtime/src/vita_menu.c);
  - add a "Vita" entry to the Options/Configuration screen that opens it, in
    both mainmenu.qvm (main menu) and ingamemenu.qvm (pause menu).

The row titles are menu texts ("LOCAL:<lang>/Vita_<key>") added to every
language's menusystem.res (tools/res/resfile.py), so they are translated like
the rest of the game; the ListBox value labels come from the runtime (English
short labels such as 16:9 / Drag / Zoom / x1.0).

The originals are kept as <file>.orig the first time; running again rebuilds
from them, so the script is idempotent.

Usage:
  build_menu.py PC_DIR          # edit the menus and texts in PC_DIR
  build_menu.py PC_DIR --check  # validate only, write nothing
"""
import argparse
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "res"))
sys.path.insert(0, os.path.join(HERE, "..", "qvm"))
import resfile  # noqa: E402
import qvm  # noqa: E402

VITA_SCREEN = 980   # id of the new MenuScreen (clear of the game's 900..969)
FIRST_WIDGET = 980  # first widget id (each row uses one)

# One row per setting, in screen order. kind: "toggle" or "list". `key` is the
# row-title text key (LOCAL:<lang>/Vita_<key>); `fn` names the Vita_* script
# functions (runtime/src/vita_menu.c). The English title is the fallback text.
ROWS = [
    ("list",   "Aspect",       "Aspect",       "Aspect ratio"),
    ("list",   "TouchAim",     "TouchAim",     "Touch aim"),
    ("toggle", "Buttons",      "Buttons",      "On-screen buttons"),
    ("list",   "Rear",         "Rear",         "Rear touchpad"),
    ("toggle", "FrontTouch",   "FrontTouch",   "Front touch"),
    ("list",   "Sensitivity",  "Sensitivity",  "Look sensitivity"),
    ("toggle", "Overlay",      "Overlay",      "Performance overlay"),
    ("toggle", "UpdateCheck",  "UpdateCheck",  "Check for updates"),
]

# Row-title translations (cp1252). English is the key's fallback; other
# languages fall back to English when absent here.
TITLES = {
    "Aspect":      {"english": "Aspect ratio",        "portuguese": "Propor\xe7\xe3o"},
    "TouchAim":    {"english": "Touch aim",           "portuguese": "Mira por toque"},
    "Buttons":     {"english": "On-screen buttons",   "portuguese": "Bot\xf5es na tela"},
    "Rear":        {"english": "Rear touchpad",       "portuguese": "Touch traseiro"},
    "FrontTouch":  {"english": "Front touch",         "portuguese": "Toque frontal"},
    "Sensitivity": {"english": "Look sensitivity",    "portuguese": "Sensib. da mira"},
    "Overlay":     {"english": "Performance overlay", "portuguese": "Painel de desemp."},
    "UpdateCheck": {"english": "Check for updates",   "portuguese": "Buscar atualiz."},
}
# The screen's own title.
SCREEN_TITLE = {"english": "Vita Settings", "portuguese": "Ajustes do Vita"}

LANGS = ("english", "french", "german", "italian", "spanish", "USA", "portuguese")


class Error(Exception):
    pass


# ---- qvm helpers ------------------------------------------------------------

I = qvm.Ident
S = qvm.Str


def menuframe(name, x, y, w, h, lineup, xsp, ysp, selectable, *kids):
    return qvm.Call("Task_New", [-1, S("MenuFrame"), S(name), S(""), x, y, w, h,
                                 lineup, xsp, ysp, I("TRUE" if selectable else "FALSE"), *kids])


def menutext(key, click="", font="font3.fnt", colour=2, sound=""):
    # MenuText(id, "MenuText", internalName, TextKey, Font, Colour, Click, Sound):
    # the text looked up in menusystem.res is the TextKey (arg index 3).
    return qvm.Call("Task_New", [-1, S("MenuText"), S(""), S(key), S(font), colour, S(click), S(sound)])


def row(kind, title_key, fn, wid, y):
    """A label + widget row (a selectable MenuFrame), matching the Controls rows."""
    label = menutext(title_key)
    if kind == "toggle":
        widget = qvm.Call("Task_New", [wid, S("ToggleBox"), S(""),
                                       S("Vita_Get%s()" % fn), S("Vita_Set%s(this.isOn)" % fn)])
    else:
        widget = qvm.Call("Task_New", [wid, S("ListBox"), S(""), S("slide"), S("font1.fnt"), 150,
                                       S("Vita_Fill%s()" % fn), S("Vita_Get%s()" % fn),
                                       S("Vita_Set%s(this.nActiveID)" % fn), S("")])
    return qvm.Call("Task_New", [-1, S("MenuFrame"), S(title_key), S(""), 95, y, 454, 25, 8, 40, 4,
                                 I("TRUE"), label, widget])


def vita_screen(escape_action):
    """The MenuScreen with every setting row."""
    title = menuframe("", 95, 161, 455, 26, 0, 4, 4, False,
                      menutext("Vita Settings", "", "font3.fnt", 1))
    rows = []
    y = 189
    wid = FIRST_WIDGET
    for kind, title_key, fn, _en in ROWS:
        rows.append(row(kind, "Vita_" + title_key, fn, wid, y))
        y += 27
        wid += 1
    rows_frame = menuframe("", 95, 185, 455, 26, 1, 4, 2, False, *rows)
    okcancel = menuframe("OK/Cancel", 555, 400, 80, 80, 1, 4, 4, False,
                         menutext("OK", escape_action, "font3.fnt", 2, "menu_ok"))
    return qvm.Call("Task_New", [VITA_SCREEN, S("MenuScreen"), S("Vita Settings"), S("mainmenu.pic"),
                                 100, 163, 540, 470, S(escape_action), S(""), S(""),
                                 title, rows_frame, okcancel])


def add_entry(rows_frame, click):
    """Add a 'Vita' MenuText row to an Options screen's list frame (idempotent)."""
    for kid in rows_frame.args:
        if isinstance(kid, qvm.Call) and len(kid.args) > 6 and kid.args[6] == click:
            return False
    # MenuText(id, "MenuText", textkey, fallback, font, colour, click, sound)
    entry = qvm.Call("Task_New", [-1, S("MenuText"), S("Vita"), S("Vita"), S("font3.fnt"), 2, S(click), S("")])
    rows_frame.args.append(entry)
    return True


def screen_of(script, sid):
    for st in script.statements:
        for c in qvm.walk(st):
            if c.name == "Task_New" and c.args and c.args[0] == sid and \
                    len(c.args) > 1 and c.args[1] == "MenuScreen":
                return c
    return None


def manager_of(script):
    """The MenuManager call that owns the screens (top-level Task_New)."""
    for st in script.statements:
        if isinstance(st, qvm.Call) and st.name == "Task_New" and len(st.args) > 1 and st.args[1] == "MenuManager":
            return st
    raise Error("no MenuManager in script")


def rows_frame_of(screen):
    """The list frame of an Options screen: the MenuFrame with the row entries
    (the one with several MenuText kids that push/request screens)."""
    best = None
    for a in screen.args:
        if isinstance(a, qvm.Call) and a.name == "Task_New" and len(a.args) > 1 and a.args[1] == "MenuFrame":
            kids = [k for k in a.args if isinstance(k, qvm.Call) and k.name == "Task_New"
                    and len(k.args) > 1 and k.args[1] in ("MenuText", "MenuTextConditional")]
            if len(kids) >= 2:
                best = a
    if best is None:
        raise Error("no row frame in screen")
    return best


def patch_main_menu(script):
    """mainmenu.qvm: screen 904 Configuration -> add Vita, append screen 980."""
    conf = screen_of(script, 904)
    if conf is None:
        raise Error("Configuration screen 904 not found in mainmenu.qvm")
    add_entry(rows_frame_of(conf), "MenuManager_RequestScreen(%d, FALSE)" % VITA_SCREEN)
    mgr = manager_of(script)
    if not screen_of(script, VITA_SCREEN):
        mgr.args.append(vita_screen("MenuManager_RequestScreen(904, FALSE)"))


def patch_ingame_menu(script):
    """ingamemenu.qvm: the pause screen 900 -> add Vita, append screen 980."""
    pause = screen_of(script, 900)
    if pause is None:
        raise Error("pause screen 900 not found in ingamemenu.qvm")
    add_entry(rows_frame_of(pause), "MenuManager_PushScreen(%d)\n" % VITA_SCREEN)
    mgr = manager_of(script)
    if not screen_of(script, VITA_SCREEN):
        mgr.args.append(vita_screen("MenuManager_PopScreen(FALSE)"))


# ---- .res helpers (row titles) ---------------------------------------------

def add_titles(blob, lang):
    """Add the Vita row titles and the Vita entry label to a menusystem.res."""
    res = resfile.ResFile.parse(blob)
    prefix = None
    for name, c in res.resources():
        if c.tag == b"PATH":
            prefix = name
    if prefix is None:
        raise Error("no PATH resource in menusystem.res")

    wanted = {}
    for _kind, key, _fn, en in ROWS:
        wanted["Vita_" + key] = TITLES[key].get(lang, TITLES[key]["english"])
    wanted["Vita"] = {"portuguese": "Vita"}.get(lang, "Vita")
    wanted["Vita Settings"] = SCREEN_TITLE.get(lang, SCREEN_TITLE["english"])

    for shortkey, text in wanted.items():
        full = "%s/%s" % (prefix, shortkey)
        for name, c in res.resources():
            if c.tag == b"CSTR" and name.lower() == full.lower():
                c.data = resfile.cstr(text)
                c.pad = None
                break
        else:
            at = len(res.chunks) - 2   # before the trailing PATH name/data
            res.chunks[at:at] = [resfile.Chunk(b"NAME", resfile.cstr(full)),
                                 resfile.Chunk(b"CSTR", resfile.cstr(text))]
    names = [name for name, c in res.resources() if c.tag == b"CSTR"]
    for name, c in res.resources():
        if c.tag == b"PATH":
            c.data = ";".join(names).encode(resfile.ENCODING) + b"\0"
            c.pad = None
    return res.build()


# ---- driver -----------------------------------------------------------------

def patch_qvm(path, patch):
    orig = path + ".orig"
    if not os.path.exists(orig):
        shutil.copyfile(path, orig)
    script = qvm.read(orig)
    patch(script)
    data = qvm.build(script)
    # round-trip check
    again = qvm.build(qvm.parse(data))
    if again != data:
        raise Error("%s: re-encode is not stable" % path)
    return data


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("pc", help="game data directory (the one with IGI.exe)")
    ap.add_argument("--check", action="store_true", help="validate only, write nothing")
    args = ap.parse_args(argv[1:])
    pc = args.pc
    ms = os.path.join(pc, "menusystem")
    if not os.path.exists(os.path.join(ms, "mainmenu.qvm")):
        raise Error("%s: no menusystem/mainmenu.qvm" % pc)

    main_data = patch_qvm(os.path.join(ms, "mainmenu.qvm"), patch_main_menu)
    ingame_data = patch_qvm(os.path.join(ms, "ingamemenu.qvm"), patch_ingame_menu)

    # The row titles live in each language's menusystem.res (keyed
    # LOCAL:<lang>/Vita_...); that is where the menu system resolves menu text.
    res_out = {}
    for lang in LANGS:
        path = os.path.join(pc, "language", lang, "menusystem.res")
        if os.path.exists(path):
            with open(path, "rb") as f:
                res_out[path] = add_titles(f.read(), lang)
    if not res_out:
        raise Error("no language/<lang>/menusystem.res found")

    if args.check:
        print("menu OK (%d res files)" % len(res_out))
        return 0

    with open(os.path.join(ms, "mainmenu.qvm"), "wb") as f:
        f.write(main_data)
    with open(os.path.join(ms, "ingamemenu.qvm"), "wb") as f:
        f.write(ingame_data)
    for path, data in res_out.items():
        with open(path, "wb") as f:
            f.write(data)
    print("added the Vita options screen to mainmenu.qvm and ingamemenu.qvm (%d res files)" % len(res_out))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except Error as e:
        print("error: %s" % e, file=sys.stderr)
        sys.exit(1)
