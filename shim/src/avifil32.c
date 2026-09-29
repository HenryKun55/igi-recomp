/* avifil32.dll + msvfw32.dll: AVI cutscenes.
 *
 * The AVI container is parsed here; frames are decoded through video.h
 * (Indeo Video 5 via libavcodec). The game drives playback itself: it reads
 * compressed frames with AVIStreamRead, decompresses them with ICDecompress
 * into a DIB and plays the matching .wav through DirectSound.
 */
#include "ddraw_priv.h"
#include "shim.h"
#include "video.h"

#include <stdio.h>
#include <stdlib.h>

#define AVIERR_OK 0u
#define AVIERR_FILEOPEN 0x80044067u
#define AVIERR_NODATA 0x80044071u
#define ICERR_OK 0u
#define ICERR_BADFORMAT 0xFFFFFFFEu

typedef struct AviIndex {
    uint32_t offset, size;
} AviIndex;

typedef struct AviFile {
    uint8_t *data;
    size_t size;
    uint32_t guest_handle;
    uint32_t stream_handle;
    /* avih / strh */
    uint32_t micro_per_frame, width, height, frames;
    uint32_t scale, rate, fcc_handler;
    uint8_t strf[64];
    uint32_t strf_size;
    AviIndex *index;
    uint32_t index_count;
} AviFile;

static AviFile *files[16];
/* Size of the last frame handed to the game, so ICDecompress gets the exact length. */
static uint32_t last_read_buf, last_read_size;

static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static AviFile *file_by_handle(uint32_t h)
{
    for (int i = 0; i < 16; i++)
        if (files[i] && (files[i]->guest_handle == h || files[i]->stream_handle == h))
            return files[i];
    return NULL;
}

/* Walk RIFF chunks; collects headers, the movi base and idx1. */
static int parse_avi(AviFile *f)
{
    const uint8_t *d = f->data;
    size_t n = f->size;
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "AVI ", 4))
        return -1;
    size_t movi = 0;
    const uint8_t *idx1 = NULL;
    uint32_t idx1_size = 0;
    /* Iterative walk over the top-level list and the hdrl/strl sub-lists. */
    size_t stack_end[4];
    size_t off = 12;
    int depth = 0;
    stack_end[0] = n;
    int video_seen = 0;
    while (depth >= 0) {
        if (off + 8 > stack_end[depth]) {
            off = stack_end[depth];
            depth--;
            continue;
        }
        const uint8_t *c = d + off;
        uint32_t sz = rd32le(c + 4);
        size_t next = off + 8 + sz + (sz & 1u);
        if (!memcmp(c, "LIST", 4)) {
            if (!memcmp(c + 8, "movi", 4)) {
                movi = off + 8;         /* idx1 offsets are relative to the 'movi' fourcc */
                off = next;
            } else if (depth < 3) {
                stack_end[++depth] = next < n ? next : n;
                off += 12;
            } else {
                off = next;
            }
            continue;
        }
        if (!memcmp(c, "avih", 4) && sz >= 40) {
            f->micro_per_frame = rd32le(c + 8);
            f->frames = rd32le(c + 8 + 16);
            f->width = rd32le(c + 8 + 32);
            f->height = rd32le(c + 8 + 36);
        } else if (!memcmp(c, "strh", 4) && sz >= 36 && !memcmp(c + 8, "vids", 4) && !video_seen) {
            video_seen = 1;
            f->fcc_handler = rd32le(c + 12);
            f->scale = rd32le(c + 8 + 20);
            f->rate = rd32le(c + 8 + 24);
            f->frames = rd32le(c + 8 + 32);
        } else if (!memcmp(c, "strf", 4) && video_seen && !f->strf_size) {
            f->strf_size = sz < sizeof(f->strf) ? sz : (uint32_t)sizeof(f->strf);
            memcpy(f->strf, c + 8, f->strf_size);
        } else if (!memcmp(c, "idx1", 4)) {
            idx1 = c + 8;
            idx1_size = sz;
        }
        off = next;
    }
    if (!movi || !idx1 || !f->strf_size)
        return -1;
    uint32_t entries = idx1_size / 16u;
    f->index = calloc(entries ? entries : 1u, sizeof(AviIndex));
    /* idx1 offsets are relative to the 'movi' fourcc unless the first one points
     * straight at a chunk from the file start. Decide once for the whole index. */
    size_t base = movi;
    if (entries) {
        uint32_t first = rd32le(idx1 + 8);
        if (first >= movi && first + 4 <= n && !memcmp(d + first, idx1, 4))
            base = 0;
    }
    for (uint32_t i = 0; i < entries; i++) {
        const uint8_t *e = idx1 + i * 16u;
        if (e[0] != '0' || e[1] != '0' || (e[2] != 'd' && e[2] != 'D'))   /* stream 00 video chunks */
            continue;
        size_t abs = base + rd32le(e + 8);
        uint32_t s = rd32le(e + 12);
        if (abs + 8 + s > n)
            continue;
        f->index[f->index_count].offset = (uint32_t)(abs + 8);
        f->index[f->index_count].size = s;
        f->index_count++;
    }
    return 0;
}

