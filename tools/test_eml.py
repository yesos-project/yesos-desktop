#!/usr/bin/env python3
"""
EML audio measurement: quality and cost of the EML signal processing against
the plain methods, measured on YesOS itself.

1. Quality. YesOS plays sine test signals (`audio play`) and QEMU records
   them into a WAV file (bit-exact: QEMU does not touch the samples):
   - a 44.1 kHz 16-bit source converted to 48 kHz by the EML resampler and by
     linear interpolation, at 100 Hz to 20 kHz, then TPDF-dithered to 16 bits;
   - the same signals made at 48 kHz directly (no conversion): the floor that
     16-bit TPDF output allows;
   - a reference computed here in float64: a 44.1 kHz TPDF 16-bit source,
     resampled by a 256-tap Kaiser-windowed sinc, TPDF to 16 bits. It is
     what a near-ideal resampler gives from the same kind of source;
   - a quiet tone (1 % of full scale, -43 dBFS) quantised by rounding, TPDF
     dither and TPDF with noise shaping.
   SINAD: signal against noise and distortion over 0-24 kHz. SFDR: the tone
   against the largest other spectral line (for the resamplers it is the
   image of the tone left by the conversion).
2. Cost. YesOS boots under QEMU -icount and `audio bench` counts retired
   instructions (minstret) per stereo frame of each stage. Instructions, not
   cycles: QEMU does not model a pipeline or caches.

`--markdown` prints the tables for the README. Non-zero exit on failure.
"""
import os
import re
import shutil
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_audio as ta  # noqa: E402
from test_audio import RATE, BAND, analyse, spectrum, read_wav, segments  # noqa: E402

harness = ta.harness
failures = []
FREQS = [100, 1000, 3000, 6000, 10000, 15000, 20000]
LEVEL = 50                                        # percent of full scale
MS = 500
QUIET = [("round", "rounding, no dither"), ("tpdf", "TPDF dither"), ("shaped", "TPDF + noise shaping")]


def check(cond, label):
    print(("  PASS  " if cond else "  FAIL  ") + label)
    if not cond:
        failures.append(label)


def sfdr(sig):
    """Tone against the largest other line (dB) and where that line is (Hz)."""
    sp = spectrum(sig)
    k = int(np.argmax(sp[1:])) + 1
    rest = sp.copy()
    rest[:3] = 0
    rest[max(0, k - BAND):k + BAND + 1] = 0
    j = int(np.argmax(rest))
    return 10 * np.log10(sp[k] / rest[j]), j * RATE / len(sig)


def band_noise_db(sig, lo, hi):
    """Noise power between lo and hi Hz, without the tone, in dB relative to
    full-scale sine power (window-normalised)."""
    s = np.asarray(sig, dtype=np.float64)
    n = len(s)
    w = np.kaiser(n, 20)
    sp = np.abs(np.fft.rfft(s * w)) ** 2 / (np.sum(w ** 2) * n / 2)
    k = int(np.argmax(sp[1:])) + 1
    sp[max(0, k - BAND):k + BAND + 1] = 0
    a, b = int(lo * n / RATE), int(hi * n / RATE)
    return 10 * np.log10(sp[a:b].sum() / (32767.0 ** 2 / 2) + 1e-30)


def tpdf16(x, rng):
    d = rng.random(len(x)) - rng.random(len(x))
    return np.clip(np.round(x + d), -32768, 32767)


def reference(hz):
    """Near-ideal 44.1 -> 48 kHz conversion in float64 (256-tap Kaiser sinc,
    cut-off 21.6 kHz, about -120 dB stop band), TPDF 16-bit in and out."""
    rng = np.random.default_rng(hz)
    fin, n_out = 44100, RATE * MS // 1000
    n_in = n_out * fin // RATE + 300
    src = tpdf16(LEVEL / 100 * 32767 * np.sin(2 * np.pi * hz * np.arange(n_in) / fin + 0.3), rng)
    half, fc, beta = 128, 21600 / (fin / 2), 12.0
    t = 140 + np.arange(n_out) * fin / RATE        # output instants, in input samples
    out = np.empty(n_out)
    for a in range(0, n_out, 2048):
        tt = t[a:a + 2048, None]
        idx = np.floor(tt) + np.arange(-half + 1, half + 1)[None, :]
        x = tt - idx
        win = np.i0(beta * np.sqrt(np.clip(1 - (x / half) ** 2, 0, 1))) / np.i0(beta)
        h = fc * np.sinc(fc * x) * win
        out[a:a + 2048] = (h * src[idx.astype(int)]).sum(axis=1)
    return tpdf16(out, rng)


