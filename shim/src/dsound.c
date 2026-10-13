/* dsound.dll: DirectSound 7 with a software mixer on the SDL audio device.
 *
 * Buffer memory lives in guest space so the game can Lock/write it directly;
 * the audio callback reads it concurrently, like a sound card would.
 */
#include "bench.h"
#include "com.h"

#include <SDL.h>
#include <math.h>
#include <stdlib.h>

#define DS_OK 0u
#define DSERR_INVALIDPARAM E_INVALIDARG
#define DSERR_NODRIVER 0x88780078u

#define DSBCAPS_PRIMARYBUFFER 0x1u
#define DSBCAPS_CTRL3D 0x10u
#define DSBSTATUS_PLAYING 0x1u
#define DSBSTATUS_LOOPING 0x4u

#define OUT_RATE 44100
#define MAX_BUFFERS 4096

typedef struct Vec3 {
    float x, y, z;
} Vec3;

typedef struct SoundBuffer {
    int refs;
    uint32_t obj, obj3d;
    int primary;
    uint32_t flags;
    uint32_t mem, size;
    int *mem_refs;               /* shared by DuplicateSoundBuffer copies */
    int channels, bits, rate, block;
    uint32_t freq;               /* current playback rate */
    int32_t volume, pan;         /* hundredths of dB / pan units */
    int playing, looping;
    double pos;                  /* play position in frames */
    /* 3D */
    int is3d;
    uint32_t mode3d;
    Vec3 p3d;
    float min_dist, max_dist;
    float gain_l, gain_r;        /* combined gain computed on the game thread */
} SoundBuffer;

static SDL_AudioDeviceID dev;
static SoundBuffer *buffers[MAX_BUFFERS];
static uint32_t ds_vtbl, buf_vtbl, buf3d_vtbl, listener_vtbl;
static uint32_t ds_obj, listener_obj;

static struct {
    Vec3 pos, front, top;
    float rolloff, distance_factor;
} listener = { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, 1.0f, 1.0f };

/* ---- gain --------------------------------------------------------------- */

static int trace_sound(void)
{
    static int t = -1;
    if (t < 0)
        t = getenv("IGI_TRACE_SOUND") != NULL;
    return t;
}

static float db_to_gain(int32_t v)
{
    if (v <= -10000)
        return 0.0f;
    return powf(10.0f, (float)v / 2000.0f);
}

static void update_gain(SoundBuffer *b)
{
    float g = db_to_gain(b->volume);
    float l = 1.0f, r = 1.0f;
    if (b->pan < 0)
        r = db_to_gain(b->pan);
    else if (b->pan > 0)
        l = db_to_gain(-b->pan);
    if (b->is3d && b->mode3d != 2u) {
        Vec3 d = b->p3d;
        if (b->mode3d == 0u) {
            d.x -= listener.pos.x;
            d.y -= listener.pos.y;
            d.z -= listener.pos.z;
        }
        /* Min/max distances are in the same units as positions (DistanceFactor only affects doppler). */
        float dist = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
        float att = 1.0f;
        if (dist > b->min_dist) {
            float dd = dist < b->max_dist ? dist : b->max_dist;
            att = b->min_dist / (b->min_dist + listener.rolloff * (dd - b->min_dist));
        }
        g *= att;
        if (dist > 0.001f && b->mode3d == 0u) {
            /* Right vector of a left-handed listener frame: top x front. */
            Vec3 f = listener.front, t = listener.top;
            Vec3 right = { t.y * f.z - t.z * f.y, t.z * f.x - t.x * f.z, t.x * f.y - t.y * f.x };
            float rl = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
            if (rl > 0.0f) {
                float s = (d.x * right.x + d.y * right.y + d.z * right.z) / (rl * sqrtf(d.x * d.x + d.y * d.y + d.z * d.z));
                l *= sqrtf(0.5f * (1.0f - s));
                r *= sqrtf(0.5f * (1.0f + s));
                l *= 1.41421356f;
                r *= 1.41421356f;
            }
        }
    }
    b->gain_l = g * (l > 1.0f ? 1.0f : l);
    b->gain_r = g * (r > 1.0f ? 1.0f : r);
}