SHIM(avifil32, AVIFileInit) { stdcall_ret(c, 0, 0); }
SHIM(avifil32, AVIFileExit) { stdcall_ret(c, 0, 0); }

SHIM(avifil32, AVIFileOpenA)
{
    uint32_t out = ARG(0);
    const char *win = gstr(ARG(1));
    char path[1100];
    win_path_to_host(win, path, sizeof(path));
    WR32(out, 0);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        rt_log("AVIFileOpenA(%s): not found", win);
        stdcall_ret(c, AVIERR_FILEOPEN, 4);
        return;
    }
    AviFile *f = calloc(1, sizeof(*f));
    fseek(fp, 0, SEEK_END);
    f->size = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f->data = malloc(f->size);
    if (fread(f->data, 1, f->size, fp) != f->size || parse_avi(f) != 0) {
        fclose(fp);
        free(f->data);
        free(f->index);
        free(f);
        rt_log("AVIFileOpenA(%s): unsupported AVI", win);
        stdcall_ret(c, AVIERR_FILEOPEN, 4);
        return;
    }
    fclose(fp);
    int slot = -1;
    for (int i = 0; i < 16; i++)
        if (!files[i]) {
            slot = i;
            break;
        }
    if (slot < 0) {
        free(f->data);
        free(f->index);
        free(f);
        stdcall_ret(c, AVIERR_FILEOPEN, 4);
        return;
    }
    files[slot] = f;
    f->guest_handle = heap_alloc(16);
    f->stream_handle = heap_alloc(16);
    rt_log("AVIFileOpenA(%s): %ux%u, %u frames, %u/%u fps, codec %.4s", win, f->width, f->height, f->index_count,
           f->rate, f->scale, (const char *)&f->fcc_handler);
    WR32(out, f->guest_handle);
    stdcall_ret(c, AVIERR_OK, 4);
}

SHIM(avifil32, AVIFileInfoA)
{
    AviFile *f = file_by_handle(ARG(0));
    uint32_t p = ARG(1), size = ARG(2);
    memset(gptr(p), 0, size);
    if (f && size >= 44u) {
        WR32(p + 12u, 1);                 /* dwStreams */
        WR32(p + 20u, f->width);
        WR32(p + 24u, f->height);
        WR32(p + 28u, f->scale);
        WR32(p + 32u, f->rate);
        WR32(p + 36u, f->index_count);
    }
    stdcall_ret(c, f ? AVIERR_OK : AVIERR_NODATA, 3);
}

SHIM(avifil32, AVIFileGetStream)
{
    AviFile *f = file_by_handle(ARG(0));
    uint32_t out = ARG(1), type = ARG(2), n = ARG(3);
    int video = type == 0u || type == 0x73646976u;   /* 'vids' */
    if (!f || !video || n != 0u) {
        WR32(out, 0);
        stdcall_ret(c, AVIERR_NODATA, 4);
        return;
    }
    WR32(out, f->stream_handle);
    stdcall_ret(c, AVIERR_OK, 4);
}

SHIM(avifil32, AVIStreamInfoA)
{
    AviFile *f = file_by_handle(ARG(0));
    uint32_t p = ARG(1), size = ARG(2);
    memset(gptr(p), 0, size);
    if (f) {
        WR32(p + 0u, 0x73646976u);        /* fccType 'vids' */
        WR32(p + 4u, f->fcc_handler);
        WR32(p + 20u, f->scale);
        WR32(p + 24u, f->rate);
        WR32(p + 28u, 0);                 /* dwStart */
        WR32(p + 32u, f->index_count);    /* dwLength */
        WR32(p + 40u, 0x20000u);          /* dwSuggestedBufferSize */
        WR32(p + 48u, 0);
        WR32(p + 52u, 0);                 /* rcFrame */
        WR32(p + 56u, 0);
        WR32(p + 60u, f->width);
        WR32(p + 64u, f->height);
    }
    stdcall_ret(c, f ? AVIERR_OK : AVIERR_NODATA, 3);
}

SHIM(avifil32, AVIStreamReadFormat)
{
    AviFile *f = file_by_handle(ARG(0));
    uint32_t fmt = ARG(2), cb = ARG(3);
    if (!f) {
        stdcall_ret(c, AVIERR_NODATA, 4);
        return;
    }
    if (fmt && RD32(cb) >= f->strf_size)
        memcpy(gptr(fmt), f->strf, f->strf_size);
    WR32(cb, f->strf_size);
    stdcall_ret(c, AVIERR_OK, 4);
}

