# YesOS Desktop (RISC-V)

The desktop product of YesOS, built on the YesOS core (the `core` git
submodule): Machine mode only, a locked Z3-proven PMP, an integer-only kernel
(no floating point, checked at every build), no Linux, libc or binary blobs.

Today it is the core, its serial console and **HD Audio output** with the
EML signal processing (resampler, dither, soft limiter): no network services
and no optional modules. Features (display, input, windows, apps in PMP
cells) are added here one at a time, each with its own tests, brought over
piece by piece from the old desktop experiments in `yesos-lab`. Next: a test
that measures the EML resampler's quality across the band and its cost in
instructions per sample, against the plain methods.

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
make test                # tools/test_desktop.py and tools/test_audio.py
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
```

`python3 tools/test_audio.py` (44 checks) boots with QEMU's `intel-hda` and
`hda-output`, records the output into a WAV file, plays two tones and four
test signals, and measures the recording:

| Command | Duration | Frequency | Level | SINAD |
|---|---|---|---|---|
| `audio tone 1000 1500 50` | 1500.0 ms | 1000.00 Hz | −9.03 dBFS (expected −9.03) | 55.6 dB |
| `audio tone 440 1000 25` | 1000.0 ms | 440.00 Hz | −15.05 dBFS (expected −15.05) | 55.0 dB |
| `audio play 1000 1000 50` (48 kHz, TPDF) | 1000.0 ms | 1000.00 Hz | −9.03 dBFS | 86.5 dB |
| `audio play 1000 1000 50 44100 eml` | 1000.0 ms | 1000.00 Hz | −9.03 dBFS | 82.2 dB |
| `audio play 1000 1000 50 44100 linear` | 1000.0 ms | 1000.00 Hz | −9.05 dBFS | 62.4 dB |
| `audio play 1000 1000 100 48000 limit` | 1000.0 ms | 1000.00 Hz | peak 29384 (the knee formula gives 29383) | — |

There are no underruns; `audio tone` writes identical channels, and the
independent dither of `audio play` keeps them within a few LSB. SINAD is
signal against noise and distortion over the whole band.

- About 55 dB is what a 1024-point table read without interpolation gives.
- 86.5 dB is what TPDF dither to 16 bits allows at this level (theory: about
  87 dB at −9 dBFS).
- At 1 kHz, a 44.1 → 48 kHz conversion keeps 82.2 dB with the EML resampler
  and 62.4 dB with linear interpolation. That is one frequency only: at
  10 kHz the EML resampler gives about 46 dB and linear interpolation about
  20 dB. The yesos-lab comment claiming ">91 dB SNR/SFDR" for this filter
  does not hold there. The next step measures this properly across the band,
  with the cost in instructions per sample.

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
