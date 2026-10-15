// YesOS Desktop - `audio` console command.
#include "hda.h"
#include "shell.h"
#include "kprintf.h"
#include "memory.h"
#include "eml_audio.h"

extern const I32 g_sine_q31[1025];

// ---------------------------------------------------------------------------
// `audio play`: a test signal through the EML chain
//   sine at the source rate (Q31 table, interpolated) -> 16 bits (a source
//   file) -> resampler to 48 kHz -> [limiter] -> 16 bits -> HD Audio
// With a 48 kHz source the resampler is skipped.
// ---------------------------------------------------------------------------
typedef enum { RS_EML, RS_LINEAR } RsKind;
typedef enum { Q_TPDF, Q_SHAPED, Q_ROUND } QKind;

typedef struct {
    U32 hz, ms, level, rate;
    RsKind rs;
    QKind q;
    Bool limit;
} PlayCfg;

static struct {
    PlayCfg c;
    U64 left;                             // output frames still to produce
    U32 phase, step;
    I64 gain_q16;
    EmlResampler rs;
    EmlDither src_dither, out_dither;
    I16 src[2 * HDA_FILL_FRAMES];
    I32 s32[2 * HDA_FILL_FRAMES];
} s_play;

static const char* const s_q_name[] = { "TPDF dither", "TPDF dither + noise shaping", "rounding, no dither" };

static I32 sine_s32(U0) {
    U32 i = s_play.phase >> 22;
    I64 f = s_play.phase & 0x3FFFFF;
    I64 v = g_sine_q31[i] + ((((I64)g_sine_q31[i + 1] - g_sine_q31[i]) * f) >> 22);
    s_play.phase += s_play.step;
    return (I32)((v * s_play.gain_q16) >> 16);
}

static I16 quantise(EmlDither* d, I32 s32, U32 ch) {
    return s_play.c.q == Q_ROUND ? round16(s32) : eml_dither16(d, s32, ch);
}

static U32 play_fill(I16* out, U32 max, U0* unused) {
    (U0)unused;
    if (!s_play.left) return 0;
    U32 n;
    if (s_play.c.rate == HDA_RATE) {
        n = s_play.left < max ? (U32)s_play.left : max;
        for (U32 i = 0; i < n; i++) s_play.s32[2 * i] = s_play.s32[2 * i + 1] = sine_s32();
    } else {
        U32 in_n = (U32)((U64)(max - 2) * s_play.c.rate / HDA_RATE);
        for (U32 i = 0; i < in_n; i++) {
            I32 v = sine_s32();
            s_play.src[2 * i] = quantise(&s_play.src_dither, v, 0);
            s_play.src[2 * i + 1] = quantise(&s_play.src_dither, v, 1);
        }
        n = s_play.c.rs == RS_EML ? eml_resample(&s_play.rs, s_play.src, in_n, s_play.s32)
                                  : linear_resample(&s_play.rs, s_play.src, in_n, s_play.s32);
        if (n > s_play.left) n = (U32)s_play.left;
    }
    if (s_play.c.limit) eml_limit(s_play.s32, n);
    for (U32 i = 0; i < 2 * n; i++) out[i] = quantise(&s_play.out_dither, s_play.s32[i], i & 1);
    s_play.left -= n;
    return n ? n : 1;                     // never 0 before the end: 0 means finished
}

static U64 audio_play(const PlayCfg* c) {
    MemSet(&s_play, 0, sizeof(s_play));
    s_play.c = *c;
    s_play.left = (U64)c->ms * HDA_RATE / 1000;
    s_play.step = (U32)(((U64)c->hz << 32) / c->rate);
    s_play.gain_q16 = (I64)c->level * 65536 / 100;
    eml_resampler_init(&s_play.rs, c->rate, HDA_RATE);
    eml_dither_init(&s_play.src_dither, 0x9E3779B97F4A7C15ULL, c->q == Q_SHAPED);
    eml_dither_init(&s_play.out_dither, 0, c->q == Q_SHAPED);
    return hda_play(play_fill, 0);
}

// ---------------------------------------------------------------------------
// `audio bench`: retired instructions (minstret) per stereo frame of each
// stage, on 10 ms blocks (441 frames at 44.1 kHz in, 480 at 48 kHz out).
// Meaningful only under QEMU -icount, where minstret counts instructions;
// other harts can add to that count, so each figure is the minimum of
// several runs with interrupts off. It is an instruction count, not cycles.
// ---------------------------------------------------------------------------
#define BENCH_IN    441
#define BENCH_OUT   482
#define BENCH_RUNS  9

static I16 s_bsrc[2 * BENCH_IN];
static I32 s_bs32[2 * BENCH_OUT];
static I32 s_blim[2 * BENCH_OUT];
static I16 s_bout[2 * BENCH_OUT];

static inline U64 instret(U0) {
    U64 v;
    __asm__ __volatile__("csrr %0, minstret" : "=r"(v));
    return v;
}

