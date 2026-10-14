# YesOS Desktop (RISC-V)

The desktop product of YesOS, built on the YesOS core (the `core` git
submodule): Machine mode only, a locked Z3-proven PMP, no Linux, libc or binary
blobs.

For now it is **the core and its serial console, nothing else**: no network
services and no optional modules (259 KB of code). Features (display, input,
windows, audio, apps in PMP cells) will be added here one at a time, each with
its own tests, to strengthen and organise the code. `yesos-lab` holds the old
desktop experiments that can be brought over piece by piece.

```bash
git clone --recurse-submodules <url>/yesos-desktop   # or: git submodule update --init
make prover-setup        # once (Z3, in core/tools/.venv)
make                     # yesos.elf
make run                 # serial console in the terminal (Ctrl-A X to quit), own yesos_disk.img
make test                # tools/test_desktop.py
```

`python3 tools/test_desktop.py` (15 checks): it boots as YesOS Desktop with the
PMP locked and proven, its console has only the core's commands, nothing
answers on the network, a file written from the console survives a reboot,
and fsck is clean.

The console commands are the core's (see its README).

## Support YesOS

YesOS is developed in the open and funded by donations. They pay for
development time and for real RISC-V boards, so the missing storage and
network drivers can be written and tested outside the emulator.

[Donate on Liberapay](https://liberapay.com/yesos-project/donate)

## License
MIT. In memory of Terry A. Davis (1969–2018).

YesOS is inspired by his TempleOS (its HolyC became HolyQ here, and the RedSea
filesystem) and is not affiliated with it.
