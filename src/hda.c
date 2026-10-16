// ============================================================================
// YesOS Desktop - Intel High Definition Audio output.
// PCI discovery and BARs come from the core (drivers/pci.c). The controller
// is driven by polling: no interrupts. One output stream (48 kHz, 16-bit
// stereo) plays from a power-of-two DMA ring; the write position follows the
// DMA position (LPIB). Integer arithmetic only.
// Brought over from yesos-lab (drivers/hda.c, drivers/hda_pisano.c) and
// rewritten smaller: codec nodes are discovered instead of assumed.
// ============================================================================

#include "hda.h"
#include "pci.h"
#include "ports.h"
#include "kprintf.h"
#include "memory.h"
#include "timer.h"

// Controller registers (HDA spec 3.3)
#define REG_GCAP      0x00
#define REG_VMIN      0x02
#define REG_VMAJ      0x03
#define REG_GCTL      0x08
#define REG_STATESTS  0x0E
#define REG_IC        0x60
#define REG_IR        0x64
#define REG_ICS       0x68
#define GCTL_CRST     0x1
#define ICS_ICB       0x1
#define ICS_IRV       0x2

// Stream descriptor registers
#define SD_CTL        0x00
#define SD_STS        0x03
#define SD_LPIB       0x04
#define SD_CBL        0x08
#define SD_LVI        0x0C
#define SD_FMT        0x12
#define SD_BDPL       0x18
#define SD_BDPU       0x1C
#define SD_CTL_SRST   0x1
#define SD_CTL_RUN    0x2
#define SD_STS_CLEAR  0x1C          // BCIS | FIFOE | DESE (write 1 to clear)
#define STREAM_TAG    1

// Codec verbs and parameters
#define VERB_GET_PARAM      0xF00
#define VERB_SET_STREAM_CH  0x706
#define VERB_SET_PIN_CTL    0x707
#define VERB_SET_EAPD       0x70C
#define VERB_SET_POWER      0x705
#define VERB4_SET_FORMAT    0x2
#define VERB4_SET_AMP       0x3
#define PAR_NODE_COUNT      0x04
#define PAR_FG_TYPE         0x05
#define PAR_WIDGET_CAPS     0x09
#define PAR_PIN_CAPS        0x0C
#define PAR_OUT_AMP_CAPS    0x12
#define PAR_PCM             0x0A    // supported PCM sizes (bits 20:16) and rates (bits 11:0)
#define WIDGET_OUTPUT       0x0
#define WCAP_OUT_AMP        (1U << 2)   // widget has an output amplifier
#define WIDGET_PIN          0x4
#define FMT_16BIT_STEREO    0x0011  // bits 6:4 = 16-bit, bits 3:0 = channels - 1
#define AMP_OUT_LR          0xB000  // output amp, left and right, unmuted

#define RING_MASK     (HDA_RING_FRAMES - 1)
#define RING_BYTES    (HDA_RING_FRAMES * HDA_FRAME_BYTES)
#define BDL_ENTRIES   4
#define WRITE_GUARD   512           // frames kept away from the DMA read position

typedef struct {
    U32 addr_lo, addr_hi, length, flags;
} __attribute__((packed)) BdlEntry;

static U8       s_ring[RING_BYTES] __attribute__((aligned(4096)));
static BdlEntry s_bdl[BDL_ENTRIES] __attribute__((aligned(128)));
static HdaState g;
static uintptr_t s_sd;              // first output stream descriptor

extern const I16 g_sine_q15[1024];

static inline U0 io_fence(U0) {
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
}

HdaState* hda_state(U0) {
    return &g;
}

// ---- codec commands (immediate command interface) ---------------------------

static Bool wait_ics(U16 mask, U16 want, U32 ms) {
    U64 end = timer_get_uptime_ms() + ms;
    while ((mmio_read16(g.mmio + REG_ICS) & mask) != want) {
        if (timer_get_uptime_ms() > end) return FALSE;
    }
    return TRUE;
}

static U32 codec_cmd(U32 word) {
    if (!wait_ics(ICS_ICB, 0, 10)) return 0;
    mmio_write32(g.mmio + REG_IC, word);
    mmio_write16(g.mmio + REG_ICS, ICS_IRV);           // clear a stale result
    mmio_write16(g.mmio + REG_ICS, ICS_ICB);           // send
    if (!wait_ics(ICS_ICB | ICS_IRV, ICS_IRV, 10)) return 0;
    return mmio_read32(g.mmio + REG_IR);
}

static U32 verb12(U8 nid, U16 verb, U8 payload) {
    return codec_cmd(((U32)g.cad << 28) | ((U32)nid << 20) | ((U32)(verb & 0xFFF) << 8) | payload);
}

