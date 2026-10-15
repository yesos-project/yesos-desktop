// ============================================================================
// YesOS Desktop - EML audio processing (resampler, dither, limiter).
// Brought over from yesos-lab (eml/eml_audio_physics.c). The filter bank is
// the lab's, regenerated bit for bit by tools/gen_audio_tables.py.
// ============================================================================

#include "eml_audio.h"
#include "memory.h"

extern const I32 g_eml_sinc_q15[EML_SINC_PHASES + 1][EML_SINC_TAPS];

static inline I32 clamp_s32(I64 v) {
    if (v > 2147483647LL) return 2147483647;
    if (v < -2147483648LL) return (I32)-2147483648LL;
    return (I32)v;
}

U0 eml_resampler_init(EmlResampler* r, U32 in_rate, U32 out_rate) {
    MemSet(r, 0, sizeof(*r));
    r->step_q32 = ((U64)in_rate << 32) / out_rate;
}

static U0 prime(EmlResampler* r, const I16* in) {
    if (r->primed) return;
    for (U32 m = 0; m < EML_RS_HISTORY; m++) {
        r->hist[0][m] = in[0];
        r->hist[1][m] = in[1];
    }
    r->pos_q32 = 0;
    r->primed = TRUE;
}

// Input frame `i` of the current block; negative indices reach back into
// the previous block.
static inline I32 frame_at(const EmlResampler* r, const I16* in, I64 i, U32 ch) {
    if (i >= 0) return in[2 * i + ch];
    I64 h = EML_RS_HISTORY + i;
    if (h < 0) h = 0;
    return r->hist[ch][h];
}

static U0 end_block(EmlResampler* r, const I16* in, U32 in_frames) {
    for (U32 m = 0; m < EML_RS_HISTORY; m++) {
        I64 src = (I64)in_frames - EML_RS_HISTORY + m;
        if (src >= 0) {
            r->hist[0][m] = in[2 * src];
            r->hist[1][m] = in[2 * src + 1];
        } else {
            // a block shorter than the history: shift the old frames down
            r->hist[0][m] = r->hist[0][m + in_frames];
            r->hist[1][m] = r->hist[1][m + in_frames];
        }
    }
    r->pos_q32 -= (I64)in_frames << 32;
}

U32 eml_resample(EmlResampler* r, const I16* in, U32 in_frames, I32* out) {
    if (!in_frames) return 0;
    prime(r, in);
    U32 n = 0;
    for (;;) {
        I64 idx = r->pos_q32 >> 32;
        if (idx + 4 >= (I64)in_frames) break;           // taps reach idx + 4
        U32 frac = (U32)r->pos_q32;
        U32 ph = frac >> 26;                            // 64 phases
        I64 sub = (frac >> 10) & 0xFFFF;                // Q16 between phases
        I64 acc_l = 0, acc_r = 0;
        for (U32 k = 0; k < EML_SINC_TAPS; k++) {
            I64 h = ((I64)g_eml_sinc_q15[ph][k] * (65536 - sub) + (I64)g_eml_sinc_q15[ph + 1][k] * sub) >> 16;
            I64 i = idx - 3 + (I64)k;
            acc_l += h * frame_at(r, in, i, 0);
            acc_r += h * frame_at(r, in, i, 1);
        }
        out[2 * n] = clamp_s32(acc_l << 1);             // Q15 taps: << 1 gives S32
        out[2 * n + 1] = clamp_s32(acc_r << 1);
        n++;
        r->pos_q32 += (I64)r->step_q32;
    }
    end_block(r, in, in_frames);
    return n;
}

U32 linear_resample(EmlResampler* r, const I16* in, U32 in_frames, I32* out) {
    if (!in_frames) return 0;
    prime(r, in);
    U32 n = 0;
    for (;;) {
        I64 idx = r->pos_q32 >> 32;
        if (idx + 4 >= (I64)in_frames) break;           // same delay as eml_resample
        I64 f = ((U32)r->pos_q32) >> 16;                // Q16
        for (U32 ch = 0; ch < 2; ch++) {
            I64 a = frame_at(r, in, idx, ch), b = frame_at(r, in, idx + 1, ch);
            out[2 * n + ch] = clamp_s32((a << 16) + (b - a) * f);
        }
        n++;
        r->pos_q32 += (I64)r->step_q32;
    }
    end_block(r, in, in_frames);
    return n;
}

// xorshift64*: 32 good bits per call.
static inline U32 xorshift64(U64* s) {
    U64 x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return (U32)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

U0 eml_dither_init(EmlDither* d, U64 seed, Bool shaping) {
    if (!seed) seed = 0x5A5A5A5A12345678ULL;
    d->rng[0] = seed;
    d->rng[1] = seed ^ 0xF00DCAFEBABE4321ULL;
    d->err[0] = d->err[1] = 0;
    d->shaping = shaping;
}

I16 eml_dither16(EmlDither* d, I32 s32, U32 ch) {
    I64 tpdf = (I64)(xorshift64(&d->rng[ch]) & 0xFFFF) - (I64)(xorshift64(&d->rng[ch]) & 0xFFFF);
    I64 x = s32;
    if (d->shaping) x -= ((I64)d->err[ch] * 3) / 4;
    I64 q = (x + tpdf + 32768) >> 16;
    if (q > 32767) q = 32767;
    else if (q < -32768) q = -32768;
    d->err[ch] = (I32)((q << 16) - x);
    return (I16)q;
}

I16 round16(I32 s32) {
    I64 q = ((I64)s32 + 32768) >> 16;
    return (I16)(q > 32767 ? 32767 : q);
}

U0 eml_limit(I32* s, U32 frames) {
    const I64 thresh = (I64)EML_LIMIT_THRESH << 16;
    const I64 head = ((I64)32767 << 16) - thresh;
    for (U32 i = 0; i < frames; i++) {
        I64 l = s[2 * i], r = s[2 * i + 1];
        I64 al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
        I64 peak = al > ar ? al : ar;
        if (peak <= thresh) continue;
        I64 d = peak - thresh;
        I64 sat = thresh + head * d / (head + d);       // Pade [1/1] knee
        I64 gain = (sat << 16) / peak;                  // Q16, the same for both channels
        s[2 * i] = (I32)((l * gain) >> 16);
        s[2 * i + 1] = (I32)((r * gain) >> 16);
    }
}
