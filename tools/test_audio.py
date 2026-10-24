#!/usr/bin/env python3
"""
Audio test: boots YesOS Desktop with an Intel HD Audio controller whose output
QEMU records into a WAV file (48 kHz, 16-bit stereo), plays two tones from the
console and analyses the recording: frequency, duration, level, both channels
equal, SINAD (signal against noise and distortion) and no underruns. Also
checks the Pisano counter reported by `audio`.
Then WAV files on the RedSea disk (`audio play <file>`): a 48 kHz stereo and
a 44.1 kHz mono file must come out of the recording bit for bit (the second
in a run recorded at 44.1 kHz), and malformed files must be refused.
Rates the codec lacks are resampled to 48 kHz with yesmath (core module
"math"): an 11 025 Hz mono and a 24 000 Hz stereo file must come out bit for
bit equal to the same resampler built on the host (core/math, ctypes), and
their tones at the right frequency with a SINAD above 50 dB; a 192 kHz file
(above every rate the codec has: no resampling down) is refused.
Non-zero exit status on failure.
"""
import os
import re
import shutil
import signal
import struct
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "core", "tools"))
import yesos_harness as harness  # noqa: E402
import redsea_injector  # noqa: E402
import ctypes  # noqa: E402
import subprocess  # noqa: E402

RATE = 48000
failures = []
TONES = [(1000, 1500, 50), (440, 1000, 25)]       # Hz, ms, level %


def check(cond, label):
    print(("  PASS  " if cond else "  FAIL  ") + label)
    if not cond:
        failures.append(label)


class AudioYesOS(harness.YesOS):
    def __init__(self, disk, host_port, wav, rate=RATE):
        orig = harness.subprocess.Popen

        def popen(cmd, *a, **kw):
            cmd = list(cmd) + [
                "-audiodev", f"wav,id=snd0,path={wav},out.frequency={rate},out.channels=2,out.format=s16",
                "-device", "intel-hda,id=hda0", "-device", "hda-output,bus=hda0.0,audiodev=snd0"] + (["-trace", "hda_audio_*"] if os.environ.get("HDA_TRACE") else [])
            return orig(cmd, *a, **kw)

        harness.subprocess.Popen = popen
        try:
            super().__init__(disk, host_port)
        finally:
            harness.subprocess.Popen = orig

    def quit(self):
        """SIGTERM lets QEMU close the WAV file properly."""
        self.p.send_signal(signal.SIGTERM)
        try:
            self.p.wait(10)
        except Exception:
            self.p.kill()
            self.p.wait()