static U32 verb4(U8 nid, U8 verb, U16 payload) {
    return codec_cmd(((U32)g.cad << 28) | ((U32)nid << 20) | ((U32)(verb & 0xF) << 16) | payload);
}

static U32 param(U8 nid, U8 id) {
    return verb12(nid, VERB_GET_PARAM, id);
}

// Sample rates an HDA stream format can express (spec 3.7.1): base 48 or
// 44.1 kHz (bit 14), multiplier (bits 13:11) and divisor (bits 10:8). `cap`
// is the matching bit of the codec's supported-rates parameter.
static const struct { U32 hz; U16 fmt; U8 cap; } s_rates[] = {
    {   8000, (0 << 14) | (0 << 11) | (5 << 8),  0 },   // 48 / 6
    {  11025, (1 << 14) | (0 << 11) | (3 << 8),  1 },   // 44.1 / 4
    {  16000, (0 << 14) | (0 << 11) | (2 << 8),  2 },   // 48 / 3
    {  22050, (1 << 14) | (0 << 11) | (1 << 8),  3 },   // 44.1 / 2
    {  32000, (0 << 14) | (1 << 11) | (2 << 8),  4 },   // 48 * 2 / 3
    {  44100, (1 << 14),                         5 },
    {  48000, 0,                                 6 },
    {  88200, (1 << 14) | (1 << 11),             7 },   // 44.1 * 2
    {  96000, (0 << 14) | (1 << 11),             8 },   // 48 * 2
    { 176400, (1 << 14) | (3 << 11),             9 },   // 44.1 * 4
    { 192000, (0 << 14) | (3 << 11),            10 },   // 48 * 4
};

static I32 rate_index(U32 hz) {
    for (U32 i = 0; i < sizeof(s_rates) / sizeof(s_rates[0]); i++)
        if (s_rates[i].hz == hz) return (I32)i;
    return -1;
}

Bool hda_rate_supported(U32 hz) {
    I32 i = rate_index(hz);
    return i >= 0 && (g.pcm_rates & (1U << s_rates[i].cap));
}

Bool hda_set_rate(U32 hz) {
    if (g.playing || !hda_rate_supported(hz)) return FALSE;
    g.rate = hz;
    return TRUE;
}

static U16 stream_format(U0) {
    return (U16)(s_rates[rate_index(g.rate)].fmt | FMT_16BIT_STEREO);
}

// Find the first audio function group, its first output converter (DAC) and
// its first output-capable pin.
static Bool discover_codec(U0) {
    U32 root = param(0, PAR_NODE_COUNT);
    U8 fg_start = (U8)(root >> 16), fg_count = (U8)root;
    U8 afg = 0;
    for (U8 n = fg_start; n < fg_start + fg_count; n++) {
        if ((param(n, PAR_FG_TYPE) & 0xFF) == 0x01) { afg = n; break; }
    }
    if (!afg) return FALSE;
    verb12(afg, VERB_SET_POWER, 0x00);

    U32 sub = param(afg, PAR_NODE_COUNT);
    U8 w_start = (U8)(sub >> 16), w_count = (U8)sub;
    for (U8 n = w_start; n < w_start + w_count; n++) {
        U32 caps = param(n, PAR_WIDGET_CAPS);
        U32 type = (caps >> 20) & 0xF;
        if (type == WIDGET_OUTPUT && !g.dac) g.dac = n;
        if (type == WIDGET_PIN && !g.pin && (param(n, PAR_PIN_CAPS) & (1U << 4))) {
            g.pin = n;
            g.pin_has_amp = (caps & WCAP_OUT_AMP) != 0;
        }
    }
    if (!g.dac || !g.pin) return FALSE;
    U32 amp = param(g.dac, PAR_OUT_AMP_CAPS);
    if (!amp) amp = param(afg, PAR_OUT_AMP_CAPS);      // default amp caps of the group
    g.amp_0db = (U8)(amp & 0x7F);                       // "offset": the step that is 0 dB
    U32 pcm = param(g.dac, PAR_PCM);
    if (!pcm) pcm = param(afg, PAR_PCM);                // default formats of the group
    g.pcm_rates = pcm & 0xFFF;
    if (!(pcm & (1U << 17))) g.pcm_rates = 0;           // no 16-bit support: no usable rate
    g.rate = HDA_RATE;
    return TRUE;
}

// ---- Pisano counter -----------------------------------------------------------

static U0 pisano_reset(U0) {
    g.fib_prev = 0;
    g.fib_curr = 1;
    g.fib_step = 0;
}

