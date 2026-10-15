#ifndef WAV_H
#define WAV_H

#include "types.h"

// RIFF/WAVE files with 16-bit PCM samples, mono or stereo (format 1, or
// WAVE_FORMAT_EXTENSIBLE with the PCM sub-format). The data comes from
// disk, so every chunk size is checked against the size of the file.

typedef struct {
    U32       rate;            // frames per second
    U16       channels;        // 1 or 2
    const I16* samples;        // interleaved, little-endian, inside the file buffer
    U32       frames;
} WavInfo;

// NULL on success, else a short reason the file cannot be played.
const char* wav_parse(const U8* data, U64 size, WavInfo* out);

#endif // WAV_H
