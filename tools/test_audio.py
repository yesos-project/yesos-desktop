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
    files = {
        "TONES48.WAV": wav_bytes(48000, 2, stereo48),
        "SWEEP44.WAV": wav_bytes(44100, 1, mono44),
        "NOTWAV.TXT": b"just some text, not a RIFF file\n",
        "EIGHTBIT.WAV": wav_bytes(48000, 1, mono44[:1000], bits=8),
        "BADCHUNK.WAV": wav_bytes(48000, 2, stereo48[:100], extra_chunk_len=1 << 30),
        "ODDRATE.WAV": wav_bytes(12345, 2, stereo48[:100]),
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
                          ("oddrate.wav", "12345 Hz is not a rate this codec supports"),
                          ("missing.wav", "no such file")):
            check(why in play(vm, name, 5), f"audio play {name}: refused ({why})")
        check("FATAL" not in vm.output() and "RISC-V EXCEPTION" not in vm.output(), "no kernel faults")
    finally:
        vm.quit()

    x = read_wav(wav)
    check(len(x) > RATE, f"QEMU recorded {len(x) / RATE:.2f} s of audio")
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