def read_wav(path):
    data = open(path, "rb").read()
    i = data.find(b"data")
    if i < 0:
        return np.zeros((0, 2), dtype=np.int16)
    pcm = data[i + 8:]
    pcm = pcm[: len(pcm) // 4 * 4]
    return np.frombuffer(pcm, dtype="<i2").reshape(-1, 2)


def segments(x, thresh=200, min_gap=RATE // 100):
    """(start, end) of the non-silent stretches of the left channel."""
    loud = np.abs(x.astype(np.int32)) > thresh
    idx = np.flatnonzero(loud)
    if idx.size == 0:
        return []
    out, start, prev = [], idx[0], idx[0]
    for k in idx[1:]:
        if k - prev > min_gap:
            out.append((start, prev + 1))
            start = k
        prev = k
    out.append((start, prev + 1))
    return out


def analyse(sig):
    """Frequency (Hz), RMS level (dBFS) and SINAD (dB) of a stretch of tone."""
    s = sig.astype(np.float64)
    n = len(s)
    w = np.blackman(n)
    spec = np.abs(np.fft.rfft(s * w)) ** 2
    k = int(np.argmax(spec[1:])) + 1
    # parabolic interpolation of the peak bin for the frequency
    a, b, c = np.log(spec[k - 1] + 1e-30), np.log(spec[k] + 1e-30), np.log(spec[k + 1] + 1e-30)
    freq = (k + 0.5 * (a - c) / (a - 2 * b + c)) * RATE / n
    band = 8                                     # Blackman main lobe +- a few bins
    sig_p = spec[max(1, k - band):k + band + 1].sum()
    noise_p = spec[1:].sum() - sig_p
    sinad = 10 * np.log10(sig_p / max(noise_p, 1e-30))
    rms = np.sqrt(np.mean(s ** 2))
    level = 20 * np.log10(rms / 32767 + 1e-30)
    return freq, level, sinad


def wav_bytes(rate, channels, samples, bits=16, extra_chunk_len=None):
    """A RIFF/WAVE file. `samples` is an int16 array (frames x channels)."""
    data = samples.astype("<i2").tobytes() if bits == 16 else (samples.astype(np.int16) // 256 + 128).astype(np.uint8).tobytes()
    align = channels * bits // 8
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * align, align, bits)
    fmt_len = len(fmt) if extra_chunk_len is None else extra_chunk_len
    body = b"WAVE" + b"fmt " + struct.pack("<I", fmt_len) + fmt + b"LIST" + struct.pack("<I", 4) + b"INFO" \
        + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


def test_signals():
    """Deterministic test content: tones, a sweep, noise and both extremes."""
    rng = np.random.default_rng(7)
    n48 = RATE // 2                                       # 0.5 s
    t = np.arange(n48) / RATE
    left = 0.6 * 32767 * np.sin(2 * np.pi * 1000 * t) + rng.integers(-300, 300, n48)
    right = rng.integers(-32768, 32768, n48).astype(np.float64)
    st = np.stack([left, right], axis=1).round().astype(np.int16)
    st[0] = (12345, -23456)                               # a marker to find the start
    st[1] = (32767, -32768)
    n44 = 44100 // 2
    t = np.arange(n44) / 44100
    mono = (0.5 * 32767 * np.sin(2 * np.pi * (200 + 9000 * t) * t)).round().astype(np.int16)
    mono[0], mono[1], mono[2] = 23456, -32768, 32767
    return st, mono.reshape(-1, 1)


def find_exact(rec, want):
    """Index where `want` (frames x 2) appears in `rec` bit for bit, else -1."""
    for i in np.flatnonzero(rec[:, 0] == want[0, 0]):
        if i + len(want) <= len(rec) and np.array_equal(rec[i:i + len(want)], want):
            return int(i)
    return -1


def resampled(lib, x, rate_in, rate_out=RATE):
    """What Desktop plays for one channel x at rate_in: the host build of the
    same yesmath resampler, x zero-padded as src/audio_shell.c does."""
    pad = np.concatenate([np.zeros(3, np.int16), x.astype(np.int16), np.zeros(5, np.int16)])
    n = (len(x) * rate_out) // rate_in + 16
    out = (ctypes.c_int16 * n)()
    pos = ctypes.c_uint64(3 << 32)
    m = lib.ym_resample_sinc8(pad.ctypes.data_as(ctypes.c_void_p), len(pad), out, n, ctypes.byref(pos),
                              (rate_in << 32) // rate_out)
    return np.array(out[:m], dtype=np.int16)


def host_yesmath(tmpdir):
    core = os.path.join(os.path.dirname(HERE), "core")
    so = os.path.join(tmpdir, "ym.so")
    subprocess.run(["cc", "-O2", "-DYM_HOST", "-shared", "-fPIC", "-I", os.path.join(core, "include"),
                    *(os.path.join(core, "math", f) for f in ("ym_math.c", "ym_tables.c", "ym_dispatch.c")),
                    "-o", so], check=True)
    lib = ctypes.CDLL(so)
    lib.ym_resample_sinc8.restype = ctypes.c_uint64
    lib.ym_resample_sinc8.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_uint64,
                                      ctypes.c_void_p, ctypes.c_uint64]
    return lib


def find_at(rec, want):
    """Like find_exact, keyed on want's first loud frame (the resampled files
    start near silence)."""
    k = int(np.flatnonzero(np.abs(want[:, 0].astype(int)) > 1000)[0])
    for i in np.flatnonzero(rec[:, 0] == want[k, 0]):
        j = i - k
        if j >= 0 and j + len(want) <= len(rec) and np.array_equal(rec[j:j + 16], want[:16]) \
                and np.array_equal(rec[j:j + len(want)], want):
            return int(j)
    return -1


def play(vm, name, timeout=20):
    m = vm.send(f"audio play {name}", 0.5)
    vm.wait_for("YesOS [", timeout, m)
    return vm.since(m)


def main():
    tmpdir = tempfile.mkdtemp(prefix="yesos_audio_")
    disk = os.path.join(tmpdir, "disk.img")
    wav = os.path.join(tmpdir, "out.wav")
    shutil.copyfile(harness.SRC_DISK, disk)
    stereo48, mono44 = test_signals()
    # rates this codec lacks: 1 kHz at 11 025 Hz mono; 1 and 3 kHz at 24 000 Hz stereo (half scale)
    t11, t24 = np.arange(11025) / 11025, np.arange(12000) / 24000
    tone11 = np.round(0.5 * 32767 * np.sin(2 * np.pi * 1000 * t11)).astype(np.int16).reshape(-1, 1)
    tones24 = np.stack([np.round(0.5 * 32767 * np.sin(2 * np.pi * f * t24)) for f in (1000, 3000)],
                       axis=1).astype(np.int16)
    lib = host_yesmath(tmpdir)
    want11 = np.repeat(resampled(lib, tone11[:, 0], 11025).reshape(-1, 1), 2, axis=1)
    want24 = np.stack([resampled(lib, tones24[:, c], 24000) for c in (0, 1)], axis=1)
    files = {
        "TONES48.WAV": wav_bytes(48000, 2, stereo48),
        "SWEEP44.WAV": wav_bytes(44100, 1, mono44),
        "NOTWAV.TXT": b"just some text, not a RIFF file\n",
        "EIGHTBIT.WAV": wav_bytes(48000, 1, mono44[:1000], bits=8),
        "BADCHUNK.WAV": wav_bytes(48000, 2, stereo48[:100], extra_chunk_len=1 << 30),
        "HIGHRATE.WAV": wav_bytes(192000, 2, stereo48[:100]),
        "TONE11.WAV": wav_bytes(11025, 1, tone11),
        "TONES24.WAV": wav_bytes(24000, 2, tones24),
    }
    for name, data in files.items():
        check(redsea_injector.inject_bytes(disk, data, name), f"{name} ({len(data)} bytes) injected into the RedSea disk")
    vm = AudioYesOS(disk, 40000 + os.getpid() % 20000, wav)
    try:
        check(vm.wait_for("YesOS [", 90), "system boots with an HD Audio controller")
        out = vm.output()
        check("[ OK ] AUDIO" in out, "audio output ready at boot")
        m = vm.send("audio", 2)
        res = vm.since(m)
        check("Pisano : period 24576 steps (checked at init: OK)" in res, "Pisano period 3*2^13 verified at init")

        for hz, ms, lvl in TONES:
            m = vm.send(f"audio tone {hz} {ms} {lvl}", 0.5)
            done = vm.wait_for("[AUDIO] tone done", 20 + ms / 1000, m)
            res = vm.since(m)
            u = re.search(r"tone done: (\d+) frames, (\d+) underrun", res)
            check(done and u and int(u.group(1)) == ms * RATE // 1000,
                  f"tone {hz} Hz: {u.group(1) if u else '?'} frames handed to the DMA")
            check(u and u.group(2) == "0", f"tone {hz} Hz: no underruns")
            time.sleep(0.5)

        m = vm.send("audio", 2)
        res = vm.since(m)
        p = re.search(r"step (\d+), (\d+) full period", res)
        check(p is not None, "Pisano counter reported after playing")
        rates = re.search(r"Rates  :([ \d]+) Hz", res)
        check(rates and " 44100" in rates.group(1) and " 48000" in rates.group(1),
              f"codec rates reported: {rates.group(1).strip() if rates else '?'} Hz")

        res = play(vm, "tones48.wav")
        check("48000 Hz, 16-bit, stereo, 24000 frames" in res and "play done: 24000 frames, 0 underrun" in res,
              "audio play tones48.wav: 48 kHz stereo, 24000 frames, no underruns")
        time.sleep(0.5)
        for name, why in (("notwav.txt", "not a RIFF/WAVE file"), ("eightbit.wav", "only 16-bit samples are supported"),
                          ("badchunk.wav", "a chunk runs past the end of the file"),
                          ("highrate.wav", "192000 Hz is a rate above every rate this codec has"),
                          ("missing.wav", "no such file")):
            check(why in play(vm, name, 5), f"audio play {name}: refused ({why})")
        for name, rate, ch, want in (("tone11.wav", 11025, "mono", want11), ("tones24.wav", 24000, "stereo", want24)):
            res = play(vm, name)
            frames = len(tone11) if rate == 11025 else len(tones24)
            check(f"{rate} Hz, 16-bit, {ch}, {frames} frames" in res and "resampled to 48000 Hz" in res
                  and f"play done: {len(want)} frames, 0 underrun" in res,
                  f"audio play {name}: {rate} Hz {ch}, resampled to 48 kHz, {len(want)} frames, no underruns")
            time.sleep(0.5)
        check("FATAL" not in vm.output() and "RISC-V EXCEPTION" not in vm.output(), "no kernel faults")
    finally:
        vm.quit()

    x = read_wav(wav)
    check(len(x) > RATE, f"QEMU recorded {len(x) / RATE:.2f} s of audio")
    for name, rate, want, tones in (("tone11.wav", 11025, want11, (1000, 1000)),
                                    ("tones24.wav", 24000, want24, (1000, 3000))):
        at = find_at(x, want)
        check(at >= 0, f"{name} comes out bit for bit as yesmath's resampler on the host ({len(want)} frames)")
        n = np.arange(len(want))
        for c, f in enumerate(tones):
            ideal = 0.5 * 32767 * np.sin(2 * np.pi * f * n / RATE)
            mid = slice(16, len(want) - 16)                       # away from both ends
            err = want[mid, c].astype(float) - ideal[mid]
            sinad = 10 * np.log10(np.sum(ideal[mid] ** 2) / np.sum(err ** 2))
            freq = analyse(want[mid, c])[0]
            check(abs(freq - f) < 0.5 and sinad > 50,
                  f"{name} channel {c}: {f} Hz from {rate} Hz measured at {freq:.2f} Hz, SINAD {sinad:.1f} dB (> 50)")
    at = find_exact(x, stereo48)
    check(at >= 0, "tones48.wav comes out of the recording bit for bit (both channels, 24000 frames)")
    x = x[:at] if at >= 0 else x                          # the tone analysis below looks before it
    check(len(x) > 0 and np.array_equal(x[:, 0], x[:, 1]), "left and right channels are identical")
    segs = segments(x[:, 0]) if len(x) else []
    check(len(segs) == len(TONES), f"{len(segs)} tone(s) found in the recording (expected {len(TONES)})")
    for (hz, ms, lvl), (s, e) in zip(TONES, segs):
        dur = (e - s) * 1000 / RATE
        check(abs(dur - ms) <= ms * 0.02 + 5, f"{hz} Hz tone lasts {dur:.1f} ms (expected {ms})")
        mid = x[s + (e - s) // 10: e - (e - s) // 10, 0]
        freq, level, sinad = analyse(mid)
        want = 20 * np.log10(lvl / 100 / np.sqrt(2))
        check(abs(freq - hz) < 0.5, f"{hz} Hz tone measured at {freq:.2f} Hz")
        check(abs(level - want) < 0.5, f"{hz} Hz tone level {level:.2f} dBFS (expected {want:.2f})")
        check(sinad > 50, f"{hz} Hz tone SINAD {sinad:.1f} dB")

    # 44.1 kHz: a second run recorded at that rate, so QEMU does not resample
    wav44 = os.path.join(tmpdir, "out44.wav")
    vm = AudioYesOS(disk, 40000 + (os.getpid() + 1) % 20000, wav44, rate=44100)
    try:
        check(vm.wait_for("YesOS [", 90), "second boot, output recorded at 44.1 kHz")
        res = play(vm, "sweep44.wav")
        check("44100 Hz, 16-bit, mono, 22050 frames" in res and "play done: 22050 frames, 0 underrun" in res,
              "audio play sweep44.wav: 44.1 kHz mono, 22050 frames, no underruns")
        check("FATAL" not in vm.output() and "RISC-V EXCEPTION" not in vm.output(), "no kernel faults")
    finally:
        vm.quit()
    y = read_wav(wav44)
    check(find_exact(y, np.repeat(mono44, 2, axis=1)) >= 0,
          "sweep44.wav comes out bit for bit at 44.1 kHz, mono on both channels")
    shutil.rmtree(tmpdir, ignore_errors=True)

    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED")
        sys.exit(1)
    print("All checks passed")


if __name__ == "__main__":
    main()