static U0 pisano_step(U0) {
    U32 next = (g.fib_prev + g.fib_curr) & RING_MASK;
    g.fib_prev = g.fib_curr;
    g.fib_curr = next;
    if (++g.fib_step == HDA_PISANO_PERIOD) {
        if (g.fib_prev == 0 && g.fib_curr == 1) g.fib_periods++;
        g.fib_step = 0;
    }
}

// The Fibonacci sequence modulo 2^N first returns to (0, 1) after exactly
// 3 * 2^(N-1) steps. Checked once at init.
static Bool pisano_verify(U0) {
    U32 a = 0, b = 1;
    for (U32 i = 1; i <= HDA_PISANO_PERIOD; i++) {
        U32 c = (a + b) & RING_MASK;
        a = b;
        b = c;
        if (a == 0 && b == 1) return i == HDA_PISANO_PERIOD;
    }
    return FALSE;
}

// ---- controller ---------------------------------------------------------------

Bool hda_init(U0) {
    MemSet(&g, 0, sizeof(g));
    PciDevice dev;
    if (!pci_find_class(0x04, 0x03, 0, &dev)) return FALSE;    // multimedia / HD Audio
    g.present = TRUE;
    g.slot = dev.slot;
    if (!pci_enable(&dev) || !dev.bar[0]) {
        kprintf("[HDA] controller at %02x.%u: no MMIO BAR\n", dev.slot, dev.func);
        return FALSE;
    }
    g.mmio = dev.bar[0];

    U16 gcap = mmio_read16(g.mmio + REG_GCAP);
    g.num_oss = (gcap >> 12) & 0xF;
    g.num_iss = (gcap >> 8) & 0xF;
    if (!g.num_oss) {
        kprintf("[HDA] controller has no output stream\n");
        return FALSE;
    }

    mmio_write32(g.mmio + REG_GCTL, 0);                 // enter reset
    U64 end = timer_get_uptime_ms() + 50;
    while ((mmio_read32(g.mmio + REG_GCTL) & GCTL_CRST) && timer_get_uptime_ms() < end) {}
    mmio_write32(g.mmio + REG_GCTL, GCTL_CRST);         // leave reset
    end = timer_get_uptime_ms() + 50;
    while (!(mmio_read32(g.mmio + REG_GCTL) & GCTL_CRST) && timer_get_uptime_ms() < end) {}
    SleepMS(2);                                         // codecs need 521 us to report

    U16 codecs = mmio_read16(g.mmio + REG_STATESTS) & 0x7FFF;
    if (!codecs) {
        kprintf("[HDA] no codec on the link\n");
        return FALSE;
    }
    while (!(codecs & (1U << g.cad))) g.cad++;
    if (!discover_codec()) {
        kprintf("[HDA] codec %u: no output converter or output pin\n", g.cad);
        return FALSE;
    }

    s_sd = g.mmio + 0x80 + (uintptr_t)g.num_iss * 0x20;
    g.pisano_verified = pisano_verify();
    pisano_reset();
    g.ready = TRUE;
    kprintf("[HDA] HD Audio %u.%u at PCI %02x.0: codec %u, DAC node %u, pin node %u, 0 dB at step %u\n",
            mmio_read8(g.mmio + REG_VMAJ), mmio_read8(g.mmio + REG_VMIN), g.slot, g.cad, g.dac, g.pin, g.amp_0db);
    return TRUE;
}

static Bool stream_reset(U0) {
    mmio_write32(s_sd + SD_CTL, SD_CTL_SRST);
    U64 end = timer_get_uptime_ms() + 20;
    while (!(mmio_read32(s_sd + SD_CTL) & SD_CTL_SRST)) if (timer_get_uptime_ms() > end) return FALSE;
    mmio_write32(s_sd + SD_CTL, 0);
    end = timer_get_uptime_ms() + 20;
    while (mmio_read32(s_sd + SD_CTL) & SD_CTL_SRST) if (timer_get_uptime_ms() > end) return FALSE;
    return TRUE;
}

