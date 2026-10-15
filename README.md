# YesOS Desktop (RISC-V)

The desktop product of YesOS, built on the YesOS core (the `core` git
submodule): Machine mode only, a locked Z3-proven PMP, an integer-only kernel
(no floating point, checked at every build), no Linux, libc or binary blobs.

Today it is the core, its serial console and **HD Audio output**: no network
services and no optional modules. Features (display, input, windows, apps in
PMP cells) are added here one at a time, each with its own tests, brought over
piece by piece from the old desktop experiments in `yesos-lab`. Next: the EML
signal processing for audio (resampling, dithering, limiter), with tests that
measure its quality and cost against the plain methods.

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

## Audio (`src/hda.c`, `src/audio_shell.c`)

- **Intel High Definition Audio**, found on the PCI bus through the core's
  `drivers/pci.c`. The controller is driven by polling (no interrupts).
- The codec is discovered, not assumed: its function group, output converter
  and output pin are found by asking the codec, and the amplifier is set to the
  step the codec reports as 0 dB, so the output level is exact.
- One stream, 48 kHz 16-bit stereo, from a power-of-two DMA ring of 16384
  frames (341 ms). The write position follows the DMA position; the ring is
  filled before the DMA starts and kept full of silence while a sound drains,
  so the DMA never replays old samples.
- **Tone generator:** a 32-bit phase accumulator reading a 1024-point Q15 sine
  table (generated on the host by `tools/gen_sine_q15.py`). Integer only.
- **Pisano counter:** beside the ring runs the Fibonacci recurrence modulo
  the ring size, which wraps after exactly one Pisano period,
  π(2^14) = 3·2^13 = 24576 steps; the period is verified at init. It is
  telemetry: the samples are placed linearly.

```text
Audio       audio [status]   audio tone <Hz> <ms> [level %]
```

`python3 tools/test_audio.py` (20 checks) boots with QEMU's `intel-hda` and
`hda-output`, records the output into a WAV file, plays two tones and
measures the recording:

| Tone | Duration | Frequency | Level | SINAD |
|---|---|---|---|---|
| 1000 Hz, 50 % | 1500.0 ms (asked 1500) | 1000.00 Hz | −9.03 dBFS (expected −9.03) | 55.6 dB |
| 440 Hz, 25 % | 1000.0 ms (asked 1000) | 440.00 Hz | −15.05 dBFS (expected −15.05) | 55.0 dB |

Both channels are identical and there are no underruns. The SINAD (signal
against noise and distortion) of about 55 dB is what a 1024-point table read
without interpolation gives; it is the baseline the EML signal processing
will be measured against.

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
