#!/usr/bin/env python3
"""Build an extra game language directory from the English data plus a
translation kept in the repository (translations/<code>/*.txt).

The game reads its texts from pc/language/<name>/*.res (tools/res/resfile.py):
computer, menusystem, messages, missions, movies and objectives. Each text
is a CSTR resource with a key; in menusystem.res and missions.res the keys
carry the language ("LOCAL:english/Main Menu") and a PATH resource lists
them. The translation files hold, in the same order as the English files:

    [key]                 key without the "LOCAL:english/" prefix
    translated text       one line; \\t = tab, \\n = new line, \\\\ = backslash

Lines starting with '#' before a key are comments. A key that appears twice
in the English file (a few do) appears twice here too.

The script also
  - copies the English default controls (config.qvm) and uses
    translations/<code>/readme.txt as ReadMe.txt when present;
  - adds the language's own name ("<label key>") to menusystem.res of every
    other installed language, so their language screens can show it;
  - adds the entry, first in the list, to the language screen of
    pc/menusystem/mainmenu.qvm (the original is kept as mainmenu.qvm.orig);
  - checks that every character exists in the game's fonts.
Running it again rebuilds everything from the original files.

Usage:
  build_language.py PC_DIR [--src translations/pt-br] [--lang portuguese]
                    [--const PORTUGUESE] [--label-key Portuguese]
                    [--label "Português (Brasil)"]
  build_language.py PC_DIR --review FILE   # English and translation side by side
  build_language.py PC_DIR --check         # validate only, write nothing
"""
import argparse
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "res"))
sys.path.insert(0, os.path.join(HERE, "..", "qvm"))
import resfile  # noqa: E402
import qvm  # noqa: E402

RES_FILES = ("computer", "menusystem", "messages", "missions", "movies", "objectives")
SOURCE_LANG = "english"
OTHER_LANGS = ("english", "french", "german", "italian", "spanish", "USA")
FONT_FILES = ("menusystem/menusystem.res", "menusystem/ingamemenu.res", "computer/computer.res")


class Error(Exception):
    pass


def unescape(s, where):
    out = []
    i = 0
    while i < len(s):
        ch = s[i]
        if ch == "\\":
            if i + 1 >= len(s) or s[i + 1] not in "tn\\":
                raise Error("%s: bad escape in %r" % (where, s))
            out.append({"t": "\t", "n": "\n", "\\": "\\"}[s[i + 1]])
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def escape(s):
    return s.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")


def read_translation(path):
    """[(key, text, line)] in file order."""
    entries = []
    key = None
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            where = "%s:%d" % (path, n)
            if key is None:
                if not line.strip() or line.startswith("#"):
                    continue
                if not (line.startswith("[") and line.endswith("]")):
                    raise Error("%s: expected [key], got %r" % (where, line))
                key = line[1:-1]
                continue
            entries.append((key, unescape(line, where), n))
            key = None
    if key is not None:
        raise Error("%s: key [%s] without text" % (path, key))
    return entries


def short_key(name, lang):
    prefix = "LOCAL:%s/" % lang
    return name[len(prefix):] if name.lower().startswith(prefix.lower()) else name


def font_charset(pc):
    """Characters every game font can draw (cp1252 bytes with a glyph)."""
    common = None
    for rel in FONT_FILES:
        path = os.path.join(pc, rel)
        if not os.path.exists(path):
            continue
        for name, c in resfile.read(path).resources():
            if not (c.data[:4] == b"ILFF" and c.data[16:20] == b"FONT"):
                continue
            chunks = {x.tag: x.data for x in resfile.ResFile.parse(c.data).chunks}
            tran = struct.unpack("<256H", chunks[b"TRAN"])
            have = {i for i in range(256) if tran[i]}
            common = have if common is None else common & have
    return common


def check_chars(text, charset, where):
    if charset is None:
        return
    for ch in text:
        if ch in "\t\n":
            continue
        try:
            b = ch.encode(resfile.ENCODING)
        except UnicodeEncodeError:
            raise Error("%s: %r is not in the game's code page" % (where, ch))
        if b[0] not in charset and ch != " ":
            raise Error("%s: the game fonts have no glyph for %r" % (where, ch))


