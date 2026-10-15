#ifndef HDA_H
#define HDA_H

#include "types.h"

// Intel High Definition Audio output (QEMU `intel-hda` + `hda-output`).
// One output stream: 48 kHz, 16-bit stereo, a power-of-two DMA ring walked
// linearly. Integer arithmetic only.

#define HDA_RATE            48000
#define HDA_RING_LOG2       14                          // 16384 frames, ~341 ms
#define HDA_RING_FRAMES     (1U << HDA_RING_LOG2)
#define HDA_FRAME_BYTES     4                           // 16-bit stereo
// Pisano period of the Fibonacci sequence modulo 2^N: pi(2^N) = 3 * 2^(N-1).
#define HDA_PISANO_PERIOD   (3U << (HDA_RING_LOG2 - 1))

typedef struct {
    Bool      present, ready, playing, running;
    uintptr_t mmio;
    U8        slot, cad;                // PCI slot, codec address
    U8        dac, pin;                 // codec nodes in use
    U8        amp_0db;                  // DAC output amp step for 0 dB
    Bool      pin_has_amp;
    U8        num_iss, num_oss;
    U64       frames_written, frames_played, underruns;
    U32       last_lpib;
    // Pisano counter: steps the Fibonacci recurrence modulo the ring size on
    // every write and wraps after exactly one Pisano period. Telemetry only:
    // the samples are placed linearly in the ring.
    U32       fib_prev, fib_curr, fib_step;
    U64       fib_periods;
    Bool      pisano_verified;          // the period was checked at init
} HdaState;

Bool      hda_init(U0);                 // find, reset and configure; FALSE if absent
HdaState* hda_state(U0);
Bool      hda_prepare(U0);              // program the stream; the DMA is not running yet
U0        hda_run(U0);                  // start the DMA (after the ring has been filled)
U32       hda_write(const I16* stereo, U32 frames);   // frames accepted (ring space)
U0        hda_pump(U0);                 // follow the DMA position, count underruns
U0        hda_stop(U0);
U32       hda_queued_frames(U0);        // written but not yet played

// Play a sine tone: frequency in Hz, duration in ms, level in percent of
// full scale. Blocks until it has been played. Returns frames played.
U32       hda_tone(U32 hz, U32 ms, U32 level_pct);

U0        audio_shell_register(U0);

#endif // HDA_H
