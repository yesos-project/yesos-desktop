#!/usr/bin/env python3
"""
Audio test: boots YesOS Desktop with an Intel HD Audio controller whose output
QEMU records into a WAV file (48 kHz, 16-bit stereo), plays two tones from the
console, then test signals through the EML chain (`audio play`: resampler,
dither, limiter), and analyses the recording: frequency, duration, level,
both channels equal, SINAD (signal against noise and distortion), limiter
peak and no underruns. Also checks the Pisano counter reported by `audio`.
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

RATE = 48000
failures = []
# command, Hz, ms, level %, minimum SINAD (dB). The thresholds sit a few dB
# under what is measured today, to catch regressions.
TONES = [
    ("audio tone 1000 1500 50", 1000, 1500, 50, 50),                # Q15 table, no interpolation
    ("audio tone 440 1000 25", 440, 1000, 25, 50),
    ("audio play 1000 1000 50", 1000, 1000, 50, 84),                # 48 kHz, TPDF dither
    ("audio play 1000 1000 50 44100 eml", 1000, 1000, 50, 78),      # EML resampler
    ("audio play 1000 1000 50 44100 linear", 1000, 1000, 50, 58),   # linear resampler
    ("audio play 1000 1000 100 48000 limit", 1000, 1000, None, None),
]
# Soft limiter: a full-scale peak (32767) goes to thresh + head*d/(head + d).
LIM_T = 26000
LIM_PEAK = LIM_T + (32767 - LIM_T) * (32767 - LIM_T) // (2 * (32767 - LIM_T))


def check(cond, label):
    print(("  PASS  " if cond else "  FAIL  ") + label)
    if not cond:
        failures.append(label)


class AudioYesOS(harness.YesOS):
    def __init__(self, disk, host_port, wav):
        orig = harness.subprocess.Popen

        def popen(cmd, *a, **kw):
            cmd = list(cmd) + [
                "-audiodev", f"wav,id=snd0,path={wav},out.frequency={RATE},out.channels=2,out.format=s16",
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


def main():
    tmpdir = tempfile.mkdtemp(prefix="yesos_audio_")
    disk = os.path.join(tmpdir, "disk.img")
    wav = os.path.join(tmpdir, "out.wav")
    shutil.copyfile(harness.SRC_DISK, disk)
    vm = AudioYesOS(disk, 40000 + os.getpid() % 20000, wav)
    try:
        check(vm.wait_for("YesOS [", 90), "system boots with an HD Audio controller")
        out = vm.output()
        check("[ OK ] AUDIO" in out, "audio output ready at boot")
        m = vm.send("audio", 2)
        res = vm.since(m)
        check("Pisano : period 24576 steps (checked at init: OK)" in res, "Pisano period 3*2^13 verified at init")

        for cmd, hz, ms, lvl, _ in TONES:
            m = vm.send(cmd, 0.5)
            done = vm.wait_for(" done: ", 20 + ms / 1000, m)
            res = vm.since(m)
            u = re.search(r"(?:tone|play) done: (\d+) frames, (\d+) underrun", res)
            check(done and u and int(u.group(1)) == ms * RATE // 1000,
                  f"'{cmd}': {u.group(1) if u else '?'} frames handed to the DMA")
            check(u and u.group(2) == "0", f"'{cmd}': no underruns")
            time.sleep(0.5)

        m = vm.send("audio", 2)
        res = vm.since(m)
        p = re.search(r"step (\d+), (\d+) full period", res)
        check(p is not None, "Pisano counter reported after playing")
        check("FATAL" not in vm.output() and "RISC-V EXCEPTION" not in vm.output(), "no kernel faults")
    finally:
        vm.quit()

    x = read_wav(wav)
    check(len(x) > RATE, f"QEMU recorded {len(x) / RATE:.2f} s of audio")
    segs = segments(x[:, 0]) if len(x) else []
    # `audio tone` writes the same sample to both channels; the dither of
    # `audio play` is independent per channel, so there they differ by a few LSB.
    tone_segs = [(s, e) for (cmd, *_), (s, e) in zip(TONES, segs) if cmd.startswith("audio tone")]
    check(tone_segs and all(np.array_equal(x[s:e, 0], x[s:e, 1]) for s, e in tone_segs),
          "audio tone: left and right channels are identical")
    diff = int(np.abs(x[:, 0].astype(np.int32) - x[:, 1]).max()) if len(x) else -1
    check(0 <= diff <= 8, f"left and right never differ by more than 8 LSB (max {diff})")
    check(len(segs) == len(TONES), f"{len(segs)} tone(s) found in the recording (expected {len(TONES)})")
    for (cmd, hz, ms, lvl, min_sinad), (s, e) in zip(TONES, segs):
        dur = (e - s) * 1000 / RATE
        check(abs(dur - ms) <= ms * 0.02 + 5, f"'{cmd}' lasts {dur:.1f} ms (expected {ms})")
        mid = x[s + (e - s) // 10: e - (e - s) // 10, 0]
        freq, level, sinad = analyse(mid)
        check(abs(freq - hz) < 0.5, f"'{cmd}' measured at {freq:.2f} Hz")
        if lvl is not None:
            want = 20 * np.log10(lvl / 100 / np.sqrt(2))
            check(abs(level - want) < 0.5, f"'{cmd}' level {level:.2f} dBFS (expected {want:.2f})")
        if min_sinad is not None:
            check(sinad > min_sinad, f"'{cmd}' SINAD {sinad:.1f} dB (more than {min_sinad})")
        if "limit" in cmd:
            peak = int(np.abs(mid.astype(np.int32)).max())
            check(abs(peak - LIM_PEAK) <= 3, f"'{cmd}' peak {peak} (soft-limiter curve gives {LIM_PEAK})")
    shutil.rmtree(tmpdir, ignore_errors=True)

    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED")
        sys.exit(1)
    print("All checks passed")


if __name__ == "__main__":
    main()
