// YesOS Desktop - `audio` console command.
#include "hda.h"
#include "shell.h"
#include "kprintf.h"
#include "memory.h"

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
    if (shell_is_prefix("audio", cmd)) {
        kprintf("usage: audio [status] | audio tone <Hz> <ms> [level %%]\n");
        return TRUE;
    }
    return FALSE;
}

static U0 audio_help(U0) {
    kprintf("  Audio\n");
    kprintf("    audio [status]   audio tone <Hz> <ms> [level %%]\n");
}

static const ShellExtension s_audio_shell = { audio_run, audio_help, NULL, NULL };

U0 audio_shell_register(U0) {
    shell_register(&s_audio_shell);
}