SHIM(avifil32, AVIStreamRead)
{
    AviFile *f = file_by_handle(ARG(0));
    uint32_t start = ARG(1), buf = ARG(3), cb = ARG(4), pbytes = ARG(5), psamples = ARG(6);
    if (!f || start >= f->index_count) {
        if (pbytes)
            WR32(pbytes, 0);
        if (psamples)
            WR32(psamples, 0);
        stdcall_ret(c, AVIERR_NODATA, 7);
        return;
    }
    AviIndex *e = &f->index[start];
    if (buf) {
        uint32_t n = e->size < cb ? e->size : cb;
        memcpy(gptr(buf), f->data + e->offset, n);
        last_read_buf = buf;
        last_read_size = n;
    }
    if (pbytes)
        WR32(pbytes, e->size);
    if (psamples)
        WR32(psamples, 1);
    stdcall_ret(c, AVIERR_OK, 7);
}

SHIM(avifil32, AVIStreamRelease) { stdcall_ret(c, 0, 1); }

SHIM(avifil32, AVIFileRelease)
{
    AviFile *f = file_by_handle(ARG(0));
    if (f) {
        for (int i = 0; i < 16; i++)
            if (files[i] == f)
                files[i] = NULL;
        heap_free(f->guest_handle);
        heap_free(f->stream_handle);
        free(f->data);
        free(f->index);
        free(f);
    }
    stdcall_ret(c, 0, 1);
}

/* ---- msvfw32: the Indeo decompressor ------------------------------------ */

typedef struct Codec {
    VideoFrameSource src;   /* first member: the renderer's view of this decoder */
    uint32_t converted_serial;
    VideoDecoder *dec;
    uint32_t fourcc;
    int width, height;
    uint32_t *rgba;
    uint32_t bad_frames;
} Codec;

#define HIC_BASE 0x00E1C000u
static Codec codecs[4];

static Codec *codec_by_handle(uint32_t h)
{
    uint32_t i = h - HIC_BASE;
    return i < 4u && codecs[i].dec ? &codecs[i] : NULL;
}

static const uint32_t *codec_frame_rgba(VideoFrameSource *vs)
{
    Codec *k = (Codec *)vs;
    if (k->converted_serial != vs->serial) {
        video_frame_rgba(k->dec, k->rgba);
        k->converted_serial = vs->serial;
    }
    return k->rgba;
}

VideoFrameSource *video_source_for(uint32_t mem)
{
    for (uint32_t i = 0; i < 4u; i++)
        if (codecs[i].dec && codecs[i].src.mem == mem && codecs[i].src.serial)
            return &codecs[i].src;
    return NULL;
}

SHIM(msvfw32, ICLocate)
{
    uint32_t handler = ARG(1), bi_in = ARG(2);
    uint32_t fourcc = bi_in ? RD32(bi_in + 16u) : handler;    /* biCompression */
    int w = bi_in ? (int)RD32(bi_in + 4u) : 320, h = bi_in ? (int)RD32(bi_in + 8u) : 240;
    if (h < 0)
        h = -h;
    for (uint32_t i = 0; i < 4u; i++) {
        if (codecs[i].dec)
            continue;
        VideoDecoder *d = video_decoder_open(fourcc, w, h);
        if (!d) {
            rt_log("ICLocate(%.4s %dx%d): no decoder available", (const char *)&fourcc, w, h);
            stdcall_ret(c, 0, 5);
            return;
        }
        codecs[i].dec = d;
        codecs[i].fourcc = fourcc;
        codecs[i].width = w;
        codecs[i].height = h;
        codecs[i].rgba = calloc((size_t)w * (size_t)h, 4u);
        codecs[i].src.width = w;
        codecs[i].src.height = h;
        codecs[i].src.rgba = codec_frame_rgba;
        stdcall_ret(c, HIC_BASE + i, 5);
        return;
    }
    stdcall_ret(c, 0, 5);
}

SHIM(msvfw32, ICSendMessage)
{
    uint32_t msg = ARG(1), dw1 = ARG(2), dw2 = ARG(3);
    uint32_t r = ICERR_OK;
    if (msg == 0x400Au && dw2 == 0u) {    /* ICM_DECOMPRESS_GET_FORMAT: size query */
        r = 40;
    } else if (msg == 0x400Au) {          /* fill a 32-bit RGB output format */
        memset(gptr(dw2), 0, 40);
        WR32(dw2 + 0u, 40);
        WR32(dw2 + 4u, RD32(dw1 + 4u));
        WR32(dw2 + 8u, RD32(dw1 + 8u));
        WR16(dw2 + 12u, 1);
        WR16(dw2 + 14u, 32);
    }
    stdcall_ret(c, r, 4);
}