static void update_all_3d(void)
{
    for (int i = 0; i < MAX_BUFFERS; i++)
        if (buffers[i] && buffers[i]->is3d)
            update_gain(buffers[i]);
}

/* ---- mixer -------------------------------------------------------------- */

static float sample_at(const SoundBuffer *b, uint32_t frame, int ch)
{
    uint32_t off = frame * (uint32_t)b->block + (uint32_t)(ch < b->channels ? ch : 0) * (uint32_t)(b->bits / 8);
    if (off + 2u > b->size)
        return 0.0f;
    const uint8_t *p = g_mem + b->mem + off;
    if (b->bits == 8)
        return ((float)p[0] - 128.0f) / 128.0f;
    int16_t s;
    memcpy(&s, p, 2);
    return (float)s / 32768.0f;
}

static void mix(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    float *out = (float *)stream;
    int frames = len / (int)(2 * sizeof(float));
    memset(stream, 0, (size_t)len);
    for (int i = 0; i < MAX_BUFFERS; i++) {
        SoundBuffer *b = buffers[i];
        if (!b || b->primary || !b->playing || !b->mem || !b->block)
            continue;
        uint32_t total = b->size / (uint32_t)b->block;
        if (!total)
            continue;
        /* 32.32 fixed-point position: no double math or float<->int
         * conversions per sample (slow on the Vita's VFP). */
        const uint64_t step = ((uint64_t)b->freq << 32) / OUT_RATE;
        uint64_t pos = (uint64_t)(b->pos * 4294967296.0);
        const uint8_t *data = g_mem + b->mem;
        const uint32_t block = (uint32_t)b->block;
        const int stereo = b->channels > 1, wide = b->bits != 8;
        const uint32_t rch = stereo ? (uint32_t)(b->bits / 8) : 0u;
        const float gl = b->gain_l, gr = b->gain_r;
        /* Frames below `total` lie fully inside the buffer (block = channels * bytes). */
        const uint32_t limit = total;
        for (int f = 0; f < frames; f++) {
            uint32_t i0 = (uint32_t)(pos >> 32);
            if (i0 >= total) {
                if (!b->looping) {
                    b->playing = 0;
                    pos = 0;
                    break;
                }
                pos -= (uint64_t)total << 32;
                i0 = (uint32_t)(pos >> 32);
            }
            float frac = (float)(uint32_t)(pos >> 8 & 0xFFFFFFu) * (1.0f / 16777216.0f);
            uint32_t i1 = i0 + 1u < total ? i0 + 1u : (b->looping ? 0u : i0);
            float l0, l1, r0, r1;
            if (i0 < limit && i1 < limit) {
                const uint8_t *p0 = data + i0 * block, *p1 = data + i1 * block;
                if (wide) {
                    int16_t a, c, d, e;
                    memcpy(&a, p0, 2);
                    memcpy(&c, p1, 2);
                    memcpy(&d, p0 + rch, 2);
                    memcpy(&e, p1 + rch, 2);
                    l0 = (float)a * (1.0f / 32768.0f);
                    l1 = (float)c * (1.0f / 32768.0f);
                    r0 = (float)d * (1.0f / 32768.0f);
                    r1 = (float)e * (1.0f / 32768.0f);
                } else {
                    l0 = ((float)p0[0] - 128.0f) * (1.0f / 128.0f);
                    l1 = ((float)p1[0] - 128.0f) * (1.0f / 128.0f);
                    r0 = ((float)p0[rch] - 128.0f) * (1.0f / 128.0f);
                    r1 = ((float)p1[rch] - 128.0f) * (1.0f / 128.0f);
                }
            } else {
                l0 = sample_at(b, i0, 0);
                l1 = sample_at(b, i1, 0);
                r0 = sample_at(b, i0, 1);
                r1 = sample_at(b, i1, 1);
            }
            float l = l0 + (l1 - l0) * frac;
            float r = stereo ? r0 + (r1 - r0) * frac : l;
            out[f * 2] += l * gl;
            out[f * 2 + 1] += r * gr;
            pos += step;
        }
        b->pos = (double)pos * (1.0 / 4294967296.0);
    }
    for (int f = 0; f < frames * 2; f++) {
        if (out[f] > 1.0f)
            out[f] = 1.0f;
        else if (out[f] < -1.0f)
            out[f] = -1.0f;
    }
    /* IGI_BENCH_MUTE=1 silences benchmark runs (the mixing work above still happens). */
    static int muted = -1;
    if (muted < 0) {
        const char *e = getenv("IGI_BENCH_MUTE");
        muted = bench_enabled() && e && e[0] == '1';
    }
    if (muted)
        memset(stream, 0, (size_t)len);
}

