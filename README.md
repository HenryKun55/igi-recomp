# igi-recomp

A static recompilation of **Project I.G.I.: I'm Going In** (Innerloop, 2000) from
its x86 Windows executable to portable C. It runs natively on **macOS** and on the
**PlayStation Vita**, and is being decompiled into hand-written C one function at
a time.

No game data is included. You need your own copy of the game: the build
recompiles *your* `IGI.exe`, and the game reads its data from your installation.

## Status

- Full game playable on macOS and on the Vita (real hardware and Vita3K).
- Vita: a stable 30 fps in gameplay at 444 MHz, with the game's own loading
  screen, options saved as soon as a menu is confirmed, LiveArea bubble and
  in-engine AVI cutscenes (Indeo 5 through a minimal FFmpeg).
- An extra **Português (Brasil)** language, listed first on the language screen.
- Hot spots of the recompiled code have been replaced by native C
  (`runtime/src/native_*.c`), each one checked against the recompiled original.

## How it works

1. `tools/recomp` disassembles `IGI.exe` (Capstone), finds its functions and
   translates every one of them to C. The generated code runs against a flat
   32-bit guest memory image, with an exact x87 model where the game needs it.
2. `runtime/` loads the executable image, runs the generated code and holds the
   port's fixes and the hand-decompiled functions.
3. `shim/` reimplements the parts of Windows the game uses: kernel32/user32,
   DirectDraw/Direct3D 7 (on OpenGL, vitaGL on the Vita), DirectSound, DirectInput
   (keyboard/mouse on the desktop, controller on the Vita), AVI video and files.

## Building

Requirements: CMake 3.20+, Ninja, Python 3 with `capstone` (and `Pillow` for
the Vita build), and your game installation, whose `pc` directory (the one with
`IGI.exe`) is expected at `gamedata/pc` (or pass `-DIGI_EXE=/path/to/IGI.exe`).

### macOS

```sh
brew install cmake ninja sdl2 ffmpeg pkg-config   # ffmpeg: optional, for the cutscenes
pip3 install capstone
cmake -S . -B build -G Ninja
cmake --build build
build/igi gamedata/pc
```

### PlayStation Vita

Needs [VitaSDK](https://vitasdk.org). The private vitaGL build (pinned commit
plus the patches in `tools/vitagl/`) and the minimal FFmpeg are optional but
recommended:

```sh
tools/build_vitagl_vita.sh
tools/build_ffmpeg_vita.sh
cmake -S . -B build-vita -G Ninja -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake --build build-vita
```

Install `build-vita/igi.vpk` and copy the game's `pc` directory to
`ux0:data/igi/pc`. The LiveArea pictures are made from the game's own art at
configure time, so they are not part of this repository either.

Optional settings go in `ux0:data/igi/igi.cfg` (one `key=value` per line, e.g.
`overlay=1` for the performance overlay, `update_check=0` to skip the update
check at start-up). Releases and the in-game updater: `docs/VITA_RELEASE.md`.

## Brazilian Portuguese

The translation lives in `translations/pt-br/` as plain text, one entry per
game text key. To install it into a game directory:

```sh
tools/install_language.sh gamedata/pc
```

It builds `language/portuguese/` from the English files plus the translation,
adds the entry to the language screen and keeps the originals. The runtime
enables the language when those files are present.

## Tools

| Tool | Purpose |
| --- | --- |
| `tools/recomp/` | Recompiler, plus difference tests of native functions against the recompiled ones |
| `tools/res/resfile.py` | List, extract and repack the game's `.res` (ILFF) archives |
| `tools/qvm/qvm.py` | Decode and re-encode compiled menu/config scripts (`.qvm`) |
| `tools/bench_mac.sh`, `tools/bench_vita3k.sh` | Automated in-game benchmarks |
| `tools/profile_vita3k.sh` | Guest profile of the Vita build under Vita3K |

## Legal

This is an unofficial fan project, not affiliated with or endorsed by the
rights holders of Project I.G.I. It contains no game code or assets: the
recompiled code is generated locally from the user's own copy. The Portuguese
translation is an unofficial fan translation.

The code in this repository is under the MIT license (see `LICENSE`), except
the patches in `tools/vitagl/`, which modify vitaGL and follow its license
(LGPL-3.0). The Vita build links vitaGL and FFmpeg, both LGPL.
