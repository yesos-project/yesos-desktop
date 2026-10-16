// YesOS Desktop - `audio` console command.
#include "hda.h"
#include "shell.h"
#include "kprintf.h"
#include "memory.h"
#include "wav.h"
#include "redsea_eml.h"

// ---------------------------------------------------------------------------
// `audio play <file>`: a 16-bit PCM WAV file from RedSea, played at its own
// sample rate (no resampling), mono duplicated to both channels.
// ---------------------------------------------------------------------------
typedef struct {
    WavInfo w;
    U32 next;
} WavCtx;

static U32 wav_fill(I16* out, U32 max, U0* p) {
    WavCtx* c = (WavCtx*)p;
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
    if (why) {
        kprintf("audio: %s: %s\n", name, why);
    } else if (!hda_set_rate(c.w.rate)) {
        kprintf("audio: %s: %u Hz is not a rate this codec supports\n", name, c.w.rate);
        why = "a rate this codec does not support";
    } else {
        U32 ms = (U32)((U64)c.w.frames * 1000 / c.w.rate);
        kprintf("[AUDIO] play %s: %u Hz, 16-bit, %s, %u frames (%u.%03u s)\n", name, c.w.rate,
                c.w.channels == 2 ? "stereo" : "mono", c.w.frames, ms / 1000, ms % 1000);
        c.next = 0;
        U64 frames = hda_play(wav_fill, &c);
        if (frames_played) *frames_played = frames;
        kprintf("[AUDIO] play done: %u frames, %u underrun(s)\n", (U32)frames, (U32)hda_state()->underruns);
    }
    Free(data);
    return why;
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
    if (shell_is_prefix("audio play ", cmd)) {
        const char* p = cmd + 11;
        while (*p == ' ') p++;
        if (!*p) kprintf("usage: audio play <file.wav>\n");
        else audio_play_file(p, NULL);
        return TRUE;
    }
    if (shell_is_prefix("audio", cmd)) {
        kprintf("usage: audio [status] | audio tone <Hz> <ms> [level %%] | audio play <file.wav>\n");
        return TRUE;
    }
    return FALSE;
}

static U0 audio_help(U0) {
    kprintf("  Audio\n");
    kprintf("    audio [status]   audio tone <Hz> <ms> [level %%]   audio play <file.wav>\n");
}

static const ShellExtension s_audio_shell = { audio_run, audio_help, NULL, NULL };

U0 audio_shell_register(U0) {
    shell_register(&s_audio_shell);
}