Bool hda_prepare(U0) {
    if (!g.ready) return FALSE;
    hda_cancel();
    if (g.playing) hda_stop();
    if (!stream_reset()) {
        kprintf("[HDA] stream reset timed out\n");
        return FALSE;
    }

    // Codec path: DAC on stream 1 channel 0, 16-bit stereo at g.rate, 0 dB; pin out.
    verb12(g.dac, VERB_SET_POWER, 0x00);
    verb4(g.dac, VERB4_SET_FORMAT, stream_format());
    verb12(g.dac, VERB_SET_STREAM_CH, (STREAM_TAG << 4) | 0);
    verb4(g.dac, VERB4_SET_AMP, AMP_OUT_LR | g.amp_0db);
    verb12(g.pin, VERB_SET_POWER, 0x00);
    verb12(g.pin, VERB_SET_PIN_CTL, 0x40);              // output enable
    verb12(g.pin, VERB_SET_EAPD, 0x02);
    // Only a pin that has its own output amp gets an amp command: on codecs
    // without one (QEMU's), the command lands on the DAC's amp and mutes it.
    if (g.pin_has_amp) verb4(g.pin, VERB4_SET_AMP, AMP_OUT_LR | (U8)(param(g.pin, PAR_OUT_AMP_CAPS) & 0x7F));

    MemSet(s_ring, 0, sizeof(s_ring));
    for (U32 i = 0; i < BDL_ENTRIES; i++) {
        U64 addr = (U64)(uintptr_t)(s_ring + i * (RING_BYTES / BDL_ENTRIES));
        s_bdl[i].addr_lo = (U32)addr;
        s_bdl[i].addr_hi = (U32)(addr >> 32);
        s_bdl[i].length = RING_BYTES / BDL_ENTRIES;
        s_bdl[i].flags = 0;                             // no interrupt: polled
    }
    io_fence();
    U64 bdl = (U64)(uintptr_t)s_bdl;
    mmio_write32(s_sd + SD_BDPL, (U32)bdl);
    mmio_write32(s_sd + SD_BDPU, (U32)(bdl >> 32));
    mmio_write32(s_sd + SD_CBL, RING_BYTES);
    mmio_write16(s_sd + SD_LVI, BDL_ENTRIES - 1);
    mmio_write16(s_sd + SD_FMT, stream_format());
    mmio_write8(s_sd + SD_STS, SD_STS_CLEAR);

    g.frames_written = 0;
    g.frames_played = 0;
    g.last_lpib = 0;
    pisano_reset();
    g.playing = TRUE;                                   // writes are accepted from now on
    g.running = FALSE;
    return TRUE;
}

U0 hda_run(U0) {
    if (!g.playing || g.running) return;
    io_fence();
    mmio_write32(s_sd + SD_CTL, ((U32)STREAM_TAG << 20) | SD_CTL_RUN);
    g.running = TRUE;
    g.last_pump_ms = timer_get_uptime_ms();
}

U0 hda_pump(U0) {
    if (!g.running) return;
    // Held up for longer than the ring lasts: the position wrapped around an
    // unknown number of times and old samples were played. Count it and
    // start again from what the DMA plays now.
    U64 now = timer_get_uptime_ms();
    if (now - g.last_pump_ms > (U64)(HDA_RING_FRAMES - WRITE_GUARD) * 1000 / g.rate) {
        g.underruns++;
        g.last_lpib = mmio_read32(s_sd + SD_LPIB) % RING_BYTES;
        g.frames_written = g.frames_played;
        g.last_pump_ms = now;
        return;
    }
    g.last_pump_ms = now;
    U32 lpib = mmio_read32(s_sd + SD_LPIB) % RING_BYTES;
    U32 delta = (lpib + RING_BYTES - g.last_lpib) % RING_BYTES;
    g.last_lpib = lpib;
    g.frames_played += delta / HDA_FRAME_BYTES;
    if (g.frames_played > g.frames_written) {           // the DMA ran past the data
        g.underruns++;
        g.frames_written = g.frames_played;
    }
}

U32 hda_queued_frames(U0) {
    return (U32)(g.frames_written - g.frames_played);
}

U32 hda_write(const I16* stereo, U32 frames) {
    if (!g.playing || !stereo) return 0;
    hda_pump();
    U32 queued = hda_queued_frames();
    U32 room = HDA_RING_FRAMES - WRITE_GUARD;
    U32 space = queued < room ? room - queued : 0;
    U32 n = frames < space ? frames : space;
    I16* ring = (I16*)s_ring;
    for (U32 i = 0; i < n; i++) {
        U32 at = (U32)((g.frames_written + i) & RING_MASK) * 2;
        ring[at] = stereo[2 * i];
        ring[at + 1] = stereo[2 * i + 1];
    }
    io_fence();
    g.frames_written += n;
    if (n) pisano_step();
    return n;
}

U0 hda_stop(U0) {
    if (!g.ready) return;
    mmio_write32(s_sd + SD_CTL, 0);
    g.playing = FALSE;
    g.running = FALSE;
}

// ---- tone generator (phase accumulator, Q15 sine table) --------------------------