def mid(x, s, e):
    return x[s + (e - s) // 10: e - (e - s) // 10, 0]


def quality():
    cmds = []
    for hz in FREQS:
        for mode in ("48000", "44100 eml", "44100 linear"):
            cmds.append((f"audio play {hz} {MS} {LEVEL} {mode}", hz, MS))
    for q, _ in QUIET:
        cmds.append((f"audio play 1000 1000 1 48000 {q}", 1000, 1000))

    tmpdir = tempfile.mkdtemp(prefix="yesos_eml_")
    disk = os.path.join(tmpdir, "disk.img")
    wav = os.path.join(tmpdir, "out.wav")
    shutil.copyfile(harness.SRC_DISK, disk)
    vm = ta.AudioYesOS(disk, 40000 + os.getpid() % 20000, wav)
    try:
        check(vm.wait_for("YesOS [", 90), "system boots with an HD Audio controller")
        ok = True
        for cmd, hz, ms in cmds:
            m = vm.send(cmd, 0.3)
            done = vm.wait_for(" done: ", 20 + ms / 1000, m)
            u = re.search(r"play done: (\d+) frames, (\d+) underrun", vm.since(m))
            ok = ok and done and u and int(u.group(1)) == ms * RATE // 1000 and u.group(2) == "0"
            time.sleep(0.3)
        check(ok, f"{len(cmds)} test signals played in full, no underruns")
    finally:
        vm.quit()
    x = read_wav(wav)
    segs = segments(x[:, 0], thresh=150) if len(x) else []
    shutil.rmtree(tmpdir, ignore_errors=True)
    check(len(segs) == len(cmds), f"{len(segs)} signals found in the recording (expected {len(cmds)})")
    if len(segs) != len(cmds):
        return None
    res = {}
    for (cmd, hz, ms), (s, e) in zip(cmds, segs):
        sig = mid(x, s, e)
        _, level, sinad = analyse(sig)
        sf, at = sfdr(sig)
        res[cmd] = dict(level=level, sinad=sinad, sfdr=sf, at=at, sig=sig)
    return res


def bench():
    class IcountYesOS(harness.YesOS):
        def __init__(self, disk, port):
            orig = harness.subprocess.Popen
            harness.subprocess.Popen = lambda c, *a, **k: orig(list(c) + ["-icount", "shift=0"], *a, **k)
            try:
                super().__init__(disk, port)
            finally:
                harness.subprocess.Popen = orig

    tmpdir = tempfile.mkdtemp(prefix="yesos_bench_")
    disk = os.path.join(tmpdir, "disk.img")
    shutil.copyfile(harness.SRC_DISK, disk)
    vm = IcountYesOS(disk, 40000 + (os.getpid() + 7) % 20000)
    out = ""
    try:
        check(vm.wait_for("YesOS [", 300), "system boots under QEMU -icount")
        m = vm.send("audio bench", 1)
        check(vm.wait_for("bench done", 120, m), "audio bench finishes")
        out = vm.since(m)
    finally:
        vm.p.kill()
        vm.p.wait()
        shutil.rmtree(tmpdir, ignore_errors=True)
    b = {name: float(v) for name, v in re.findall(r"bench ([^:\n]+): ([\d.]+) instr/frame", out)}
    cal = b.get("calibration loop of 2 instructions", 0)
    check(2.0 <= cal <= 2.1, f"calibration loop: {cal:.2f} instructions per iteration (2 expected)")
    return b


def main():
    md = "--markdown" in sys.argv
    q = quality()
    if q is None:
        print(f"\n{len(failures)} check(s) FAILED")
        sys.exit(1)
    want = 20 * np.log10(LEVEL / 100 / np.sqrt(2))
    rows = []
    for hz in FREQS:
        d, e, l = q[f"audio play {hz} {MS} {LEVEL} 48000"], q[f"audio play {hz} {MS} {LEVEL} 44100 eml"], \
            q[f"audio play {hz} {MS} {LEVEL} 44100 linear"]
        ref = reference(hz)
        r_sinad, r_sfdr = analyse(ref)[2], sfdr(ref)[0]
        rows.append((hz, d, e, l, r_sinad, r_sfdr))
        check(abs(d["sinad"] - 87.3) < 1.5, f"{hz} Hz direct 48 kHz: SINAD {d['sinad']:.1f} dB (16-bit TPDF floor about 87.3)")
        if hz < 1000:
            # Both sit at the floor here. Linear interpolation even measures a
            # little better: its response falls towards high frequencies, so it
            # also removes some of the source's dither noise (and some signal).
            check(abs(e["sinad"] - r_sinad) < 1.5 and abs(l["sinad"] - r_sinad) < 1.5,
                  f"{hz} Hz: EML {e['sinad']:.1f} dB and linear {l['sinad']:.1f} dB at the reference {r_sinad:.1f} dB")
        else:
            check(e["sinad"] > l["sinad"] + 1, f"{hz} Hz: EML {e['sinad']:.1f} dB above linear {l['sinad']:.1f} dB")
        check(r_sinad >= e["sinad"] - 1, f"{hz} Hz: float64 reference {r_sinad:.1f} dB not below EML {e['sinad']:.1f} dB")
    e1, e10 = q[f"audio play 1000 {MS} {LEVEL} 44100 eml"], q[f"audio play 10000 {MS} {LEVEL} 44100 eml"]
    check(e1["sinad"] > 80, f"EML at 1 kHz: SINAD {e1['sinad']:.1f} dB (more than 80)")
    check(e10["sinad"] > 43, f"EML at 10 kHz: SINAD {e10['sinad']:.1f} dB (more than 43)")
    check(abs(q[f"audio play 1000 {MS} {LEVEL} 44100 eml"]["level"] - want) < 0.05, "EML at 1 kHz: level within 0.05 dB")

    quiet = {k: q[f"audio play 1000 1000 1 48000 {k}"] for k, _ in QUIET}
    for k in quiet:
        quiet[k]["low"] = band_noise_db(quiet[k]["sig"], 20, 4000)
        quiet[k]["high"] = band_noise_db(quiet[k]["sig"], 16000, 24000)
    check(quiet["tpdf"]["sfdr"] > quiet["round"]["sfdr"] + 10,
          f"-43 dBFS: TPDF SFDR {quiet['tpdf']['sfdr']:.1f} dB vs rounding {quiet['round']['sfdr']:.1f} dB (dither removes the distortion lines)")
    check(quiet["shaped"]["low"] < quiet["tpdf"]["low"] - 6,
          f"noise shaping: 20 Hz-4 kHz noise {quiet['shaped']['low']:.1f} vs {quiet['tpdf']['low']:.1f} dB")

    b = bench()

    print()
    print("| Tone | 48 kHz direct (floor) | float64 reference | EML 8-tap sinc | linear |")
    print("|---|---|---|---|---|")
    for hz, d, e, l, r_sinad, r_sfdr in rows:
        print(f"| {hz} Hz | {d['sinad']:.1f} dB | {r_sinad:.1f} / {r_sfdr:.1f} dB | "
              f"{e['sinad']:.1f} / {e['sfdr']:.1f} dB, {e['level'] - want:+.2f} dB | "
              f"{l['sinad']:.1f} / {l['sfdr']:.1f} dB, {l['level'] - want:+.2f} dB |")
    print()
    print("| 1 kHz at -43 dBFS | SINAD | SFDR | noise 20 Hz-4 kHz | noise 16-24 kHz |")
    print("|---|---|---|---|---|")
    for k, name in QUIET:
        v = quiet[k]
        print(f"| {name} | {v['sinad']:.1f} dB | {v['sfdr']:.1f} dB | {v['low']:.1f} dBFS | {v['high']:.1f} dBFS |")
    print()
    print("| Stage | Instructions per stereo frame | At 48 kHz |")
    print("|---|---|---|")
    for name, v in b.items():
        if name.startswith("calibration"):
            continue
        print(f"| {name} | {v:.1f} | {v * 48000 / 1e6:.2f} M instr/s |")
    if not md:
        print("\n(SINAD / SFDR; the level column is the gain error of the conversion)")

    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED")
        sys.exit(1)
    print("All checks passed")


if __name__ == "__main__":
    main()
