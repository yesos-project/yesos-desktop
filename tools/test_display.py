#!/usr/bin/env python3
"""
Display test: boots YesOS Desktop with QEMU's bochs-display, draws on the
framebuffer from the console and takes screenshots through QEMU's monitor
(`screendump`, PPM). Each screenshot is compared pixel for pixel with the
picture computed here. Also: mode changes, a refused mode, and a boot
without a display device. Non-zero exit status on failure.
"""
import os
import shutil
import socket
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "core", "tools"))
import yesos_harness as harness  # noqa: E402

failures = []


def check(cond, label):
    print(("  PASS  " if cond else "  FAIL  ") + label)
    if not cond:
        failures.append(label)


class DisplayYesOS(harness.YesOS):
    def __init__(self, disk, host_port, mon_path=None):
        orig = harness.subprocess.Popen
        extra = []
        if mon_path:
            extra = ["-device", "bochs-display",
                     "-chardev", f"socket,id=mon0,path={mon_path},server=on,wait=off",
                     "-mon", "chardev=mon0,mode=readline"]

        def popen(cmd, *a, **kw):
            return orig(list(cmd) + extra, *a, **kw)

        harness.subprocess.Popen = popen
        try:
            super().__init__(disk, host_port)
        finally:
            harness.subprocess.Popen = orig
        self.mon_path = mon_path

    def screendump(self, path):
        """Take a screenshot through the monitor; returns an (h, w, 3) array."""
        if os.path.exists(path):
            os.remove(path)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(self.mon_path)
        s.settimeout(5)
        time.sleep(0.3)
        s.sendall(f"screendump {path}\n".encode())
        end = time.time() + 10
        last = -1
        while time.time() < end:
            time.sleep(0.3)
            if os.path.exists(path):
                size = os.path.getsize(path)
                if size and size == last:
                    break
                last = size
        s.close()
        return read_ppm(path)


def read_ppm(path):
    data = open(path, "rb").read()
    parts, pos = [], 0
    while len(parts) < 4:                     # P6, width, height, maxval
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(data[start:pos])
    pos += 1
    w, h = int(parts[1]), int(parts[2])
    return np.frombuffer(data[pos:pos + w * h * 3], dtype=np.uint8).reshape(h, w, 3)


def rgb(v):
    return [(v >> 16) & 255, (v >> 8) & 255, v & 255]


def test_pattern(w, h):
    """Same picture as display_test_pattern() in src/display.c."""
    img = np.zeros((h, w, 3), dtype=np.uint8)
    bars = [0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000]
    bw = w // 8
    for i, c in enumerate(bars):
        x1 = w if i == 7 else (i + 1) * bw
        img[:h // 2, i * bw:x1] = rgb(c)
    v = (np.arange(w) * 255 // (w - 1)).astype(np.uint8)
    img[h // 2:h * 3 // 4] = np.stack([v, v, v], axis=1)[None, :, :]
    ys, xs = np.mgrid[h * 3 // 4:h, 0:w]
    checker = (((xs >> 4) ^ (ys >> 4)) & 1).astype(bool)
    img[h * 3 // 4:h][~checker] = 255
    img[h * 3 // 4:h][checker] = 0
    for sl in (np.s_[0, :], np.s_[h - 1, :], np.s_[:, 0], np.s_[:, w - 1]):
        img[sl] = rgb(0xFF0000)
    return img


def compare(got, want, label):
    if got.shape != want.shape:
        check(False, f"{label}: screenshot is {got.shape[1]}x{got.shape[0]}, expected {want.shape[1]}x{want.shape[0]}")
        return
    bad = int(np.any(got != want, axis=2).sum())
    check(bad == 0, f"{label}: {want.shape[1]}x{want.shape[0]}, {bad} pixel(s) differ")


def main():
    tmpdir = tempfile.mkdtemp(prefix="yesos_display_")
    disk = os.path.join(tmpdir, "disk.img")
    mon = os.path.join(tmpdir, "mon.sock")
    shot = os.path.join(tmpdir, "shot.ppm")
    shutil.copyfile(harness.SRC_DISK, disk)
    vm = DisplayYesOS(disk, 40000 + os.getpid() % 20000, mon)
    try:
        check(vm.wait_for("YesOS [", 90), "system boots with a bochs-display")
        check("[ OK ] DISPLAY          : bochs-display, 1024x768, 32 bpp" in vm.output(), "display ready at boot, 1024x768")
        m = vm.send("display", 2)
        check("1024x768, 32 bpp, pitch 1024" in vm.since(m), "display status reports the mode")
        compare(vm.screendump(shot), np.zeros((768, 1024, 3), np.uint8), "screen cleared to black at boot")

        m = vm.send("display test", 3)
        check("test pattern drawn (1024x768)" in vm.since(m), "display test runs")
        compare(vm.screendump(shot), test_pattern(1024, 768), "test pattern")

        m = vm.send("display clear 336699", 3)
        compare(vm.screendump(shot), np.full((768, 1024, 3), rgb(0x336699), np.uint8), "clear to 336699")

        for w, h in ((800, 600), (1280, 720)):
            m = vm.send(f"display mode {w} {h}", 3)
            check(f"mode {w}x{h}" in vm.since(m), f"mode {w}x{h} set")
            vm.send("display test", 3)
            compare(vm.screendump(shot), test_pattern(w, h), f"test pattern at {w}x{h}")

        m = vm.send("display mode 9000 9000", 3)
        check("mode 9000x9000 refused" in vm.since(m), "an impossible mode is refused")
        m = vm.send("display", 2)
        check("1024x768" in vm.since(m), "back to 1024x768 after a refused mode")
        check("FATAL" not in vm.output() and "RISC-V EXCEPTION" not in vm.output(), "no kernel faults")
    finally:
        vm.stop()

    vm = DisplayYesOS(disk, 40000 + (os.getpid() + 1) % 20000)
    try:
        check(vm.wait_for("YesOS [", 90), "system boots without a display device")
        check("[ -- ] DISPLAY          : no display" in vm.output(), "boot summary says there is no display")
        m = vm.send("display test", 2)
        check("no usable display" in vm.since(m), "display commands refuse cleanly")
    finally:
        vm.stop()
    shutil.rmtree(tmpdir, ignore_errors=True)

    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED")
        sys.exit(1)
    print("All checks passed")


if __name__ == "__main__":
    main()
