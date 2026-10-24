// YesOS Desktop - `audio` console command.
#include "hda.h"
#include "shell.h"
#include "kprintf.h"
#include "memory.h"
#include "wav.h"
#include "redsea_eml.h"
#include "yesmath.h"

// ---------------------------------------------------------------------------
// `audio play <file>`: a 16-bit PCM WAV file from RedSea, mono duplicated to
// both channels. At its own sample rate when the codec has it; otherwise
// resampled with yesmath's 8-tap windowed sinc (ym_resample_sinc8) to 48 kHz,
// or, above 48 kHz, to the next rate up the codec has. Only upwards: going
// down would need a lower cut-off to avoid aliasing, so such files are refused.
// ---------------------------------------------------------------------------
#define RS_PAD_BEFORE 3                 // the resampler reads x[i - 3 .. i + 4]
#define RS_PAD_AFTER  5
#define RS_CHUNK      256

typedef struct {
    WavInfo w;
    U32 next;                           // input frames done
    U32 out_rate;                       // the stream's rate
    I16* ch[2];                         // resampling: each channel, zero-padded
    U64 nx, pos, step;                  // their length, position and step (Q32 input samples)
} WavCtx;

static I16 s_rs_tmp[2][RS_CHUNK];

static U32 wav_fill(I16* out, U32 max, U0* p) {
    WavCtx* c = (WavCtx*)p;
    if (!c->ch[0]) {
        U32 n = c->w.frames - c->next < max ? c->w.frames - c->next : max;
        const I16* src = c->w.samples + (U64)c->next * c->w.channels;
        if (c->w.channels == 2) {
            MemCpy(out, src, (size_t)n * 4);
        } else {
            for (U32 i = 0; i < n; i++) out[2 * i] = out[2 * i + 1] = src[i];
        }
        c->next += n;
        return n;
    }
    U32 done = 0;
    while (done < max) {
        U32 k = max - done < RS_CHUNK ? max - done : RS_CHUNK;
        U64 p0 = c->pos, p1 = c->pos;
        U32 m = (U32)ym_resample_sinc8(c->ch[0], c->nx, s_rs_tmp[0], k, &p0, c->step);
        if (c->w.channels == 2) ym_resample_sinc8(c->ch[1], c->nx, s_rs_tmp[1], k, &p1, c->step);
        const I16* r = s_rs_tmp[c->w.channels == 2 ? 1 : 0];
        for (U32 i = 0; i < m; i++) {
            out[2 * (done + i)] = s_rs_tmp[0][i];
            out[2 * (done + i) + 1] = r[i];
        }
        c->pos = p0;
        done += m;
        if (m < k) break;               // the taps reached the end of the file
    }
    U64 at = (c->pos >> 32) - RS_PAD_BEFORE;
    c->next = at < c->w.frames ? (U32)at : c->w.frames;
    return done;
}

static U0 wav_release(WavCtx* c) {
    for (U32 i = 0; i < 2; i++) {
        if (c->ch[i]) Free(c->ch[i]);
        c->ch[i] = NULL;
    }
}

// Set the codec's rate for the file, and the resampler if it needs one.
static const char* wav_prepare(WavCtx* c) {
    c->next = 0;
    c->ch[0] = c->ch[1] = NULL;
    if (hda_set_rate(c->w.rate)) {
        c->out_rate = c->w.rate;
        return NULL;
    }
    static const U32 up[] = { 48000, 88200, 96000, 176400, 192000 };
    U32 o = 0;
    for (U32 i = 0; i < sizeof(up) / sizeof(up[0]) && !o; i++)
        if (up[i] >= c->w.rate && hda_rate_supported(up[i])) o = up[i];
    if (!o) return "a rate above every rate this codec has (no resampling downwards)";
    if (!hda_set_rate(o)) return "the stream rate could not be set";
    c->nx = (U64)c->w.frames + RS_PAD_BEFORE + RS_PAD_AFTER;
    for (U32 k = 0; k < c->w.channels; k++) {
        I16* x = (I16*)Malloc((size_t)c->nx * 2);
        if (!x) { wav_release(c); return "out of memory"; }
        c->ch[k] = x;
        MemSet(x, 0, (size_t)c->nx * 2);
        for (U32 i = 0; i < c->w.frames; i++) x[RS_PAD_BEFORE + i] = c->w.samples[(U64)i * c->w.channels + k];
    }
    c->pos = (U64)RS_PAD_BEFORE << 32;  // output 0 is input frame 0
    c->step = ((U64)c->w.rate << 32) / o;
    c->out_rate = o;
    return NULL;
}

static U0 wav_announce(const char* verb, const char* name, const WavCtx* c, const char* tail) {
    U32 ms = (U32)((U64)c->w.frames * 1000 / c->w.rate);
    kprintf("[AUDIO] %s %s: %u Hz, %s%s, %u frames (%u.%03u s)", verb, name, c->w.rate,
            StrCmp(verb, "play") == 0 ? "16-bit, " : "",
            c->w.channels == 2 ? "stereo" : "mono", c->w.frames, ms / 1000, ms % 1000);
    if (c->ch[0]) kprintf(", resampled to %u Hz", c->out_rate);
    kprintf("%s\n", tail);
}