SHIM(msvfw32, ICClose)
{
    Codec *k = codec_by_handle(ARG(0));
    if (k) {
        video_decoder_close(k->dec);
        free(k->rgba);
        memset(k, 0, sizeof(*k));
    }
    stdcall_ret(c, ICERR_OK, 1);
}

/* ICDecompress is VFWAPIV (cdecl): the caller pops the arguments. */
SHIM(msvfw32, ICDecompress)
{
    Codec *k = codec_by_handle(ARG(0));
    uint32_t data = ARG(3), bi_out = ARG(4), bits = ARG(5), bi_fmt = ARG(2);
    uint32_t r = ICERR_BADFORMAT;
    static int logged;
    if (logged < 1 && bi_out) {
        logged++;
        rt_log("ICDecompress(hic %08X flags %08X data %08X bits %08X): out %dx%d %ubpp comp %u size %u", ARG(0), ARG(1),
               data, bits, (int32_t)RD32(bi_out + 4u), (int32_t)RD32(bi_out + 8u), RD16(bi_out + 14u),
               RD32(bi_out + 16u), RD32(bi_out + 20u));
    }
    if (k && data && bits && bi_out) {
        uint32_t size = data == last_read_buf ? last_read_size : RD32(bi_fmt + 20u);
        /* Empty / undecodable packets (drop frames) repeat the previous picture. */
        if (size == 0u || video_decode(k->dec, gptr(data), size) != 0)
            k->bad_frames++;
        k->src.serial++;
        k->src.mem = bits;
        {
            int w = (int)RD32(bi_out + 4u);
            int32_t hh = (int32_t)RD32(bi_out + 8u);
            int top_down = hh < 0;
            int h = top_down ? -hh : hh;
            uint32_t bpp = RD16(bi_out + 14u), comp = RD32(bi_out + 16u);
            uint32_t rm = 0x7C00u, gm = 0x03E0u, bmask = 0x001Fu;   /* 16-bit BI_RGB is 555 */
            if (comp == 3u) {                 /* BI_BITFIELDS: masks follow the header */
                rm = RD32(bi_out + 40u);
                gm = RD32(bi_out + 44u);
                bmask = RD32(bi_out + 48u);
            }
            if (ddraw_is_client_mem(bits)) {
                /* The frame reaches the screen through a game-memory surface: the
                 * renderer converts it lazily, only when that surface is drawn. */
                c->r[0] = ICERR_OK;
                c->r[4] += 4u;
                return;
            }
            const uint32_t *rgba = codec_frame_rgba(&k->src);
            uint32_t pitch = (((uint32_t)w * bpp / 8u) + 3u) & ~3u;
            /* No divisions per pixel: the Vita's Cortex-A9 has no hardware divide. */
            static int32_t xmap[2048];
            if (w > 2048)
                w = 2048;
            for (int x = 0; x < w; x++)
                xmap[x] = (int32_t)((int64_t)x * k->width / w);
            int rs = __builtin_ctz(rm), gs = __builtin_ctz(gm), bs = __builtin_ctz(bmask);
            int rb = __builtin_popcount(rm), gb = __builtin_popcount(gm), bb = __builtin_popcount(bmask);
            for (int y = 0; y < h; y++) {
                int sy = (int)((int64_t)y * k->height / h);
                const uint32_t *src = rgba + (size_t)sy * (size_t)k->width;
                uint32_t row = top_down ? (uint32_t)y : (uint32_t)(h - 1 - y);
                uint8_t *dst = g_mem + bits + row * pitch;
                if (bpp == 16u) {
                    for (int x = 0; x < w; x++) {
                        uint32_t p = src[xmap[x]];
                        uint32_t v = (((p & 0xFFu) >> (8 - rb)) << rs) | ((((p >> 8) & 0xFFu) >> (8 - gb)) << gs) |
                                     ((((p >> 16) & 0xFFu) >> (8 - bb)) << bs);
                        dst[x * 2] = (uint8_t)v;
                        dst[x * 2 + 1] = (uint8_t)(v >> 8);
                    }
                } else if (bpp == 32u) {
                    for (int x = 0; x < w; x++) {
                        uint32_t p = src[xmap[x]];
                        uint32_t v = ((p >> 16) & 0xFFu) | (p & 0xFF00u) | ((p & 0xFFu) << 16);
                        memcpy(dst + x * 4, &v, 4);
                    }
                } else if (bpp == 24u) {
                    for (int x = 0; x < w; x++) {
                        uint32_t p = src[xmap[x]];
                        dst[x * 3] = (uint8_t)(p >> 16);
                        dst[x * 3 + 1] = (uint8_t)(p >> 8);
                        dst[x * 3 + 2] = (uint8_t)p;
                    }
                }
            }
            r = ICERR_OK;
        }
    }
    c->r[0] = r;
    c->r[4] += 4u;   /* return address only */
}
