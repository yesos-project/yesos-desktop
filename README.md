# YesOS Desktop (RISC-V)

The desktop product of YesOS, built on the YesOS core (the `core` git
submodule): Machine mode only, a locked Z3-proven PMP, an integer-only kernel
(no floating point, checked at every build), no Linux, libc or binary blobs.

Today it is the core, its serial console and **HD Audio output** with the
EML signal processing (resampler, dither, soft limiter): no network services
and no optional modules. Features (display, input, windows, apps in PMP
cells) are added here one at a time, each with its own tests, brought over
piece by piece from the old desktop experiments in `yesos-lab`. The EML
processing is measured against the plain methods below
([EML against the plain methods](#eml-against-the-plain-methods)).

Design choices, a comparison with Linux, seL4, Zephyr and TempleOS, and what is
measured today: see the [yesos-core README](https://github.com/yesos-project/yesos-core#2-design-choices).

Requirements (clang, lld, llvm, QEMU, Rust, Python): see the
[yesos-core README](https://github.com/yesos-project/yesos-core#readme).

```bash
git clone --recurse-submodules https://github.com/yesos-project/yesos-desktop && cd yesos-desktop
make prover-setup        # once (Z3, in core/tools/.venv)
make                     # yesos.elf
make run                 # serial console (Ctrl-A X to quit), own yesos_disk.img, sound via PulseAudio
make run AUDIO=none      # the same without sound (or AUDIO=pipewire, alsa)
make test                # tools/test_desktop.py, test_audio.py and test_eml.py
```

`python3 tools/test_desktop.py` (15 checks): it boots as YesOS Desktop with the
PMP locked and proven, its console has only the core's commands, nothing
answers on the network, a file written from the console survives a reboot,
and fsck is clean.

## Audio (`src/hda.c`, `src/eml_audio.c`, `src/audio_shell.c`)

- **Intel High Definition Audio**, found on the PCI bus through the core's
  `drivers/pci.c`. The controller is driven by polling (no interrupts).
- The codec is discovered, not assumed: its function group, output converter
  and output pin are found by asking the codec, and the amplifier is set to the
  step the codec reports as 0 dB, so the output level is exact.
- One stream, 48 kHz 16-bit stereo, from a power-of-two DMA ring of 16384
  frames (341 ms). The write position follows the DMA position; the ring is
  filled before the DMA starts and kept full of silence while a sound drains,
  so the DMA never replays old samples.
- **Tone generator** (`audio tone`): a 32-bit phase accumulator reading a
  1024-point Q15 sine table without interpolation.
- **EML signal processing**, brought over from `yesos-lab`
  (`eml/eml_audio_physics.c`), integer only:
  - *resampler*: an 8-tap Blackman-windowed sinc in 64 phases, with linear
    interpolation between neighbouring phases; the filter bank is the lab's,
    regenerated bit for bit by `tools/gen_audio_tables.py`. A linear
    interpolator with the same delay is kept beside it as the reference;
  - *TPDF dither* to 16 bits (±1 LSB triangular noise from xorshift64\*,
    one generator per channel), optionally with first-order error feedback
    (noise transfer 1 − 0.75 z⁻¹);
  - *soft limiter*, stereo-linked: above 26000 (about −2 dBFS) peaks follow
    a Padé [1/1] knee and never reach full scale. The lab's optional
    "warmth" (an added second harmonic) is left out.
- **Test signals** (`audio play`): a sine from a Q31 table with linear
  interpolation, made at a source rate of 8000–48000 Hz, quantised to 16 bits
  as a source file would be, resampled to 48 kHz, optionally limited, and
  quantised to 16 bits again (TPDF, shaped TPDF or plain rounding). All
  tables are generated on the host by `tools/gen_audio_tables.py`.
- **Pisano counter:** beside the ring runs the Fibonacci recurrence modulo
  the ring size, which wraps after exactly one Pisano period,
  π(2^14) = 3·2^13 = 24576 steps; the period is verified at init. It is
  telemetry: the samples are placed linearly.

```text
Audio       audio [status]   audio tone <Hz> <ms> [level %]
            audio play <Hz> <ms> [level %] [source rate] [eml|linear] [tpdf|shaped|round] [limit]
            audio bench      (instructions per frame of each stage; QEMU -icount)
```

`python3 tools/test_audio.py` (44 checks) boots with QEMU's `intel-hda` and
`hda-output`, records the output into a WAV file, plays two tones and four
test signals, and measures the recording:

| Command | Duration | Frequency | Level | SINAD |
|---|---|---|---|---|
| `audio tone 1000 1500 50` | 1500.0 ms | 1000.00 Hz | −9.03 dBFS (expected −9.03) | 55.6 dB |
| `audio tone 440 1000 25` | 1000.0 ms | 440.00 Hz | −15.05 dBFS (expected −15.05) | 55.0 dB |
| `audio play 1000 1000 50` (48 kHz, TPDF) | 1000.0 ms | 1000.00 Hz | −9.03 dBFS | 87.3 dB |
| `audio play 1000 1000 50 44100 eml` | 1000.0 ms | 1000.00 Hz | −9.03 dBFS | 82.4 dB |
| `audio play 1000 1000 50 44100 linear` | 1000.0 ms | 1000.00 Hz | −9.05 dBFS | 62.4 dB |
| `audio play 1000 1000 100 48000 limit` | 1000.0 ms | 1000.00 Hz | peak 29384 (the knee formula gives 29383) | — |

There are no underruns; `audio tone` writes identical channels, and the
independent dither of `audio play` keeps them within a few LSB. SINAD is
signal against noise and distortion over the whole band.

- About 55 dB is what a 1024-point table read without interpolation gives.
- 87.3 dB is what TPDF dither to 16 bits allows at this level (theory:
  87.3 dB at −9 dBFS).
- At 1 kHz a 44.1 → 48 kHz conversion keeps 82.4 dB with the EML resampler
  and 62.4 dB with linear interpolation; across the band, see below.

## EML against the plain methods

`python3 tools/test_eml.py` (32 checks; `--markdown` prints these tables).
Everything is measured on YesOS: it plays the signals, QEMU records them
into a WAV file, and the script analyses the recording. QEMU passes the
samples through untouched: a recording was compared bit for bit with the
samples computed independently in Python, over 18 consecutive signals.

**Method.** Sine at 50 % of full scale (−9 dBFS), 500 ms. Source: 44.1 kHz,
TPDF-dithered to 16 bits as a CD track would be. Converted to 48 kHz,
TPDF-dithered to 16 bits again. Analysis: Kaiser window (β = 20) over the
middle 80 % of each signal. SINAD is the signal against noise and distortion
over 0–24 kHz; SFDR is the signal against the largest other line, which for
the resamplers is the image of the tone left by the conversion. The *float64
reference* is computed by the script: the same kind of 16-bit source, a
256-tap Kaiser-windowed sinc (cut-off 21.6 kHz), the same 16-bit output. It
shows what a near-ideal resampler gives. *48 kHz direct* is the signal made at
48 kHz with no conversion: the floor of 16-bit TPDF output (87.3 dB in theory).

| Tone | 48 kHz direct | float64 reference (SINAD / SFDR) | EML 8-tap sinc (SINAD / SFDR, gain) | Linear (SINAD / SFDR, gain) |
|---|---|---|---|---|
| 100 Hz | 87.3 dB | 84.3 / 111.2 dB | 84.1 / 103.9 dB, −0.00 dB | 85.0 / 107.5 dB, −0.00 dB |
| 1 kHz | 87.5 dB | 84.2 / 110.6 dB | 82.4 / 90.0 dB, −0.00 dB | 62.4 / 65.4 dB, −0.01 dB |
| 3 kHz | 87.4 dB | 84.4 / 111.2 dB | 72.1 / 74.7 dB, −0.00 dB | 43.2 / 45.5 dB, −0.13 dB |
| 6 kHz | 87.3 dB | 84.5 / 111.6 dB | 68.6 / 71.7 dB, −0.01 dB | 30.6 / 32.2 dB, −0.53 dB |
| 10 kHz | 87.4 dB | 84.4 / 111.6 dB | 45.6 / 45.7 dB, −0.05 dB | 20.4 / 21.4 dB, −1.46 dB |
| 15 kHz | 87.5 dB | 84.2 / 111.1 dB | 20.9 / 21.0 dB, −0.72 dB | 11.1 / 11.6 dB, −3.12 dB |
| 20 kHz | 87.4 dB | 84.4 / 111.2 dB | 5.5 / 5.6 dB, −2.63 dB | 3.1 / 3.4 dB, −4.59 dB |

Dither, on a quiet tone (1 kHz at 1 % of full scale, −43 dBFS, 48 kHz, 1 s):

| Quantisation to 16 bits | SINAD | SFDR | Noise 20 Hz–4 kHz | Noise 16–24 kHz |
|---|---|---|---|---|
| Rounding, no dither | 57.3 dB | 59.3 dB | −116.8 dBFS | −98.9 dBFS |
| TPDF dither | 53.4 dB | 82.8 dB | −101.3 dBFS | −98.2 dBFS |
| TPDF + noise shaping | 51.5 dB | 78.2 dB | −110.1 dBFS | −93.6 dBFS |

Cost, from `audio bench` under QEMU `-icount shift=0`: `minstret` counts
retired instructions. Each figure is the minimum of 9 runs of a 10 ms block
with interrupts off (in `-icount` the other harts add to the same count, so
the larger runs are discarded). Three separate boots gave the same minima, and
a calibration loop of 2 instructions measures 2.05 per iteration. These are
instruction counts, not cycles or time: QEMU models neither a pipeline nor
caches, and a real core will differ (multiplies, loads).

| Stage | Instructions per stereo frame | At 48 kHz |
|---|---|---|
| EML resampler 44.1 → 48 kHz | 172.8 | 8.30 M instructions/s |
| Linear resampler 44.1 → 48 kHz | 43.6 | 2.09 M instructions/s |
| TPDF dither to 16 bits | 94.1 | 4.51 M instructions/s |
| Rounding to 16 bits | 20.1 | 0.96 M instructions/s |
| Soft limiter (full-scale sine) | 25.0 | 1.20 M instructions/s |
| Sine source (Q31 table, interpolated) | 17.1 | 0.82 M instructions/s |

**What this shows**

- The EML resampler is much better than linear interpolation from 1 kHz up:
  20 dB better at 1 kHz, about 25–38 dB better from 3 to 10 kHz, and it
  keeps the gain flat to 0.05 dB up to 10 kHz, where linear interpolation
  loses 1.5 dB. It costs about four times as many instructions (172.8
  against 43.6 per frame).
- It is still far from a near-ideal resampler above a few kHz. With only
  8 taps the images of the tone stay strong: 45.6 dB at 10 kHz, 20.9 dB at
  15 kHz, 5.5 dB at 20 kHz (an image at 23.9 kHz almost as loud as the
  tone), where the reference keeps 84 dB everywhere. The yesos-lab comment
  "> 91 dB SNR / SFDR" holds for the SFDR only up to about 1 kHz. Closing the
  gap needs a longer filter, which costs more instructions per frame.
- At 100 Hz both resamplers sit at the floor. Linear interpolation even
  measures 0.9 dB better there: its response falls towards high frequencies,
  so it also removes part of the source's dither noise.
- TPDF dither does what it should: on a quiet tone the distortion lines of
  plain rounding (SFDR 59.3 dB) give way to flat noise (SFDR 82.8 dB), at
  3.9 dB of SINAD and 74 more instructions per frame. Noise shaping moves
  noise from 20 Hz–4 kHz (−8.8 dB) to 16–24 kHz (+4.6 dB), as its
  1 − 0.75 z⁻¹ transfer predicts.

The other console commands are the core's (see its README).

## Support YesOS

YesOS is developed in the open and funded by donations. They pay for
development time and for real RISC-V boards, so the missing storage and
network drivers can be written and tested outside the emulator.

[Donate on Liberapay](https://liberapay.com/yesos-project/donate)

## License
MIT. In memory of Terry A. Davis (1969–2018).

YesOS is inspired by his TempleOS (its HolyC became HolyQ here, and the RedSea
filesystem) and is not affiliated with it.
