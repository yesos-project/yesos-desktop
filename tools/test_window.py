#!/usr/bin/env python3
"""
Window test (stage 1): boots YesOS Desktop with a display, a virtio keyboard
and a virtio tablet. The console is in a window on the desktop; the test
drags it by its title bar with tablet events sent through QMP, and moves it
from the console. Every screenshot is compared pixel for pixel with the
picture computed here: background, border, title bar and title, the
console text (replayed from the serial stream by test_input.Terminal), the
pointer on top. Also: a press outside the title bar does not drag, the
window stays whole on the screen, `desktop off`/`on`, and a mode change.
Non-zero exit status on failure.
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
import test_input as ti  # noqa: E402

harness = ti.harness
failures = []
W, H = 1024, 768
BG, BORDER, TITLE_BG, TITLE_FG = 0x1F3B57, 0x8899AA, 0x2B4C7E, 0xFFFFFF
B, T = 2, 20
COLS, ROWS = 100, 40
TITLE = "YesOS Console"


def check(cond, label):
    print(("  PASS  " if cond else "  FAIL  ") + label)
    if not cond:
        failures.append(label)


def compare(got, want, label):
    if got.shape != want.shape:
        check(False, f"{label}: screenshot {got.shape[1]}x{got.shape[0]}, expected {want.shape[1]}x{want.shape[0]}")
        return
    bad = int(np.any(got != want, axis=2).sum())
    check(bad == 0, f"{label}: {bad} pixel(s) differ")


def to_abs(p, size):
    """Tablet value that the driver maps to pixel p (v * (size-1) // 32767 == p)."""
    return -(-p * 32767 // (size - 1))


def desktop(w, h, wx, wy, cols, rows, client, ptr):
    img = np.zeros((h, w, 3), np.uint8)
    img[:] = ti.rgb(BG)
    ww, wh = 2 * B + cols * 8, 2 * B + T + rows * 16
    img[wy:wy + wh, wx:wx + ww] = ti.rgb(BORDER)
    img[wy + B:wy + B + T, wx + B:wx + ww - B] = ti.rgb(TITLE_BG)
    x = wx + B + 6
    for ch in TITLE:
        g = ti.glyph(ord(ch))
        for r in range(16):
            for c in range(8):
                if g[r] & (0x80 >> c):
                    img[wy + B + 2 + r, x + c] = ti.rgb(TITLE_FG)
        x += 8
    img[wy + B + T:wy + B + T + rows * 16, wx + B:wx + B + cols * 8] = client
    ti.draw_arrow(img, *ptr)
    return img


class Session:
    """The VM, the replayed terminal and where the window is."""

    def __init__(self, vm, cols=COLS, rows=ROWS):
        self.vm = vm
        self.cut = None
        self.cols, self.rows = cols, rows

    def clear_terminal(self):
        start = len(self.vm.raw())
        self.vm.send("term clear", 1)
        raw = self.vm.raw()
        self.cut = raw.index(b"term clear\n", start) + len(b"term clear\n")

    def client(self):
        t = ti.Terminal(self.cols * 8, self.rows * 16)
        for b in self.vm.raw()[self.cut:]:
            t.put(b)
        return t.picture()

    def point(self, x, y):
        self.vm.pointer(to_abs(x, W), to_abs(y, H))
        self.ptr = (x, y)

    def drag(self, x0, y0, x1, y1, steps=6):
        self.point(x0, y0)
        self.vm.button("left", True)
        for i in range(1, steps + 1):
            self.point(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
        self.vm.button("left", False)
        time.sleep(0.3)


def status(vm):
    m = vm.send("desktop", 1.5)
    r = re.search(r"window at (-?\d+),(-?\d+), (\d+)x(\d+) pixels, (\d+)x(\d+) characters; (\d+) moves, (\d+) drags",
                  vm.since(m))
    return tuple(int(v) for v in r.groups()) if r else None


def main():
    tmpdir = tempfile.mkdtemp(prefix="yesos_window_")
    disk = os.path.join(tmpdir, "disk.img")
    qmp = os.path.join(tmpdir, "qmp.sock")
    shot = os.path.join(tmpdir, "shot.ppm")
    shutil.copyfile(harness.SRC_DISK, disk)
    vm = ti.InputYesOS(disk, 40000 + os.getpid() % 20000, qmp)
    try:
        check(vm.wait_for("YesOS [", 90), "system boots with a display, keyboard and tablet")
        out = vm.output()
        check("[ OK ] WINDOWS          : desktop, the console in a window at 40,40" in out,
              "boot summary: the console in a window at 40,40")
        check("[ OK ] TERMINAL         : console on the screen, 100x40 characters" in out,
              "boot summary: the console is 100x40 characters")
        ww, wh = 2 * B + COLS * 8, 2 * B + T + ROWS * 16
        check(status(vm) == (40, 40, ww, wh, COLS, ROWS, 0, 0), f"desktop status: window {ww}x{wh} at 40,40")

        s = Session(vm)
        s.point(900, 740)                                    # on the background, out of the way
        s.clear_terminal()
        ti.type_us(vm, "qq typed in the window")
        vm.tap("ret")
        time.sleep(1)
        compare(vm.screendump(shot), desktop(W, H, 40, 40, COLS, ROWS, s.client(), s.ptr),
                "desktop, window frame, title and console")

        # drag by the title bar: +150, +50
        s.drag(300, 50, 450, 100)
        st = status(vm)
        check(st is not None and st[:2] == (190, 90) and st[7] == 1,
              f"dragged by the title bar to 190,90 ({st})")
        s.clear_terminal()                                   # the status line above went into the window
        compare(vm.screendump(shot), desktop(W, H, 190, 90, COLS, ROWS, s.client(), s.ptr),
                "after the drag: window moved whole, background repainted")

        # a press in the text area or on the background does not drag
        s.drag(400, 400, 600, 500)
        s.drag(100, 700, 50, 600)
        st = status(vm)
        check(st is not None and st[:2] == (190, 90) and st[7] == 1,
              f"presses outside the title bar move nothing ({st})")

        # dragged far right and down: stops with the window whole on the screen
        s.drag(260, 100, 1023, 767)
        st = status(vm)
        check(st is not None and st[:2] == (W - ww, H - wh), f"clamped at {W - ww},{H - wh} ({st})")

        # moved from the console, then typed into at the new place
        m = vm.send("desktop move 0 0", 1.5)
        check("window at 0,0" in vm.since(m), "desktop move 0 0")
        s.clear_terminal()
        ti.type_us(vm, "qq after the move")
        time.sleep(1)
        compare(vm.screendump(shot), desktop(W, H, 0, 0, COLS, ROWS, s.client(), s.ptr),
                "moved to 0,0: frame, text typed there, pointer")
        vm.tap("ret")
        time.sleep(1)

        # a small drag in steps of a few pixels (overlapping copies)
        s.drag(100, 10, 103, 13, steps=3)
        st = status(vm)
        check(st is not None and st[:2] == (3, 3), f"small overlapping moves: at 3,3 ({st})")
        s.clear_terminal()
        compare(vm.screendump(shot), desktop(W, H, 3, 3, COLS, ROWS, s.client(), s.ptr),
                "after small overlapping moves")

        # off: the console takes the whole screen; on: the window again
        m = vm.send("desktop off", 1.5)
        m = vm.send("term", 1)
        check("on, 128x48 characters at 0,0" in vm.since(m), "desktop off: the console takes the whole screen")
        vm.send("desktop on", 1.5)
        st = status(vm)
        check(st is not None and st[:2] == (3, 3), f"desktop on: the window where it was ({st})")

        # a smaller mode: the console shrinks to fit, the window stays whole
        vm.send("display mode 800 600", 2)
        st = status(vm)
        cols, rows = (800 - 2 * B - 32) // 8, (600 - 2 * B - T - 32) // 16
        check(st is not None and st[4:6] == (cols, rows) and st[0] + st[2] <= 800 and st[1] + st[3] <= 600,
              f"800x600: console {cols}x{rows}, window on the screen ({st})")
        check("FATAL" not in vm.output() and "RISC-V EXCEPTION" not in vm.output(), "no kernel faults")
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