U64 hda_play(HdaFill fill, U0* ctx) {
    if (!g.ready || !fill || !hda_prepare()) return 0;
    I16 block[2 * HDA_FILL_FRAMES];
    U64 total = 0;
    U32 n;
    while ((n = fill(block, HDA_FILL_FRAMES, ctx)) != 0) {
        U32 off = 0;
        while (off < n) {
            U32 w = hda_write(block + 2 * off, n - off);
            off += w;
            if (!w) {
                hda_run();                              // the ring is full: start playing
                SleepMS(2);
            }
        }
        total += n;
    }
    hda_run();                                          // short sounds fit in the ring
    // Drain: keep the ring full of silence until everything has gone out,
    // so the DMA never wraps onto old samples, then stop.
    U64 end = g.frames_written;
    MemSet(block, 0, sizeof(block));
    U64 deadline = timer_get_uptime_ms() + 2000 + total * 1000 / g.rate;
    while (g.frames_played < end + 2048 && timer_get_uptime_ms() < deadline) {
        if (!hda_write(block, HDA_FILL_FRAMES)) SleepMS(2);
    }
    hda_stop();
    return total;
}

// ---- background playback --------------------------------------------------------

static struct {
    Bool    active, ended;
    HdaFill fill;
    U0*     ctx;
    HdaDone done;
    I16     block[2 * HDA_FILL_FRAMES];
    U32     have, off;                  // frames in block, frames of it already written
    U64     total, end, deadline_ms;
} s_bg;

Bool hda_busy(U0) { return s_bg.active; }

static U0 bg_finish(Bool cancelled) {
    hda_stop();
    s_bg.active = FALSE;
    if (s_bg.done) s_bg.done(s_bg.total, cancelled, s_bg.ctx);
}

U0 hda_cancel(U0) {
    if (s_bg.active) bg_finish(TRUE);
}

U0 hda_service(U0) {
    if (!s_bg.active) return;
    while (!s_bg.ended) {
        if (s_bg.off == s_bg.have) {
            s_bg.have = s_bg.fill(s_bg.block, HDA_FILL_FRAMES, s_bg.ctx);
            s_bg.off = 0;
            if (s_bg.have == 0) {
                s_bg.ended = TRUE;
                s_bg.end = g.frames_written;
                s_bg.deadline_ms = timer_get_uptime_ms() + 2000 + (U64)HDA_RING_FRAMES * 1000 / g.rate;
                hda_run();                      // a short sound may not have filled the ring
                MemSet(s_bg.block, 0, sizeof(s_bg.block));
                break;
            }
            s_bg.total += s_bg.have;
        }
        U32 w = hda_write(s_bg.block + 2 * s_bg.off, s_bg.have - s_bg.off);
        s_bg.off += w;
        if (!w) {
            hda_run();                          // the ring is full: playing (or still playing)
            return;
        }
    }
    // The producer is done: silence after the end until it has played out.
    while (hda_write(s_bg.block, HDA_FILL_FRAMES)) {}
    if (g.frames_played >= s_bg.end + 2048 || timer_get_uptime_ms() > s_bg.deadline_ms) bg_finish(FALSE);
}

Bool hda_start(HdaFill fill, U0* ctx, HdaDone done) {
    if (!g.ready || !fill || !hda_prepare()) return FALSE;
    MemSet(&s_bg, 0, sizeof(s_bg));
    s_bg.fill = fill;
    s_bg.ctx = ctx;
    s_bg.done = done;
    s_bg.active = TRUE;
    hda_service();                              // fill the ring now
    return TRUE;
}

typedef struct {
    U64 left;
    U32 phase, step;
    I32 amp;
} ToneCtx;

static U32 tone_fill(I16* stereo, U32 max, U0* p) {
    ToneCtx* t = (ToneCtx*)p;
    U32 n = t->left < max ? (U32)t->left : max;
    for (U32 i = 0; i < n; i++) {
        I16 s = (I16)((g_sine_q15[t->phase >> 22] * t->amp) >> 15);
        stereo[2 * i] = s;
        stereo[2 * i + 1] = s;
        t->phase += t->step;
    }
    t->left -= n;
    return n;
}

U32 hda_tone(U32 hz, U32 ms, U32 level_pct) {
    if (!g.ready || hz == 0 || hz >= HDA_RATE / 2 || !hda_set_rate(HDA_RATE)) return 0;
    if (level_pct > 100) level_pct = 100;
    ToneCtx t;
    t.left = (U64)ms * HDA_RATE / 1000;
    t.step = (U32)(((U64)hz << 32) / HDA_RATE);         // phase increment per frame
    t.phase = 0;
    t.amp = (I32)(32767 * level_pct / 100);
    return (U32)hda_play(tone_fill, &t);
}