static int audio_open(void)
{
    if (dev)
        return 1;
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = OUT_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = mix;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) {
        rt_log("SDL_OpenAudioDevice: %s", SDL_GetError());
        return 0;
    }
    rt_log("audio device opened: %d Hz, %d channels", have.freq, have.channels);
    SDL_PauseAudioDevice(dev, 0);
    return 1;
}

/* Stop (pause == 1) or resume (0) the SDL audio callback. Used by the Vita
 * suspend handler so the mixer stops feeding sceAudioOut while the system is
 * asleep or the app is in the background. No-op before the device is open. */
void dsound_audio_pause(int pause)
{
    if (dev)
        SDL_PauseAudioDevice(dev, pause ? 1 : 0);
}

/* ---- IDirectSoundBuffer ------------------------------------------------- */

static SoundBuffer *buf(uint32_t obj) { return (SoundBuffer *)com_host(obj); }

static void buffer_free(SoundBuffer *b)
{
    SDL_LockAudioDevice(dev);
    for (int i = 0; i < MAX_BUFFERS; i++)
        if (buffers[i] == b)
            buffers[i] = NULL;
    SDL_UnlockAudioDevice(dev);
    if (b->mem && b->mem_refs && --*b->mem_refs == 0) {
        heap_free(b->mem);
        free(b->mem_refs);
    }
    if (b->obj3d) {
        WR32(b->obj3d + 8u, 1);
        com_release(b->obj3d);
    }
    WR32(b->obj + 8u, 1);
    com_release(b->obj);
    free(b);
}

static void b_QueryInterface(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    Guid iid;
    guid_read(ARG(1), &iid);
    uint32_t o = 0;
    if (guid_eq(&iid, "279AFA86-4981-11CE-A521-0020AF0BE560") ||      /* IDirectSound3DBuffer */
        guid_eq(&iid, "279AFA85-4981-11CE-A521-0020AF0BE560")) {      /* IDirectSoundBuffer */
        if (guid_eq(&iid, "279AFA85-4981-11CE-A521-0020AF0BE560")) {
            o = b->obj;
        } else if (b->primary) {
            o = listener_obj;
        } else {
            if (!b->obj3d)
                b->obj3d = com_new(buf3d_vtbl, b);
            o = b->obj3d;
        }
    } else if (guid_eq(&iid, "279AFA84-4981-11CE-A521-0020AF0BE560")) {  /* IDirectSound3DListener */
        o = listener_obj;
    }
    if (!o) {
        rt_log("IDirectSoundBuffer::QueryInterface(%s): no interface", guid_str(&iid));
        WR32(ARG(2), 0);
        stdcall_ret(c, E_NOINTERFACE, 3);
        return;
    }
    if (o != listener_obj)
        b->refs++;
    WR32(ARG(2), o);
    stdcall_ret(c, DS_OK, 3);
}

static void b_AddRef(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    stdcall_ret(c, (uint32_t)++b->refs, 1);
}

static void b_Release(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    int n = --b->refs;
    if (n <= 0) {
        buffer_free(b);
        n = 0;
    }
    stdcall_ret(c, (uint32_t)n, 1);
}

