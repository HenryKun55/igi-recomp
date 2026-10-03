# Project I.G.I.: I'm Going In — native PS Vita port

Project I.G.I. (Innerloop, 2000) running natively on the PS Vita / PS TV. This
is not an emulator: the original Windows executable was statically recompiled
to C and built for the Vita, with Direct3D 7 / DirectSound / DirectInput
reimplemented on top of vitaGL and the Vita's own APIs. Hot parts of the game
(terrain, meshes, skinning, culling...) have been rewritten by hand for the
Vita's CPU.

**The download contains no game files.** You need your own copy of the PC game.

## What works

- The whole game: menus, all missions, saves, options (saved as soon as you
  confirm them).
- Locked 30 fps in gameplay (the game's own frame cap), CPU clocked to 444 MHz
  automatically.
- Sound, music, FMV cutscenes and in-engine cutscenes.
- The game's loading screens, LiveArea bubble, 16:9 fullscreen (4:3 optional).

## Requirements

1. A PS Vita or PS TV with HENkaku/Ensō (any firmware supported by them) and
   VitaShell.
2. **libshacccg.suprx** (the runtime shader compiler) in `ur0:data/`.
   If you already play other vitaGL ports you have it. Otherwise, extract it
   with [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D).
3. **The PC game files** from the original 2000 release. The port is built for
   this exact `IGI.exe`:
   - size: 1,384,448 bytes
   - SHA-1: `e4daed6ab0feb7ca5614a0d61fa8aa6c14bd8164`

   Other versions/patches of `IGI.exe` are not supported yet. If yours doesn't
   match, the game will not start.

## Installation

1. Install `igi.vpk` with VitaShell.
2. On your PC, open the folder where the game is installed and find the folder
   that contains `IGI.exe` (it is called `pc`, next to folders like
   `missions`, `language`, `menusystem`, `weapons`...). It is about 480 MB.
3. Copy that whole folder to the Vita so you end up with:

   ```
   ux0:data/igi/pc/IGI.exe
   ux0:data/igi/pc/missions/...
   ux0:data/igi/pc/language/...
   ux0:data/igi/pc/menusystem/...
   ...
   ```

   (USB or FTP with VitaShell; the `.url` shortcut files can be skipped.)
4. Launch **Project IGI** from the LiveArea.

Updating to a newer version only needs the new vpk; your game data and saves in
`ux0:data/igi/` are kept.

## Controls

| Vita | Action |
| --- | --- |
| Left stick | Move / strafe |
| Right stick | Look / aim |
| R | Fire |
| L | Crouch |
| Cross | Jump |
| Circle | Use / activate |
| Square | Reload / confirm in menus |
| Triangle | Next weapon |
| D-pad left | Previous weapon |
| D-pad up | Binoculars |
| D-pad down | Walk / run |
| D-pad right | Map computer |
| Select | Peek |
| Start | Menu / back |

The controls follow the game's default key bindings: if you changed the key
bindings in the game's options, set them back to the defaults.

## Optional settings

Create `ux0:data/igi/igi.cfg` (a text file, one setting per line):

| Setting | Effect |
| --- | --- |
| `aspect=keep` | Original 4:3 picture with black bars (default: fill the 16:9 screen) |
| `overlay=1` | Performance overlay (frame time, CPU time per frame) |

## Known issues

- Only the `IGI.exe` above is supported.
- The log is written to `ux0:data/igi/igi.log`; please include it when
  reporting a problem.
