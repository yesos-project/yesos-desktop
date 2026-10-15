// ============================================================================
// YesOS Desktop - WAV (RIFF/WAVE) parser: 16-bit PCM, mono or stereo.
// ============================================================================

#include "wav.h"
#include "memory.h"

static inline U32 le32(const U8* p) { return (U32)p[0] | ((U32)p[1] << 8) | ((U32)p[2] << 16) | ((U32)p[3] << 24); }
static inline U16 le16(const U8* p) { return (U16)(p[0] | (p[1] << 8)); }
static inline Bool tag(const U8* p, const char* t) { return p[0] == t[0] && p[1] == t[1] && p[2] == t[2] && p[3] == t[3]; }

#define WAVE_FORMAT_PCM        0x0001
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE

const char* wav_parse(const U8* data, U64 size, WavInfo* out) {
    MemSet(out, 0, sizeof(*out));
    if (!data || size < 12 || !tag(data, "RIFF") || !tag(data + 8, "WAVE")) return "not a RIFF/WAVE file";
    Bool have_fmt = FALSE;
    U16 format = 0, channels = 0, bits = 0, align = 0;
    U32 rate = 0;
    U64 pos = 12;
    while (pos + 8 <= size) {
        const U8* ck = data + pos;
        U64 len = le32(ck + 4);
        U64 body = pos + 8;
        if (len > size - body) {
            if (!tag(ck, "data")) return "a chunk runs past the end of the file";
            len = size - body;                          // truncated data chunk: play what is there
        }
        if (tag(ck, "fmt ")) {
            if (len < 16) return "fmt chunk too short";
            format = le16(ck + 8);
            channels = le16(ck + 10);
            rate = le32(ck + 12);
            align = le16(ck + 20);
            bits = le16(ck + 22);
            if (format == WAVE_FORMAT_EXTENSIBLE) {
                if (len < 40) return "WAVE_FORMAT_EXTENSIBLE chunk too short";
                format = le16(ck + 32);                 // first two bytes of the sub-format GUID
            }
            have_fmt = TRUE;
        } else if (tag(ck, "data")) {
            if (!have_fmt) return "data chunk before the fmt chunk";
            if (format != WAVE_FORMAT_PCM) return "not PCM (compressed WAV files are not supported)";
            if (bits != 16) return "only 16-bit samples are supported";
            if (channels != 1 && channels != 2) return "only mono and stereo are supported";
            if (align != channels * 2) return "inconsistent block alignment";
            if (rate == 0) return "sample rate is 0";
            if ((body & 1) != 0) return "misaligned sample data";
            out->rate = rate;
            out->channels = channels;
            out->samples = (const I16*)(data + body);
            out->frames = (U32)(len / align);
            return out->frames ? NULL : "no samples";
        }
        pos = body + len + (len & 1);                   // chunks are padded to even sizes
    }
    return have_fmt ? "no data chunk" : "no fmt chunk";
}