static void b_GetCaps(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    uint32_t p = ARG(1);
    WR32(p + 4u, b->flags);
    WR32(p + 8u, b->size);
    WR32(p + 12u, 0);
    WR32(p + 16u, 0);
    stdcall_ret(c, DS_OK, 2);
}

static void b_GetCurrentPosition(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    uint32_t play = b->block ? ((uint32_t)b->pos * (uint32_t)b->block) % (b->size ? b->size : 1u) : 0u;
    uint32_t write = b->size ? (play + (uint32_t)b->block * (b->freq / 100u + 1u)) % b->size : 0u;
    if (ARG(1))
        WR32(ARG(1), play);
    if (ARG(2))
        WR32(ARG(2), write);
    stdcall_ret(c, DS_OK, 3);
}

static void write_format(uint32_t p, const SoundBuffer *b)
{
    WR16(p + 0u, 1);
    WR16(p + 2u, (uint32_t)b->channels);
    WR32(p + 4u, (uint32_t)b->rate);
    WR32(p + 8u, (uint32_t)(b->rate * b->block));
    WR16(p + 12u, (uint32_t)b->block);
    WR16(p + 14u, (uint32_t)b->bits);
    WR16(p + 16u, 0);
}

static void b_GetFormat(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    if (ARG(1))
        write_format(ARG(1), b);
    if (ARG(3))
        WR32(ARG(3), 18);
    stdcall_ret(c, DS_OK, 4);
}

static void b_GetVolume(Ctx *c)
{
    WR32(ARG(1), (uint32_t)buf(THIS)->volume);
    stdcall_ret(c, DS_OK, 2);
}

static void b_GetPan(Ctx *c)
{
    WR32(ARG(1), (uint32_t)buf(THIS)->pan);
    stdcall_ret(c, DS_OK, 2);
}

static void b_GetFrequency(Ctx *c)
{
    WR32(ARG(1), buf(THIS)->freq);
    stdcall_ret(c, DS_OK, 2);
}

static void b_GetStatus(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    uint32_t s = 0;
    if (b->playing)
        s |= DSBSTATUS_PLAYING | (b->looping ? DSBSTATUS_LOOPING : 0u);
    WR32(ARG(1), s);
    stdcall_ret(c, DS_OK, 2);
}

static void b_Lock(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    uint32_t off = ARG(1), bytes = ARG(2), flags = ARG(7);
    if (flags & 2u) {        /* DSBLOCK_ENTIREBUFFER */
        off = 0;
        bytes = b->size;
    } else if (flags & 1u) { /* DSBLOCK_FROMWRITECURSOR */
        off = b->block ? ((uint32_t)b->pos * (uint32_t)b->block) % b->size : 0u;
    }
    if (bytes > b->size)
        bytes = b->size;
    off %= b->size ? b->size : 1u;
    uint32_t n1 = bytes, n2 = 0;
    if (off + bytes > b->size) {
        n1 = b->size - off;
        n2 = bytes - n1;
    }
    WR32(ARG(3), b->mem + off);
    WR32(ARG(4), n1);
    if (ARG(5))
        WR32(ARG(5), n2 ? b->mem : 0u);
    if (ARG(6))
        WR32(ARG(6), n2);
    stdcall_ret(c, DS_OK, 8);
}

static void b_Unlock(Ctx *c) { stdcall_ret(c, DS_OK, 5); }

static void b_Play(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    SDL_LockAudioDevice(dev);
    b->looping = (ARG(3) & 1u) != 0;
    b->playing = 1;
    SDL_UnlockAudioDevice(dev);
    if (trace_sound())
        rt_log("DS Play %08X loop %d vol %d pan %d freq %u 3d %d gain %.3f/%.3f", b->obj, b->looping, b->volume,
               b->pan, b->freq, b->is3d, (double)b->gain_l, (double)b->gain_r);
    stdcall_ret(c, DS_OK, 4);
}

static void b_Stop(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    SDL_LockAudioDevice(dev);
    b->playing = 0;
    SDL_UnlockAudioDevice(dev);
    stdcall_ret(c, DS_OK, 1);
}