typedef enum { B_CAL, B_SINE, B_EML, B_LINEAR, B_TPDF, B_ROUND, B_LIMIT } BenchOp;
static const char* const s_bench_name[] = {
    "calibration loop of 2 instructions",
    "sine source (Q31 table, interpolated)",
    "EML resampler 44.1 -> 48 kHz",
    "linear resampler 44.1 -> 48 kHz",
    "TPDF dither to 16 bits",
    "rounding to 16 bits",
    "soft limiter (full-scale sine)",
};

// Runs one block of `op`; returns the frames it produced.
static U32 bench_once(BenchOp op, EmlResampler* rs, EmlDither* d) {
    switch (op) {
    case B_CAL: {
        U64 k = 480;
        __asm__ __volatile__("1: addi %0, %0, -1\n bnez %0, 1b" : "+r"(k));
        return 480;
    }
    case B_SINE:
        for (U32 i = 0; i < BENCH_IN; i++) s_bs32[2 * i] = s_bs32[2 * i + 1] = sine_s32();
        return BENCH_IN;
    case B_EML:    return eml_resample(rs, s_bsrc, BENCH_IN, s_bs32);
    case B_LINEAR: return linear_resample(rs, s_bsrc, BENCH_IN, s_bs32);
    case B_TPDF:
        for (U32 i = 0; i < 2 * 480; i++) s_bout[i] = eml_dither16(d, s_bs32[i], i & 1);
        return 480;
    case B_ROUND:
        for (U32 i = 0; i < 2 * 480; i++) s_bout[i] = round16(s_bs32[i]);
        return 480;
    case B_LIMIT:
        MemCpy(s_bs32, s_blim, sizeof(s_bs32));
        eml_limit(s_bs32, 480);
        return 480;
    }
    return 0;
}

static U0 audio_bench(U0) {
    MemSet(&s_play, 0, sizeof(s_play));
    s_play.step = (U32)(((U64)1000 << 32) / 44100);
    s_play.gain_q16 = 65536;                            // full scale, for the limiter
    for (U32 i = 0; i < 480; i++) s_blim[2 * i] = s_blim[2 * i + 1] = sine_s32();
    s_play.gain_q16 = 32768;
    EmlDither d;
    eml_dither_init(&d, 0, FALSE);
    for (U32 i = 0; i < BENCH_IN; i++) s_bsrc[2 * i] = s_bsrc[2 * i + 1] = eml_dither16(&d, sine_s32(), 0);
    kprintf("[AUDIO] bench: retired instructions per stereo frame (min of %u runs of 10 ms)\n", BENCH_RUNS);
    for (U32 op = B_CAL; op <= B_LIMIT; op++) {
        EmlResampler rs;
        eml_resampler_init(&rs, 44100, HDA_RATE);
        bench_once((BenchOp)op, &rs, &d);              // warm up: caches, resampler history
        U64 best = ~0ULL, worst = 0;
        U32 frames = 1;
        for (U32 r = 0; r < BENCH_RUNS; r++) {
            __asm__ __volatile__("csrc mstatus, %0" : : "r"(1ULL << 3));
            U64 t0 = instret();
            U32 n = bench_once((BenchOp)op, &rs, &d);
            U64 t1 = instret();
            __asm__ __volatile__("csrs mstatus, %0" : : "r"(1ULL << 3));
            U64 per = (t1 - t0) * 100 / (n ? n : 1);
            if (per < best) { best = per; frames = n; }
            if (per > worst) worst = per;
        }
        kprintf("[AUDIO] bench %s: %u.%02u instr/frame (max %u.%02u, %u frames)\n", s_bench_name[op],
                (U32)(best / 100), (U32)(best % 100), (U32)(worst / 100), (U32)(worst % 100), frames);
    }
    kprintf("[AUDIO] bench done\n");
}

static U0 audio_status(U0) {
    HdaState* s = hda_state();
    if (!s->present) {
        kprintf("Audio: no HD Audio controller on the PCI bus\n");
        return;
    }
    kprintf("Audio: HD Audio at PCI %02x.0, %s\n", s->slot, s->ready ? "ready" : "NOT ready");
    if (!s->ready) return;
    kprintf("  Codec %u: DAC node %u, pin node %u, 0 dB at amp step %u\n", s->cad, s->dac, s->pin, s->amp_0db);
    kprintf("  Stream : 48000 Hz, 16-bit stereo, ring %u frames (%u ms), %s\n",
            HDA_RING_FRAMES, HDA_RING_FRAMES * 1000 / HDA_RATE, s->playing ? "playing" : "stopped");
    kprintf("  Frames : %u written, %u played, %u underrun(s)\n",
            (U32)s->frames_written, (U32)s->frames_played, (U32)s->underruns);
    kprintf("  Pisano : period %u steps (checked at init: %s), step %u, %u full period(s), F=(%u, %u)\n",
            HDA_PISANO_PERIOD, s->pisano_verified ? "OK" : "FAILED", s->fib_step, (U32)s->fib_periods,
            s->fib_prev, s->fib_curr);
}

