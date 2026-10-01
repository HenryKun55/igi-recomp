/* Hand-decompiled IMA ADPCM decoder of IGI.exe.
 *
 *   0x00496280  adpcm_decode   decodes count frames of 4-bit IMA ADPCM into
 *                              16-bit samples
 *
 *   void adpcm_decode(const uint8_t *in, int16_t *out, int count, AdpcmState *st);   (cdecl)
 *
 * AdpcmState: +0 channels (s16), +2 predictor[2] (s16), +6 step index[2] (s16),
 * +0x0A nibble flag (u8: the low nibble of the last byte read is next). Frames
 * hold one sample per channel, interleaved; each sample takes the next nibble,
 * high nibble of a byte first. Tables: index adjustment (s8[16]) at 0x543920,
 * step sizes (s16[89]) at 0x543930.
 *
 * Integer code only. Each sample is one lookup in a table built from the
 * guest's tables (rebuilt whenever they differ from the copies kept): the
 * predictor change and the next step index for every step index and nibble,
 * computed exactly as the guest does, then a 16-bit saturation. The guest keeps
 * the per-channel predictor, index and step in a local array of two entries
 * and leaves its last values, with the output and input pointers, the last
 * byte read and the remaining count, in its frame and argument slots; this
 * does the same, so the dead stack matches as well. Left to the recompiled
 * code: more than two channels (the guest's arrays then run into each other),
 * none, a step index out of range, and output that overlaps the input, the
 * state, the frame or the tables (the guest would read back its own samples). */
#include "native.h"

#define FN_ADPCM_DECODE  0x00496280u

#define T_INDEX          0x00543920u   /* s8[16] */
#define T_STEP           0x00543930u   /* s16[89] */

#define ST_CHANNELS      0x00u
#define ST_PRED          0x02u
#define ST_INDEX         0x06u
#define ST_FLAG          0x0Au
#define ST_SIZE          0x0Bu

static inline int overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return a < b + bn && b < a + an;
}

/* Per channel: predictor, step index and step (the guest's three local arrays). */
typedef struct Chan {
    int32_t p, i, s;
} Chan;

/* The guest's tables and, for every step index i and nibble n (k = 16 * i + n),
 * the signed predictor change and 16 times the next index, exactly as the guest
 * computes them. Rebuilt when the guest's tables differ from the copies. */
static struct {
    int ready;
    uint8_t raw_index[16], raw_step[2 * 89];
    int32_t step[89];
    int32_t delta[89 * 16];
    uint16_t next[89 * 16];
} tab;

static void build_tables(const uint8_t *gm)
{
    memcpy(tab.raw_index, gm + T_INDEX, sizeof(tab.raw_index));
    memcpy(tab.raw_step, gm + T_STEP, sizeof(tab.raw_step));
    for (int i = 0; i < 89; i++)
        tab.step[i] = (int16_t)rt_ld16(tab.raw_step + 2 * i);
    for (int i = 0; i < 89; i++)
        for (uint32_t n = 0; n < 16; n++) {
            int32_t j = i + (int8_t)tab.raw_index[n];
            j = j < 0 ? 0 : j > 88 ? 88 : j;
            int32_t s = tab.step[i];
            int32_t d = s >> 3;
            if (n & 4u)
                d += s;
            if (n & 2u)
                d += s >> 1;
            if (n & 1u)
                d += s >> 2;
            tab.delta[16 * i + (int)n] = (n & 8u) ? -d : d;
            tab.next[16 * i + (int)n] = (uint16_t)(16 * j);
        }
    tab.ready = 1;
}

static inline int32_t sat16(int32_t p)
{
#if defined(__ARM_ARCH) && !defined(__aarch64__)
    __asm__("ssat %0, #16, %1" : "=r"(p) : "r"(p));
    return p;
#else
    return p < -32768 ? -32768 : p > 32767 ? 32767 : p;
#endif
}

/* One sample: p and i16 (16 * step index) of a channel, nibble n. */
#define SAMPLE(p, i16, n, dst)                     \
    do {                                           \
        uint32_t k_ = (i16) + (n);                 \
        p = sat16(p + tab.delta[k_]);              \
        i16 = tab.next[k_];                        \
        rt_st16(dst, (uint32_t)(p));               \
    } while (0)

