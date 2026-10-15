#ifndef EML_AUDIO_H
#define EML_AUDIO_H

#include "types.h"

// EML audio processing, brought over from yesos-lab (eml/eml_audio_physics.c):
// sample-rate conversion, TPDF dither and a soft limiter. Integer only.
//
// "S32" samples carry a 16-bit sample in the top half of an I32 (x << 16),
// so that a stage can hand more than 16 bits of precision to the next one.
// All buffers are interleaved stereo.

#define EML_SINC_TAPS     8
#define EML_SINC_PHASES   64
#define EML_RS_HISTORY    16

typedef struct {
    U64 step_q32;                         // (in_rate << 32) / out_rate
    I64 pos_q32;                          // position in the current input block
    I16 hist[2][EML_RS_HISTORY];          // last input frames of the previous block
    Bool primed;
} EmlResampler;

U0  eml_resampler_init(EmlResampler* r, U32 in_rate, U32 out_rate);
// Convert one block. `out` must hold in_frames * out_rate / in_rate + 2
// frames: the whole block is always consumed. Returns the frames written.
// EML: 8-tap Blackman-windowed sinc, 64 phases with linear interpolation
// between neighbouring phases.
U32 eml_resample(EmlResampler* r, const I16* in, U32 in_frames, I32* out_s32);
// The conventional reference: linear interpolation between two input frames,
// with the same delay and block handling as eml_resample.
U32 linear_resample(EmlResampler* r, const I16* in, U32 in_frames, I32* out_s32);

// TPDF dither to 16 bits: triangular noise of +-1 LSB from a xorshift64*
// generator per channel, then rounding. With `shaping` the quantisation
// error is fed back (noise transfer 1 - 0.75 z^-1), which moves noise
// power towards high frequencies.
typedef struct {
    U64  rng[2];
    I32  err[2];
    Bool shaping;
} EmlDither;

U0  eml_dither_init(EmlDither* d, U64 seed, Bool shaping);
I16 eml_dither16(EmlDither* d, I32 s32, U32 ch);
I16 round16(I32 s32);                     // plain rounding, no dither

// Stereo-linked soft limiter on S32 frames: peaks above the threshold
// (26000, about -2 dBFS) follow thresh + head*d/(head + d) and never reach
// full scale; both channels get the same gain. The lab version could also
// add a second harmonic ("warmth"); that is left out.
#define EML_LIMIT_THRESH  26000
U0  eml_limit(I32* stereo_s32, U32 frames);

#endif // EML_AUDIO_H