static void b_SetCurrentPosition(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    SDL_LockAudioDevice(dev);
    b->pos = b->block ? (double)(ARG(1) / (uint32_t)b->block) : 0.0;
    SDL_UnlockAudioDevice(dev);
    stdcall_ret(c, DS_OK, 2);
}

static void read_format(uint32_t wfx, SoundBuffer *b)
{
    b->channels = (int)RD16(wfx + 2u);
    b->rate = (int)RD32(wfx + 4u);
    b->bits = (int)RD16(wfx + 14u);
    b->block = (int)RD16(wfx + 12u);
    if (!b->block)
        b->block = b->channels * b->bits / 8;
    b->freq = (uint32_t)b->rate;
}

static void b_SetFormat(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    if (ARG(1))
        read_format(ARG(1), b);
    stdcall_ret(c, DS_OK, 2);
}

static void b_SetVolume(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    b->volume = (int32_t)ARG(1);
    update_gain(b);
    stdcall_ret(c, DS_OK, 2);
}

static void b_SetPan(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    b->pan = (int32_t)ARG(1);
    update_gain(b);
    stdcall_ret(c, DS_OK, 2);
}

static void b_SetFrequency(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    uint32_t f = ARG(1);
    b->freq = f ? f : (uint32_t)b->rate;
    stdcall_ret(c, DS_OK, 2);
}

static void b_Restore(Ctx *c) { stdcall_ret(c, DS_OK, 1); }

static const ComMethod buffer_methods[] = {
    { "QueryInterface", b_QueryInterface },
    { "AddRef", b_AddRef },
    { "Release", b_Release },
    { "GetCaps", b_GetCaps },
    { "GetCurrentPosition", b_GetCurrentPosition },
    { "GetFormat", b_GetFormat },
    { "GetVolume", b_GetVolume },
    { "GetPan", b_GetPan },
    { "GetFrequency", b_GetFrequency },
    { "GetStatus", b_GetStatus },
    { "Initialize", NULL },
    { "Lock", b_Lock },
    { "Play", b_Play },
    { "SetCurrentPosition", b_SetCurrentPosition },
    { "SetFormat", b_SetFormat },
    { "SetVolume", b_SetVolume },
    { "SetPan", b_SetPan },
    { "SetFrequency", b_SetFrequency },
    { "Stop", b_Stop },
    { "Unlock", b_Unlock },
    { "Restore", b_Restore },
};

/* ---- IDirectSound3DBuffer ----------------------------------------------- */

static void b3_QueryInterface(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    b->refs++;
    WR32(ARG(2), b->obj);
    stdcall_ret(c, DS_OK, 3);
}

static void b3_AddRef(Ctx *c) { stdcall_ret(c, (uint32_t)++buf(THIS)->refs, 1); }

static void b3_Release(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    int n = --b->refs;
    if (n <= 0) {
        buffer_free(b);
        n = 0;
    }
    stdcall_ret(c, (uint32_t)n, 1);
}