int rt_native_adpcm_decode(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(FN_ADPCM_DECODE);
    (void)ecx;
    uint8_t *const gm = g_mem;
    /* Frame: S = esp - 0x24 after sub esp, 0x1c and two pushes; the locals are
     * S+8 out, S+0xc pred[2], S+0x14 index[2], S+0x1c step[2]; the arguments
     * at S+0x28 (in; then the last byte read), S+0x2c (out; then in), S+0x30
     * (count), S+0x34 (state). */
    const uint32_t S = esp - 0x24u;
    uint32_t in = rt_ld32(gm + esp + 4u), out = rt_ld32(gm + esp + 8u);
    int32_t count = (int32_t)rt_ld32(gm + esp + 12u);
    uint32_t st = rt_ld32(gm + esp + 16u);
    int32_t ch = (int16_t)rt_ld16(gm + st + ST_CHANNELS);
    if (ch < 1 || ch > 2 || count > 0x1000000)
        return 0;
    /* Step indices out of range (never in the game's data) take the guest code. */
    for (int e = 0; e < ch; e++)
        if (rt_ld16(gm + st + ST_INDEX + 2u * (uint32_t)e) > 88u)
            return 0;
    if (count > 0) {
        uint32_t nout = 2u * (uint32_t)count * (uint32_t)ch, nin = ((uint32_t)count * (uint32_t)ch + 3u) / 2u;
        if (overlaps(out, nout, in, nin) || overlaps(out, nout, st, ST_SIZE) ||
            overlaps(out, nout, esp - 0x2Cu, 0x40u) || overlaps(in, nin, st, ST_SIZE) ||
            overlaps(in, nin, esp - 0x2Cu, 0x40u) || overlaps(out, nout, T_INDEX, 0x10u + 2u * 89u) ||
            out + nout < out || in + nin < in)
            return 0;
    }

    Chan ch0, ch1;
    ch0.p = (int16_t)rt_ld16(gm + st + ST_PRED);
    ch1.p = (int16_t)rt_ld16(gm + st + ST_PRED + 2u);
    ch0.i = (int16_t)rt_ld16(gm + st + ST_INDEX);
    ch1.i = (int16_t)rt_ld16(gm + st + ST_INDEX + 2u);
    ch0.s = (int16_t)rt_ld16(gm + T_STEP + 2u * (uint32_t)ch0.i);
    ch1.s = (int16_t)rt_ld16(gm + T_STEP + 2u * (uint32_t)ch1.i);
    int flag = gm[st + ST_FLAG] != 0;
    uint32_t cur = in, edx = (uint32_t)ch1.s;
    if (flag) {
        cur = gm[in];
        in++;
        edx = cur;
        rt_st32(gm + S + 0x28u, cur);
    }
    rt_st32(gm + S + 0x2Cu, in);

    uint32_t eax_hi = (uint32_t)count;
    if (count > 0) {
        /* The output overlaps neither table. */
        if (!tab.ready || memcmp(tab.raw_index, gm + T_INDEX, sizeof(tab.raw_index)) != 0 ||
            memcmp(tab.raw_step, gm + T_STEP, sizeof(tab.raw_step)) != 0)
            build_tables(gm);
        uint8_t *o = gm + out;
        const uint8_t *src = gm + in;
        int32_t p0 = ch0.p, p1 = ch1.p;
        uint32_t i0 = 16u * (uint32_t)ch0.i, i1 = 16u * (uint32_t)ch1.i;
        /* Nibbles high first; with the flag set, the low one of cur is next. */
        if (ch == 2) {
            if (!flag) {
                for (int32_t n = count; n != 0; n--, o += 4) {
                    cur = *src++;
                    SAMPLE(p0, i0, cur >> 4, o);
                    SAMPLE(p1, i1, cur & 0xFu, o + 2);
                }
            } else {
                for (int32_t n = count; n != 0; n--, o += 4) {
                    SAMPLE(p0, i0, cur & 0xFu, o);
                    cur = *src++;
                    SAMPLE(p1, i1, cur >> 4, o + 2);
                }
            }
            ch1.p = p1;
            ch1.i = (int32_t)(i1 >> 4);
            ch1.s = tab.step[ch1.i];
        } else {
            int32_t n = count;
            if (flag) {
                SAMPLE(p0, i0, cur & 0xFu, o);
                o += 2;
                n--;
                flag = 0;
            }
            for (; n >= 2; n -= 2, o += 4) {
                cur = *src++;
                SAMPLE(p0, i0, cur >> 4, o);
                SAMPLE(p0, i0, cur & 0xFu, o + 2);
            }
            if (n) {
                cur = *src++;
                SAMPLE(p0, i0, cur >> 4, o);
                o += 2;
                flag = 1;
            }
        }
        ch0.p = p0;
        ch0.i = (int32_t)(i0 >> 4);
        ch0.s = tab.step[ch0.i];
        in += (uint32_t)(src - (gm + in));
        out += (uint32_t)(o - (gm + out));
        gm[st + ST_FLAG] = (uint8_t)flag;
        rt_st32(gm + S + 0x28u, cur);
        rt_st32(gm + S + 0x2Cu, in);
        rt_st32(gm + S + 0x30u, 0);
        /* push ebx, push edi */
        rt_st32(gm + S - 4u, c->r[3]);
        rt_st32(gm + S - 8u, c->r[7]);
        eax_hi = 0;
        edx = (uint32_t)ch;
    }

    rt_st32(gm + S, c->r[6]);
    rt_st32(gm + S + 4u, c->r[5]);
    rt_st32(gm + S + 8u, out);
    for (int e = 0; e < 2; e++) {
        const Chan *k = e ? &ch1 : &ch0;
        rt_st32(gm + S + 0x0Cu + 4u * (uint32_t)e, (uint32_t)k->p);
        rt_st32(gm + S + 0x14u + 4u * (uint32_t)e, (uint32_t)k->i);
        rt_st32(gm + S + 0x1Cu + 4u * (uint32_t)e, (uint32_t)k->s);
    }
    rt_st16(gm + st + ST_INDEX, (uint16_t)ch0.i);
    rt_st16(gm + st + ST_PRED, (uint16_t)ch0.p);
    rt_st16(gm + st + ST_PRED + 2u, (uint16_t)ch1.p);
    rt_st16(gm + st + ST_INDEX + 2u, (uint16_t)ch1.i);
    (void)eax;
    c->r[1] = (uint32_t)ch1.i;
    c->r[2] = (edx & 0xFFFF0000u) | ((uint32_t)ch1.p & 0xFFFFu);
    *ret = ((uint64_t)(esp + 4u) << 32) | (eax_hi & 0xFFFF0000u) | ((uint32_t)ch0.p & 0xFFFFu);
    return 1;
}