static Bool audio_run(const char* cmd) {
    if (StrCmp(cmd, "audio") == 0 || StrCmp(cmd, "audio status") == 0) {
        audio_status();
        return TRUE;
    }
    if (shell_is_prefix("audio tone ", cmd)) {
        const char* p = cmd + 11;
        U32 hz = (U32)shell_parse_uint(p);
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        U32 ms = (U32)shell_parse_uint(p);
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        U32 level = *p ? (U32)shell_parse_uint(p) : 50;
        if (!hda_state()->ready) {
            kprintf("audio: no HD Audio output\n");
        } else if (hz < 20 || hz > 20000 || ms == 0 || ms > 60000 || level > 100) {
            kprintf("usage: audio tone <20-20000 Hz> <1-60000 ms> [level 0-100 %%]\n");
        } else {
            kprintf("[AUDIO] tone %u Hz, %u ms, level %u%%\n", hz, ms, level);
            U32 frames = hda_tone(hz, ms, level);
            kprintf("[AUDIO] tone done: %u frames, %u underrun(s)\n", frames, (U32)hda_state()->underruns);
        }
        return TRUE;
    }
    if (StrCmp(cmd, "audio bench") == 0) {
        audio_bench();
        return TRUE;
    }
    if (shell_is_prefix("audio play ", cmd)) {
        PlayCfg c = { 0, 0, 50, HDA_RATE, RS_EML, Q_TPDF, FALSE };
        U32 num[3], nn = 0;
        Bool bad = FALSE;
        for (const char* p = cmd + 11; *p;) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char* w = p;
            while (*p && *p != ' ') p++;
            char tok[16];
            U32 len = (U32)(p - w);
            if (len >= sizeof(tok)) { bad = TRUE; break; }
            MemCpy(tok, w, len);
            tok[len] = 0;
            if (tok[0] >= '0' && tok[0] <= '9') {
                U32 v = (U32)shell_parse_uint(tok);
                if (nn < 3) num[nn++] = v;
                else c.rate = v;                        // a fourth number: the source rate
            }
            else if (StrCmp(tok, "eml") == 0) c.rs = RS_EML;
            else if (StrCmp(tok, "linear") == 0) c.rs = RS_LINEAR;
            else if (StrCmp(tok, "tpdf") == 0) c.q = Q_TPDF;
            else if (StrCmp(tok, "shaped") == 0) c.q = Q_SHAPED;
            else if (StrCmp(tok, "round") == 0) c.q = Q_ROUND;
            else if (StrCmp(tok, "limit") == 0) c.limit = TRUE;
            else bad = TRUE;
        }
        if (nn >= 2) { c.hz = num[0]; c.ms = num[1]; }
        if (nn >= 3) c.level = num[2];
        if (!hda_state()->ready) {
            kprintf("audio: no HD Audio output\n");
        } else if (bad || nn < 2 || c.hz < 20 || c.hz >= c.rate / 2 || c.ms == 0 || c.ms > 60000 ||
                   c.level > 100 || c.rate < 8000 || c.rate > HDA_RATE) {
            kprintf("usage: audio play <Hz> <ms> [level %%] [source rate 8000-48000] [eml|linear] [tpdf|shaped|round] [limit]\n");
        } else {
            kprintf("[AUDIO] play %u Hz, %u ms, level %u%%: source %u Hz", c.hz, c.ms, c.level, c.rate);
            if (c.rate != HDA_RATE)
                kprintf(", %s to 48000 Hz", c.rs == RS_EML ? "EML 8-tap sinc resampler" : "linear interpolation");
            kprintf(", %s%s\n", c.limit ? "soft limiter, " : "", s_q_name[c.q]);
            U64 frames = audio_play(&c);
            kprintf("[AUDIO] play done: %u frames, %u underrun(s)\n", (U32)frames, (U32)hda_state()->underruns);
        }
        return TRUE;
    }
    if (shell_is_prefix("audio", cmd)) {
        kprintf("usage: audio [status] | audio tone <Hz> <ms> [level %%] | audio play ... | audio bench\n");
        return TRUE;
    }
    return FALSE;
}

static U0 audio_help(U0) {
    kprintf("  Audio\n");
    kprintf("    audio [status]   audio tone <Hz> <ms> [level %%]\n");
    kprintf("    audio play <Hz> <ms> [level %%] [source rate] [eml|linear] [tpdf|shaped|round] [limit]\n");
    kprintf("    audio bench      instructions per frame of each stage (run QEMU with -icount)\n");
}

static const ShellExtension s_audio_shell = { audio_run, audio_help, NULL, NULL };

U0 audio_shell_register(U0) {
    shell_register(&s_audio_shell);
}