const char* audio_play_file(const char* name, U64* frames_played) {
    if (frames_played) *frames_played = 0;
    if (!hda_state()->ready) {
        kprintf("audio: no HD Audio output\n");
        return "no HD Audio output";
    }
    if (!redsea_eml_is_mounted()) {
        kprintf("audio: no RedSea volume mounted\n");
        return "no RedSea volume mounted";
    }
    size_t size = 0;
    U8* data = redsea_eml_read_file_path(name, &size);
    if (!data) {
        kprintf("audio: %s: no such file\n", name);
        return "no such file";
    }
    WavCtx c;
    const char* why = wav_parse(data, size, &c.w);
    if (!why) why = wav_prepare(&c);
    if (why) {
        if (c.w.rate && StrNCmp(why, "a rate", 6) == 0) kprintf("audio: %s: %u Hz is %s\n", name, c.w.rate, why);
        else kprintf("audio: %s: %s\n", name, why);
    } else {
        wav_announce("play", name, &c, "");
        U64 frames = hda_play(wav_fill, &c);
        if (frames_played) *frames_played = frames;
        kprintf("[AUDIO] play done: %u frames, %u underrun(s)\n", (U32)frames, (U32)hda_state()->underruns);
        wav_release(&c);
    }
    Free(data);
    return why;
}

// ---- background: `audio start <file>`, the file browser ---------------------------------

typedef struct {
    WavCtx      c;
    U8*         data;                   // the whole file (c.w.samples points into it)
    char        path[128];
    AudioDoneFn done;
    U0*         ctx;
} BgPlay;

static BgPlay* s_bg;                    // the sound playing in the background, if any

static U0 bg_done(U64 frames, Bool cancelled, U0* p) {
    BgPlay* b = (BgPlay*)p;
    if (cancelled) kprintf("[AUDIO] play stopped: %s after %u frames\n", b->path, (U32)frames);
    else kprintf("[AUDIO] play done: %s, %u frames, %u underrun(s)\n", b->path, (U32)frames,
                 (U32)hda_state()->underruns);
    if (s_bg == b) s_bg = NULL;
    AudioDoneFn done = b->done;
    U0* ctx = b->ctx;
    char path[128];
    StrNCpy(path, b->path, sizeof(path));
    wav_release(&b->c);
    Free(b->data);
    Free(b);
    if (done) done(path, frames, cancelled, ctx);
}

const char* audio_start_file(const char* name, AudioDoneFn done, U0* ctx) {
    if (!hda_state()->ready) return "no HD Audio output";
    if (!redsea_eml_is_mounted()) return "no RedSea volume mounted";
    hda_cancel();                       // one sound at a time; the rate can only change when stopped
    size_t size = 0;
    U8* data = redsea_eml_read_file_path(name, &size);
    if (!data) return "no such file";
    BgPlay* b = (BgPlay*)Malloc(sizeof(BgPlay));
    if (!b) { Free(data); return "out of memory"; }
    MemSet(b, 0, sizeof(*b));
    const char* why = wav_parse(data, size, &b->c.w);
    if (!why) why = wav_prepare(&b->c);
    if (why) { Free(data); Free(b); return why; }
    b->data = data;
    StrNCpy(b->path, name, sizeof(b->path));
    b->path[sizeof(b->path) - 1] = '\0';
    b->done = done;
    b->ctx = ctx;
    s_bg = b;
    wav_announce("start", name, &b->c, ", in the background");
    if (!hda_start(wav_fill, &b->c, bg_done)) {
        s_bg = NULL;
        wav_release(&b->c);
        Free(data);
        Free(b);
        return "the stream could not be started";
    }
    return NULL;
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
    kprintf("  Stream : %u Hz, 16-bit stereo, ring %u frames (%u ms), %s\n",
            s->rate, HDA_RING_FRAMES, HDA_RING_FRAMES * 1000 / s->rate, s->playing ? "playing" : "stopped");
    kprintf("  Rates  :");
    static const U32 rates[] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000 };
    for (U32 i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
        if (hda_rate_supported(rates[i])) kprintf(" %u", rates[i]);
    kprintf(" Hz\n");
    if (s_bg) kprintf("  Background: playing %s (frame %u of %u)\n", s_bg->path, s_bg->c.next, s_bg->c.w.frames);
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
    if (shell_is_prefix("audio start ", cmd)) {
        const char* p = cmd + 12;
        while (*p == ' ') p++;
        const char* why = *p ? audio_start_file(p, NULL, NULL) : "usage: audio start <file.wav>";
        if (why) kprintf("audio: %s: %s\n", p, why);
        return TRUE;
    }
    if (StrCmp(cmd, "audio stop") == 0) {
        if (hda_busy()) hda_cancel();
        else kprintf("audio: nothing is playing in the background\n");
        return TRUE;
    }
    if (shell_is_prefix("audio play ", cmd)) {
        const char* p = cmd + 11;
        while (*p == ' ') p++;
        if (!*p) kprintf("usage: audio play <file.wav>\n");
        else audio_play_file(p, NULL);
        return TRUE;
    }
    if (shell_is_prefix("audio", cmd)) {
        kprintf("usage: audio [status] | audio tone <Hz> <ms> [level %%] | audio play <file.wav>\n"
                "       audio start <file.wav> (in the background) | audio stop\n");
        return TRUE;
    }
    return FALSE;
}

static U0 audio_help(U0) {
    kprintf("  Audio\n");
    kprintf("    audio [status]   audio tone <Hz> <ms> [level %%]   audio play <file.wav>\n");
    kprintf("    audio start <file.wav>   audio stop   (in the background: the desktop keeps working)\n");
}

static const ShellExtension s_audio_shell = { audio_run, audio_help, NULL, NULL };

U0 audio_shell_register(U0) {
    shell_register(&s_audio_shell);
}
