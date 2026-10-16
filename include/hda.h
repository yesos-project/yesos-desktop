#ifndef HDA_H
#define HDA_H

#include "types.h"

// Intel High Definition Audio output (QEMU `intel-hda` + `hda-output`).
// One output stream: 16-bit stereo at a rate the codec supports (48 kHz by
// default), a power-of-two DMA ring walked linearly. Integer arithmetic only.

#define HDA_RATE            48000                       // default rate, used by tones
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
    U32       rate;                     // current stream rate, Hz
    U32       pcm_rates;                // codec's supported-rate bits (HDA spec 7.3.4.7)
    U64       frames_written, frames_played, underruns;
    U32       last_lpib;
    U64       last_pump_ms;             // when the DMA position was last read
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
Bool      hda_rate_supported(U32 hz);
Bool      hda_set_rate(U32 hz);         // while stopped; FALSE if the codec cannot

// Stream sound produced on the fly at the current rate: `fill` writes up to
// `max` stereo frames and returns how many (0 = finished). Blocks until
// everything has been played. Returns the number of frames played.
#define HDA_FILL_FRAMES     512
typedef U32 (*HdaFill)(I16* stereo, U32 max, U0* ctx);
U64       hda_play(HdaFill fill, U0* ctx);

// Background playback: the same producer, but hda_start() returns at once
// and hda_service(), called from the main loop, keeps the ring filled (the
// ring holds about 341 ms, the loop runs every millisecond). When the
// producer is done and the ring has played out, or on hda_cancel(), the
// stream stops and `done` is called. hda_play() and hda_prepare() cancel a
// background sound first. If the main loop is held up for longer than the
// ring, the DMA plays old samples: that is counted as an underrun.
typedef U0 (*HdaDone)(U64 frames, Bool cancelled, U0* ctx);
Bool      hda_start(HdaFill fill, U0* ctx, HdaDone done);
U0        hda_service(U0);
Bool      hda_busy(U0);                 // a background sound is playing
U0        hda_cancel(U0);

// Play a sine tone at 48 kHz: frequency in Hz, duration in ms, level in
// percent of full scale. Blocks until it has been played. Returns frames played.
U32       hda_tone(U32 hz, U32 ms, U32 level_pct);

// Play a WAV file from the RedSea disk at its own rate (blocks until it has
// been played; reports on the console). NULL if it played, else why not.
const char* audio_play_file(const char* path, U64* frames_played);
// The same in the background: returns at once (NULL if it started, else
// why not); `done` (may be NULL) is called from the main loop at the end.
// A sound already playing in the background is stopped first.
typedef U0 (*AudioDoneFn)(const char* path, U64 frames, Bool stopped, U0* ctx);
const char* audio_start_file(const char* path, AudioDoneFn done, U0* ctx);
U0        audio_shell_register(U0);

#endif // HDA_H
