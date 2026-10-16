#!/usr/bin/env python3
"""
Makes docs/screenshot.png, the picture at the top of the README: YesOS
Desktop booted in QEMU (display, virtio keyboard and tablet, HD Audio with
no sound device), a text file put on the disk, the console used a little,
the file browser and the viewer opened, the windows placed, then QEMU's
`screendump`. Nothing is drawn by this script: the picture is the screen.
Usage: python3 tools/screenshot.py [output.png]
"""
import os
import shutil
import struct
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "core", "tools"))
import test_input as ti  # noqa: E402
import redsea_injector  # noqa: E402

WELCOME = """Welcome to YesOS Desktop
========================

A bare-metal operating system for 64-bit RISC-V.
Machine mode only, a PMP layout proven with Z3
at every build, and no floating point anywhere.

On this desktop:
\t* the console, in its own window
\t* this file browser and text viewer
\t* WAV files play in the background
\t* F5 copy, F2 rename, Del delete, F7 folder

Keyboard: US or Spanish (input layout es),
with accents: á é í ó ú ñ ü ¿ ¡ €
"""


def write_png(path, img):
    h, w, _ = img.shape
    raw = b"".join(b"\x00" + img[y].tobytes() for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


class ShotYesOS(ti.InputYesOS):
    def __init__(self, disk, port, qmp):
        orig = ti.harness.subprocess.Popen

        def popen(cmd, *a, **kw):
            return orig(list(cmd) + ["-audiodev", "none,id=snd0", "-device", "intel-hda,id=hda0",
                                     "-device", "hda-output,bus=hda0.0,audiodev=snd0"], *a, **kw)

        ti.harness.subprocess.Popen = popen
        try:
            super().__init__(disk, port, qmp)
        finally:
            ti.harness.subprocess.Popen = orig


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(HERE), "docs", "screenshot.png")
    tmp = tempfile.mkdtemp(prefix="yesos_shot_")
    disk = os.path.join(tmp, "disk.img")
    shutil.copyfile(ti.harness.SRC_DISK, disk)
    redsea_injector.inject_bytes(disk, WELCOME.encode(), "WELCOME.TXT")
    vm = ShotYesOS(disk, 40000 + os.getpid() % 20000, os.path.join(tmp, "qmp.sock"))
    try:
        if not vm.wait_for("YesOS [", 90):
            sys.exit("no boot")
        # windows first: the browser, WELCOME.TXT in the viewer, placed
        vm.send("mkdir /music", 1)
        vm.send("files /", 1.5)
        m = vm.send("files list", 1.5)
        names = [l.strip().split(" ")[0] for l in vm.since(m).splitlines() if l.startswith("  ")]
        vm.tap("home")
        for _ in range(names.index("WELCOME.TXT")):
            vm.tap("down")
        vm.tap("ret")
        time.sleep(1)
        vm.send("window move 0 24 24", 1)
        vm.send("window move 1 600 64", 1)
        vm.send("window move 2 404 330", 1)
        vm.send("window front 1", 1)
        vm.send("window front 2", 1)
        # then the console, cleared and used a little
        vm.send("term clear", 1)
        vm.send("pci", 2)
        vm.send("fsck quick", 3)
        vm.send("input layout es", 1)
        vm.pointer(int(760 * 32767 / 1023) + 1, int(220 * 32767 / 767) + 1)
        time.sleep(1)
        img = vm.screendump(os.path.join(tmp, "shot.ppm"))
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        write_png(out, img)
        print(f"{out}: {img.shape[1]}x{img.shape[0]}, {os.path.getsize(out)} bytes")
    finally:
        vm.stop()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