def build_res(src_path, entries, src_lang, lang, charset, tr_path, label_key):
    """Translated copy of an English .res file (without the label an earlier run added)."""
    res = resfile.read(src_path)
    drop_label(res, src_lang, label_key)
    texts = [(name, c) for name, c in res.resources() if c.tag == b"CSTR"]
    if len(texts) != len(entries):
        raise Error("%s: %d entries, the English file has %d" % (tr_path, len(entries), len(texts)))
    for (name, c), (key, text, line) in zip(texts, entries):
        if short_key(name, src_lang) != key:
            raise Error("%s:%d: [%s] where the English file has [%s]" % (tr_path, line, key, short_key(name, src_lang)))
        check_chars(text, charset, "%s:%d" % (tr_path, line))
        c.data = resfile.cstr(text)
        c.pad = None
    rename(res, src_lang, lang)
    return res


def drop_label(res, lang, label_key):
    key = ("LOCAL:%s/%s" % (lang, label_key)).lower()
    for i, c in enumerate(res.chunks):
        if c.tag == b"NAME" and resfile.cstr_text(c.data).lower() == key:
            del res.chunks[i:i + 2]
            return


def rename(res, src_lang, lang):
    """Move LOCAL:<src_lang>/ keys (and the PATH list) to LOCAL:<lang>/."""
    old = "LOCAL:%s" % src_lang
    new = "LOCAL:%s" % lang
    names = []
    for i, c in enumerate(res.chunks):
        if c.tag != b"NAME":
            continue
        name = resfile.cstr_text(c.data)
        if name.lower() == old.lower() or name.lower().startswith(old.lower() + "/"):
            name = new + name[len(old):]
            c.data = resfile.cstr(name)
            c.pad = None
        if res.chunks[i + 1].tag == b"CSTR":
            names.append(name)
    for name, c in res.resources():
        if c.tag == b"PATH":
            c.data = ";".join(names).encode(resfile.ENCODING) + b"\0"
            c.pad = None


def add_label(blob, where, label_key, label):
    """Add or replace LOCAL:<lang>/<label_key> in a language's menusystem.res."""
    res = resfile.ResFile.parse(blob)
    prefix = None
    for name, c in res.resources():
        if c.tag == b"PATH":
            prefix = name
    if prefix is None:
        raise Error("%s: no PATH resource" % where)
    key = "%s/%s" % (prefix, label_key)
    for name, c in res.resources():
        if c.tag == b"CSTR" and name.lower() == key.lower():
            c.data = resfile.cstr(label)
            c.pad = None
            break
    else:
        # insert before the PATH name/data pair, which stays last
        at = len(res.chunks) - 2
        res.chunks[at:at] = [resfile.Chunk(b"NAME", resfile.cstr(key)), resfile.Chunk(b"CSTR", resfile.cstr(label))]
    names = [name for name, c in res.resources() if c.tag == b"CSTR"]
    for name, c in res.resources():
        if c.tag == b"PATH":
            c.data = ";".join(names).encode(resfile.ENCODING) + b"\0"
            c.pad = None
    return res.build()


def patch_menu(script, label_key, const):
    """Insert the language entry first in the language screen's list."""
    action = "MenuManager_SetLanguage(%s)" % const
    for call in (n for st in script.statements for n in qvm.walk(st)):
        items = [a for a in call.args if isinstance(a, qvm.Call) and len(a.args) >= 7 and
                 isinstance(a.args[6], str) and a.args[6].startswith("MenuManager_SetLanguage(")]
        if not items:
            continue
        if any(a.args[6] == action for a in items):
            return False
        first = call.args.index(items[0])
        entry = qvm.Call(items[0].name, list(items[0].args))
        entry.args[2] = qvm.Str(label_key)
        entry.args[3] = qvm.Str(label_key)
        entry.args[6] = qvm.Str(action)
        call.args.insert(first, entry)
        # MenuFrame(id, type, name, picture, x, y, width, height, ...): one more row
        if isinstance(call.args[7], int) and len(items) > 0:
            call.args[7] = call.args[7] + call.args[7] // len(items)
        return True
    raise Error("language screen not found in mainmenu.qvm")