static float argf(Ctx *c, int n)
{
    uint32_t v = ARG(n);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static void b3_SetPosition(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    b->p3d.x = argf(c, 1);
    b->p3d.y = argf(c, 2);
    b->p3d.z = argf(c, 3);
    update_gain(b);
    stdcall_ret(c, DS_OK, 5);
}

static void b3_GetPosition(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    memcpy(gptr(ARG(1)), &b->p3d, 12);
    stdcall_ret(c, DS_OK, 2);
}

static void b3_SetMinDistance(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    b->min_dist = argf(c, 1);
    update_gain(b);
    stdcall_ret(c, DS_OK, 3);
}

static void b3_SetMaxDistance(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    b->max_dist = argf(c, 1);
    update_gain(b);
    stdcall_ret(c, DS_OK, 3);
}

static void b3_GetMinDistance(Ctx *c)
{
    memcpy(gptr(ARG(1)), &buf(THIS)->min_dist, 4);
    stdcall_ret(c, DS_OK, 2);
}

static void b3_GetMaxDistance(Ctx *c)
{
    memcpy(gptr(ARG(1)), &buf(THIS)->max_dist, 4);
    stdcall_ret(c, DS_OK, 2);
}

static void b3_SetMode(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    b->mode3d = ARG(1);
    update_gain(b);
    stdcall_ret(c, DS_OK, 3);
}

static void b3_GetMode(Ctx *c)
{
    WR32(ARG(1), buf(THIS)->mode3d);
    stdcall_ret(c, DS_OK, 2);
}

static void b3_ok3(Ctx *c) { stdcall_ret(c, DS_OK, 3); }
static void b3_ok4(Ctx *c) { stdcall_ret(c, DS_OK, 4); }
static void b3_ok5(Ctx *c) { stdcall_ret(c, DS_OK, 5); }

static void b3_SetAllParameters(Ctx *c)
{
    SoundBuffer *b = buf(THIS);
    uint32_t p = ARG(1);
    memcpy(&b->p3d, gptr(p + 4u), 12);
    memcpy(&b->min_dist, gptr(p + 52u), 4);
    memcpy(&b->max_dist, gptr(p + 56u), 4);
    b->mode3d = RD32(p + 60u);
    update_gain(b);
    stdcall_ret(c, DS_OK, 3);
}

static const ComMethod buffer3d_methods[] = {
    { "QueryInterface", b3_QueryInterface },
    { "AddRef", b3_AddRef },
    { "Release", b3_Release },
    { "GetAllParameters", NULL },
    { "GetConeAngles", NULL },
    { "GetConeOrientation", NULL },
    { "GetConeOutsideVolume", NULL },
    { "GetMaxDistance", b3_GetMaxDistance },
    { "GetMinDistance", b3_GetMinDistance },
    { "GetMode", b3_GetMode },
    { "GetPosition", b3_GetPosition },
    { "GetVelocity", NULL },
    { "SetAllParameters", b3_SetAllParameters },
    { "SetConeAngles", b3_ok4 },
    { "SetConeOrientation", b3_ok5 },
    { "SetConeOutsideVolume", b3_ok3 },
    { "SetMaxDistance", b3_SetMaxDistance },
    { "SetMinDistance", b3_SetMinDistance },
    { "SetMode", b3_SetMode },
    { "SetPosition", b3_SetPosition },
    { "SetVelocity", b3_ok5 },
};

/* ---- IDirectSound3DListener --------------------------------------------- */

static void l_QueryInterface(Ctx *c)
{
    WR32(ARG(2), THIS);
    stdcall_ret(c, DS_OK, 3);
}

static void l_AddRef(Ctx *c) { stdcall_ret(c, 2, 1); }
static void l_Release(Ctx *c) { stdcall_ret(c, 1, 1); }

static void l_SetPosition(Ctx *c)
{
    listener.pos.x = argf(c, 1);
    listener.pos.y = argf(c, 2);
    listener.pos.z = argf(c, 3);
    update_all_3d();
    stdcall_ret(c, DS_OK, 5);
}

static void l_SetOrientation(Ctx *c)
{
    listener.front.x = argf(c, 1);
    listener.front.y = argf(c, 2);
    listener.front.z = argf(c, 3);
    listener.top.x = argf(c, 4);
    listener.top.y = argf(c, 5);
    listener.top.z = argf(c, 6);
    update_all_3d();
    stdcall_ret(c, DS_OK, 8);
}

static void l_SetRolloffFactor(Ctx *c)
{
    listener.rolloff = argf(c, 1);
    update_all_3d();
    stdcall_ret(c, DS_OK, 3);
}

static void l_SetDistanceFactor(Ctx *c)
{
    listener.distance_factor = argf(c, 1);
    update_all_3d();
    stdcall_ret(c, DS_OK, 3);
}

static void l_ok1(Ctx *c) { stdcall_ret(c, DS_OK, 1); }
static void l_ok3(Ctx *c) { stdcall_ret(c, DS_OK, 3); }
static void l_ok5(Ctx *c) { stdcall_ret(c, DS_OK, 5); }

static void l_GetPosition(Ctx *c)
{
    memcpy(gptr(ARG(1)), &listener.pos, 12);
    stdcall_ret(c, DS_OK, 2);
}

static void l_SetAllParameters(Ctx *c)
{
    uint32_t p = ARG(1);
    memcpy(&listener.pos, gptr(p + 4u), 12);
    memcpy(&listener.front, gptr(p + 28u), 12);
    memcpy(&listener.top, gptr(p + 40u), 12);
    memcpy(&listener.distance_factor, gptr(p + 52u), 4);
    memcpy(&listener.rolloff, gptr(p + 56u), 4);
    update_all_3d();
    stdcall_ret(c, DS_OK, 3);
}

static const ComMethod listener_methods[] = {
    { "QueryInterface", l_QueryInterface },
    { "AddRef", l_AddRef },
    { "Release", l_Release },
    { "GetAllParameters", NULL },
    { "GetDistanceFactor", NULL },
    { "GetDopplerFactor", NULL },
    { "GetOrientation", NULL },
    { "GetPosition", l_GetPosition },
    { "GetRolloffFactor", NULL },
    { "GetVelocity", NULL },
    { "SetAllParameters", l_SetAllParameters },
    { "SetDistanceFactor", l_SetDistanceFactor },
    { "SetDopplerFactor", l_ok3 },
    { "SetOrientation", l_SetOrientation },
    { "SetPosition", l_SetPosition },
    { "SetRolloffFactor", l_SetRolloffFactor },
    { "SetVelocity", l_ok5 },
    { "CommitDeferredSettings", l_ok1 },
};

/* ---- IDirectSound ------------------------------------------------------- */

static SoundBuffer *buffer_new(void)
{
    SoundBuffer *b = calloc(1, sizeof(*b));
    b->refs = 1;
    b->min_dist = 1.0f;
    b->max_dist = 1e9f;
    b->gain_l = b->gain_r = 1.0f;
    b->obj = com_new(buf_vtbl, b);
    int slot = -1;
    SDL_LockAudioDevice(dev);
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (!buffers[i]) {
            buffers[i] = b;
            slot = i;
            break;
        }
    }
    SDL_UnlockAudioDevice(dev);
    if (slot < 0)
        rt_log("DirectSound: more than %d buffers, this one will be silent", MAX_BUFFERS);
    return b;
}

