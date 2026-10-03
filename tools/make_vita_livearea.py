#!/usr/bin/env python3
"""Make the Vita LiveArea pictures from the game's own art (run by CMake for the Vita build).

The loading screen picture (LOCAL:menusystem/loading.pic in
pc/menusystem/loadingscreen.res: the "I.G.I" logo over the agent and an
explosion, 640x480) becomes:
  sce_sys/icon0.png                       128x128  bubble icon (logo and agent)
  sce_sys/pic0.png                        960x544  shown while the app starts
  sce_sys/livearea/contents/bg.png        840x500  LiveArea background, with the credits
  sce_sys/livearea/contents/startup.png   280x158  LiveArea start gate (logo)
All are 8-bit palette PNGs, as the LiveArea expects.

The pictures are derived from the retail data, so they are not kept in the
repo: CMake writes them into <build>/sce_sys next to a copy of
platform/vita/sce_sys (the LiveArea template).

usage: tools/make_vita_livearea.py [game pc directory] [sce_sys directory] [port version]
       (defaults: gamedata/pc, build-vita/sce_sys, no version on the LiveArea)
Needs Pillow. The credits use Oswald (platform/vita/fonts, SIL Open Font License).
"""
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, 'platform', 'vita', 'fonts', 'Oswald.ttf')

# At the bottom of the LiveArea, so everyone who opens the bubble knows what this is.
# The version is the one installed from the .vpk: an update made from the game replaces
# its files but not sce_sys, so after one the boot screen has the current version.
CREDITS = [
    'Project I.G.I. for PS Vita  -  PS Vita port by HenryKun55, made with love',
    'Project I.G.I. (c) Eidos Interactive / Innerloop Studios. Not affiliated with them or Sony.',
    'Needs your own copy of the game.',
]


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


def draw_credits(img, version):
    """The credit lines, centred at the bottom over a band that darkens from above."""
    lines = list(CREDITS)
    if version:
        lines[-1] += '  v' + version
    font = ImageFont.truetype(FONT, 16)
    font.set_variation_by_name('Regular')
    img = img.convert('RGBA')
    w, h = img.size
    line, top = 21, h - 21 * len(lines) - 10
    band = Image.new('L', (w, h), 0)
    bd = ImageDraw.Draw(band)
    for y in range(top - 24, h):
        bd.line([(0, y), (w, y)], fill=min(190, max(0, (y - (top - 24)) * 190 // 24)))
    img.paste(Image.new('RGBA', (w, h), (0, 0, 0, 255)), (0, 0), band)
    d = ImageDraw.Draw(img)
    for i, text in enumerate(lines):
        tw = d.textlength(text, font=font)
        x, y = (w - tw) / 2, top + i * line
        d.text((x + 1, y + 1), text, font=font, fill=(0, 0, 0, 255))
        d.text((x, y), text, font=font, fill=(255, 205, 120, 255) if i == 0 else (225, 225, 225, 255))
    return img.convert('RGB')


def save_png8(img, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.quantize(colors=256, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG).save(path, optimize=True)
    print('wrote %s (%dx%d)' % (path, img.width, img.height))


def main():
    game = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'gamedata', 'pc')
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'build-vita', 'sce_sys')
    version = sys.argv[3] if len(sys.argv) > 3 else ''
    with open(os.path.join(game, 'menusystem', 'loadingscreen.res'), 'rb') as f:
        res = res_entries(f.read())
    art = decode_pic(res['LOCAL:menusystem/loading.pic'], 10)   # 640x480: 10 x 15 tiles of 64x32
    if art.size != (640, 480):
        raise ValueError('unexpected loading.pic size %dx%d' % art.size)

    # The loading picture fades to black at its edges, so black bars blend in.
    save_png8(fit(art, (960, 544)), os.path.join(out, 'pic0.png'))
    # The background a little higher, so the credits sit on its black bottom edge.
    bg = Image.new('RGB', (840, 500))
    bg.paste(fit(art, (840, 500)), (0, -22))
    save_png8(draw_credits(bg, version), os.path.join(out, 'livearea', 'contents', 'bg.png'))
    # The logo ("PROJECT I.G.I" with the agent's silhouette) on its black band.
    logo = art.crop((180, 30, 460, 188))   # 280x158
    save_png8(logo, os.path.join(out, 'livearea', 'contents', 'startup.png'))
    # Icon: the logo over the agent and the explosion, a little down from the top (black
    # padding above the picture) so the round bubble does not cut it.
    icon = art.crop((176, -6, 464, 282)).resize((128, 128), Image.LANCZOS)
    save_png8(icon, os.path.join(out, 'icon0.png'))


if __name__ == '__main__':
    main()