def review(pc, src, name):
    entries = read_translation(os.path.join(src, name + ".txt"))
    res = resfile.read(os.path.join(pc, "language", SOURCE_LANG, name + ".res"))
    texts = [(n, c) for n, c in res.resources() if c.tag == b"CSTR"]
    for (n, c), (key, text, _) in zip(texts, entries):
        print("[%s]\n  en: %s\n  pt: %s" % (key, escape(resfile.cstr_text(c.data)), escape(text)))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("pc", help="game data directory (the one with IGI.exe)")
    ap.add_argument("--src", default=os.path.join(HERE, "..", "..", "translations", "pt-br"))
    ap.add_argument("--lang", default="portuguese", help="directory under pc/language (lower case)")
    ap.add_argument("--const", default="PORTUGUESE", help="script constant the runtime registers")
    ap.add_argument("--label-key", default="Portuguese")
    ap.add_argument("--label", default="Português (Brasil)")
    ap.add_argument("--check", action="store_true", help="validate the translation only")
    ap.add_argument("--review", metavar="FILE", help="print English and translation of FILE (e.g. messages)")
    args = ap.parse_args(argv[1:])
    pc = args.pc
    if args.review:
        review(pc, args.src, args.review)
        return 0
    if not os.path.exists(os.path.join(pc, "language", SOURCE_LANG, "messages.res")):
        raise Error("%s: no language/%s (not the game's pc directory?)" % (pc, SOURCE_LANG))
    charset = font_charset(pc)
    outputs = {}
    for name in RES_FILES:
        tr_path = os.path.join(args.src, name + ".txt")
        entries = read_translation(tr_path)
        res = build_res(os.path.join(pc, "language", SOURCE_LANG, name + ".res"), entries, SOURCE_LANG, args.lang,
                        charset, tr_path, args.label_key)
        outputs[name + ".res"] = res.build()
    check_chars(args.label, charset, "--label")
    outputs["menusystem.res"] = add_label(outputs["menusystem.res"], "menusystem.res", args.label_key, args.label)
    readme_src = os.path.join(args.src, "readme.txt")
    if os.path.exists(readme_src):
        with open(readme_src, encoding="utf-8") as f:
            readme = f.read()
        check_chars(readme, charset, readme_src)
        outputs["ReadMe.txt"] = readme.replace("\r\n", "\n").replace("\n", "\r\n").encode(resfile.ENCODING)
    if args.check:
        print("translation OK (%d files)" % len(outputs))
        return 0

    out_dir = os.path.join(pc, "language", args.lang)
    os.makedirs(out_dir, exist_ok=True)
    for name, data in outputs.items():
        with open(os.path.join(out_dir, name), "wb") as f:
            f.write(data)
    shutil.copyfile(os.path.join(pc, "language", SOURCE_LANG, "config.qvm"), os.path.join(out_dir, "config.qvm"))
    if "ReadMe.txt" not in outputs:
        shutil.copyfile(os.path.join(pc, "language", SOURCE_LANG, "ReadMe.txt"), os.path.join(out_dir, "ReadMe.txt"))
    print("wrote %s" % out_dir)

    for lang in OTHER_LANGS:
        path = os.path.join(pc, "language", lang, "menusystem.res")
        if os.path.exists(path):
            with open(path, "rb") as f:
                data = add_label(f.read(), path, args.label_key, args.label)
            with open(path, "wb") as f:
                f.write(data)
    print("language name added to the menu texts")

    menu = os.path.join(pc, "menusystem", "mainmenu.qvm")
    orig = menu + ".orig"
    if not os.path.exists(orig):
        shutil.copyfile(menu, orig)
    script = qvm.read(orig)
    patch_menu(script, args.label_key, args.const)
    data = qvm.build(script)
    with open(menu, "wb") as f:
        f.write(data)
    print("language screen entry added to %s" % menu)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except Error as e:
        print("error: %s" % e, file=sys.stderr)
        sys.exit(1)