static void ds_QueryInterface(Ctx *c)
{
    WR32(ARG(2), THIS);
    stdcall_ret(c, DS_OK, 3);
}

static void ds_AddRef(Ctx *c) { stdcall_ret(c, 2, 1); }
static void ds_Release(Ctx *c) { stdcall_ret(c, 1, 1); }

static void ds_CreateSoundBuffer(Ctx *c)
{
    uint32_t d = ARG(1);
    uint32_t flags = RD32(d + 4u), bytes = RD32(d + 8u), wfx = RD32(d + 16u);
    SoundBuffer *b = buffer_new();
    b->flags = flags;
    b->primary = (flags & DSBCAPS_PRIMARYBUFFER) != 0;
    b->is3d = (flags & DSBCAPS_CTRL3D) != 0;
    if (b->primary) {
        b->channels = 2;
        b->bits = 16;
        b->rate = 22050;
        b->block = 4;
        b->size = 65536;
    } else {
        if (wfx)
            read_format(wfx, b);
        b->size = bytes;
        b->mem = heap_alloc(bytes ? bytes : 4u);
        b->mem_refs = malloc(sizeof(int));
        *b->mem_refs = 1;
        memset(gptr(b->mem), b->bits == 8 ? 0x80 : 0, bytes);
    }
    b->freq = (uint32_t)b->rate;
    update_gain(b);
    if (trace_sound())
        rt_log("DS CreateSoundBuffer flags %08X bytes %u fmt %dch %dbit %dHz -> %08X", flags, bytes, b->channels,
               b->bits, b->rate, b->obj);
    WR32(ARG(2), b->obj);
    stdcall_ret(c, DS_OK, 4);
}

