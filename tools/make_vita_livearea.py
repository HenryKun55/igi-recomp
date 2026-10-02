#!/usr/bin/env python3
"""Make the Vita LiveArea pictures (platform/vita/sce_sys) from the game's own art.

The loading screen picture (LOCAL:menusystem/loading.pic in
pc/menusystem/loadingscreen.res: the "I.G.I" logo over the agent and an
explosion, 640x480) becomes:
  sce_sys/icon0.png                       128x128  bubble icon (logo and agent)
  sce_sys/pic0.png                        960x544  shown while the app starts
  sce_sys/livearea/contents/bg.png        840x500  LiveArea background
  sce_sys/livearea/contents/startup.png   280x158  LiveArea start gate (logo)
All are 8-bit palette PNGs, as the LiveArea expects.

The generated files are committed: they are derived from the retail data of the
user's own copy of the game, like the recompiled code.

usage: tools/make_vita_livearea.py [game pc directory] [sce_sys directory]
       (defaults: gamedata/pc and platform/vita/sce_sys)
Needs Pillow.
"""
import os
import struct
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def res_entries(data):
    """{name: body} of an ILFF resource archive (.res)."""
    if data[0:4] != b'ILFF' or data[16:20] != b'IRES':
        raise ValueError('not an ILFF/IRES archive')
    out = {}
    pos, name = 20, None
    while pos + 16 <= len(data):
        tag, size, _align, nxt = struct.unpack_from('<4sIII', data, pos)
        body = data[pos + 16:pos + 16 + size]
        if tag == b'NAME':
            name = body.split(b'\0', 1)[0].decode('ascii')
        elif tag == b'BODY':
            out[name] = body
        if not nxt:
            break
        pos += nxt
    return out


def decode_pic(body, tiles_per_row):
    """A .pic: a table of 32-bit BGRA tiles (offset, format 3, width, height), row-major."""
    if body[0:4] != b'LOOP':
        raise ValueError('not a .pic')
    count, = struct.unpack_from('<I', body, 0x20)
    tiles = []
    for i in range(count):
        off, fmt, _flags, w, h, _pitch = struct.unpack_from('<IIHHHH', body, 0x34 + i * 0x20)
        if fmt != 3:
            raise ValueError('tile format %d not handled' % fmt)
        tiles.append(Image.frombytes('RGBA', (w, h), body[off:off + w * h * 4], 'raw', 'BGRA'))
    tw, th = tiles[0].size
    rows = (count + tiles_per_row - 1) // tiles_per_row
    img = Image.new('RGBA', (tw * tiles_per_row, th * rows))
    for i, t in enumerate(tiles):
        img.paste(t, ((i % tiles_per_row) * tw, (i // tiles_per_row) * th))
    return img.convert('RGB')


def fit(img, size, background=(0, 0, 0)):
    """img scaled to fit size (aspect kept), centered on a background."""
    w, h = size
    s = min(w / img.width, h / img.height)
    scaled = img.resize((round(img.width * s), round(img.height * s)), Image.LANCZOS)
    out = Image.new('RGB', size, background)
    out.paste(scaled, ((w - scaled.width) // 2, (h - scaled.height) // 2))
    return out


def save_png8(img, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.quantize(colors=256, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG).save(path, optimize=True)
    print('wrote %s (%dx%d)' % (path, img.width, img.height))


def main():
    game = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'gamedata', 'pc')
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'platform', 'vita', 'sce_sys')
    with open(os.path.join(game, 'menusystem', 'loadingscreen.res'), 'rb') as f:
        res = res_entries(f.read())
    art = decode_pic(res['LOCAL:menusystem/loading.pic'], 10)   # 640x480: 10 x 15 tiles of 64x32
    if art.size != (640, 480):
        raise ValueError('unexpected loading.pic size %dx%d' % art.size)

    # The loading picture fades to black at its edges, so black bars blend in.
    save_png8(fit(art, (960, 544)), os.path.join(out, 'pic0.png'))
    save_png8(fit(art, (840, 500)), os.path.join(out, 'livearea', 'contents', 'bg.png'))
    # The logo ("PROJECT I.G.I" with the agent's silhouette) on its black band.
    logo = art.crop((180, 30, 460, 188))   # 280x158
    save_png8(logo, os.path.join(out, 'livearea', 'contents', 'startup.png'))
    # Icon: the logo over the top of the agent and the explosion.
    icon = art.crop((176, 32, 464, 320)).resize((128, 128), Image.LANCZOS)
    save_png8(icon, os.path.join(out, 'icon0.png'))


if __name__ == '__main__':
    main()