static void ds_DuplicateSoundBuffer(Ctx *c)
{
    SoundBuffer *src = buf(ARG(1));
    SoundBuffer *b = buffer_new();
    b->flags = src->flags;
    b->is3d = src->is3d;
    b->channels = src->channels;
    b->bits = src->bits;
    b->rate = src->rate;
    b->block = src->block;
    b->freq = src->freq;
    b->size = src->size;
    /* Duplicates share sample memory with the original, as in DirectSound. */
    b->mem = src->mem;
    b->mem_refs = src->mem_refs;
    if (b->mem_refs)
        ++*b->mem_refs;
    b->volume = src->volume;
    b->pan = src->pan;
    b->min_dist = src->min_dist;
    b->max_dist = src->max_dist;
    update_gain(b);
    if (trace_sound())
        rt_log("DS DuplicateSoundBuffer %08X -> %08X", src->obj, b->obj);
    WR32(ARG(2), b->obj);
    stdcall_ret(c, DS_OK, 3);
}

static void ds_GetCaps(Ctx *c)
{
    uint32_t p = ARG(1);
    uint32_t size = RD32(p);
    memset(gptr(p), 0, size);
    WR32(p, size);
    WR32(p + 4u, 0x00000F5Fu);   /* DSCAPS_*: primary/secondary mono/stereo 8/16, continuous rate */
    WR32(p + 8u, 100);
    WR32(p + 12u, 100000);
    for (uint32_t k = 20u; k <= 40u; k += 4u)
        WR32(p + k, 256);
    stdcall_ret(c, DS_OK, 2);
}

static void ds_SetCooperativeLevel(Ctx *c) { stdcall_ret(c, DS_OK, 3); }

static void ds_GetSpeakerConfig(Ctx *c)
{
    WR32(ARG(1), 4);   /* DSSPEAKER_STEREO */
    stdcall_ret(c, DS_OK, 2);
}

static void ds_ok2(Ctx *c) { stdcall_ret(c, DS_OK, 2); }
static void ds_ok1(Ctx *c) { stdcall_ret(c, DS_OK, 1); }

static const ComMethod ds_methods[] = {
    { "QueryInterface", ds_QueryInterface },
    { "AddRef", ds_AddRef },
    { "Release", ds_Release },
    { "CreateSoundBuffer", ds_CreateSoundBuffer },
    { "GetCaps", ds_GetCaps },
    { "DuplicateSoundBuffer", ds_DuplicateSoundBuffer },
    { "SetCooperativeLevel", ds_SetCooperativeLevel },
    { "Compact", ds_ok1 },
    { "GetSpeakerConfig", ds_GetSpeakerConfig },
    { "SetSpeakerConfig", ds_ok2 },
    { "Initialize", ds_ok2 },
};

/* DirectSoundCreate is imported by ordinal 1. */
SHIM(dsound, _1)
{
    if (!audio_open()) {
        WR32(ARG(1), 0);
        stdcall_ret(c, DSERR_NODRIVER, 3);
        return;
    }
    if (!ds_vtbl) {
        ds_vtbl = com_vtable("IDirectSound", ds_methods, (int)(sizeof(ds_methods) / sizeof(ds_methods[0])));
        buf_vtbl = com_vtable("IDirectSoundBuffer", buffer_methods,
                              (int)(sizeof(buffer_methods) / sizeof(buffer_methods[0])));
        buf3d_vtbl = com_vtable("IDirectSound3DBuffer", buffer3d_methods,
                                (int)(sizeof(buffer3d_methods) / sizeof(buffer3d_methods[0])));
        listener_vtbl = com_vtable("IDirectSound3DListener", listener_methods,
                                   (int)(sizeof(listener_methods) / sizeof(listener_methods[0])));
        ds_obj = com_new(ds_vtbl, NULL);
        listener_obj = com_new(listener_vtbl, NULL);
    }
    WR32(ARG(1), ds_obj);
    stdcall_ret(c, DS_OK, 3);
}
