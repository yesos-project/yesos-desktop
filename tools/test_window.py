#!/usr/bin/env python3
"""
Window test: boots YesOS Desktop with a display, a virtio keyboard and a
virtio tablet. Stage 1: the console is in a window on the desktop; the test
drags it by its title bar with tablet events sent through QMP, and moves it
from the console. Stage 2 (a second boot): several windows overlap; presses
bring them to the front and give the focus, the close box closes, text
typed into the console while other windows cover it shows only where the
console is visible, at most 8 windows. Every screenshot is compared pixel for pixel with the
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
TITLE_OFF_BG, TITLE_OFF_FG, CLOSE_RGB, PAPER, CLOSE = 0x5B6878, 0xC8CCD0, 0xC04040, 0xF4F1E8, 14


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


class Win:
    def __init__(self, wid, x, y, cols, rows, title, closable=True):
        self.id, self.x, self.y, self.cols, self.rows, self.title = wid, x, y, cols, rows, title
        self.closable = closable
        self.w, self.h = 2 * B + cols * 8, 2 * B + T + rows * 16
        self.content = np.zeros((rows * 16, cols * 8, 3), np.uint8)
        self.content[:] = ti.rgb(PAPER)

    def text(self, col, row, color, text):
        x, y = col * 8, row * 16
        for ch in text:
            g = ti.glyph(ord(ch))
            for r in range(16):
                for c in range(8):
                    py, px = y + r, x + c
                    if g[r] & (0x80 >> c) and 0 <= py < self.content.shape[0] and 0 <= px < self.content.shape[1]:
                        self.content[py, px] = ti.rgb(color)
            x += 8


def put_glyph(img, x, y, cp, color):
    g = ti.glyph(cp)
    for r in range(16):
        for c in range(8):
            if g[r] & (0x80 >> c):
                img[y + r, x + c] = ti.rgb(color)


def compose(w, h, wins, focus, ptr):
    """Same as compose() in src/desktop.c, for the whole screen; wins bottom to top."""
    img = np.zeros((h, w, 3), np.uint8)
    img[:] = ti.rgb(BG)
    for win in wins:
        X, Y, W_, H_ = win.x, win.y, win.w, win.h
        on = win.id == focus
        img[Y:Y + H_, X:X + W_] = ti.rgb(BORDER)
        img[Y + B:Y + B + T, X + B:X + W_ - B] = ti.rgb(TITLE_BG if on else TITLE_OFF_BG)
        room = W_ - 2 * B - 6 - (CLOSE + 6 if win.closable else 6)
        for i, ch in enumerate(win.title[:room // 8]):
            put_glyph(img, X + B + 6 + 8 * i, Y + B + 2, ord(ch), TITLE_FG if on else TITLE_OFF_FG)
        if win.closable:
            bx, by = X + W_ - B - CLOSE - 3, Y + B + 3
            img[by:by + CLOSE, bx:bx + CLOSE] = ti.rgb(CLOSE_RGB)
            for i in range(3, 11):
                img[by + i, bx + i:bx + i + 2] = 255
                img[by + i, bx + 12 - i:bx + 14 - i] = 255
        img[Y + B + T:Y + B + T + win.rows * 16, X + B:X + B + win.cols * 8] = win.content
    ti.draw_arrow(img, *ptr)
    return img


def close_box(win):
    return win.x + win.w - B - CLOSE - 3 + 7, win.y + B + 3 + 7


def listing(vm):
    m = vm.send("window list", 1.5)
    return re.findall(r"  (\d+): \"(.*?)\" at (\d+),(\d+), (\d+)x(\d+) pixels, (\d+)x(\d+) characters(, focused)?(, top)?",
                      vm.since(m))


def stage2(tmpdir):
    disk = os.path.join(tmpdir, "disk2.img")
    qmp = os.path.join(tmpdir, "qmp2.sock")
    shot = os.path.join(tmpdir, "shot2.ppm")
    shutil.copyfile(harness.SRC_DISK, disk)
    vm = ti.InputYesOS(disk, 40000 + (os.getpid() + 7) % 20000, qmp)
    try:
        check(vm.wait_for("YesOS [", 90), "stage 2: second boot")
        s = Session(vm)
        s.point(1000, 750)
        con = Win(0, 40, 40, COLS, ROWS, TITLE, closable=False)
        s.clear_terminal()

        def screen(wins, focus, label):
            con.content = s.client()
            compare(vm.screendump(shot), compose(W, H, wins, focus, s.ptr), label)

        m = vm.send("window new 40 10 Notes", 1.5)
        check("[WINDOW] 1 \"Notes\" at 120,80, 40x10 characters" in vm.since(m), "window new: id 1 at 120,80")
        w1 = Win(1, 120, 80, 40, 10, "Notes")
        vm.send("window text 1 1 1 802020 Hola, ventana: ñ á €", 1)
        w1.text(1, 1, 0x802020, "Hola, ventana: ñ á €")
        long_title = "Second window with a title too long for it"
        m = vm.send(f"window new 30 8 {long_title}", 1.5)
        check("[WINDOW] 2 " in vm.since(m), "window new: id 2")
        w2 = Win(2, 152, 112, 30, 8, long_title)
        vm.send("window text 2 0 7 205020 bottom line, clipped at the right edge of the window", 1)
        w2.text(0, 7, 0x205020, "bottom line, clipped at the right edge of the window")
        screen([con, w1, w2], 2, "three windows: the newest on top and focused, long title cut")

        lst = listing(vm)
        check([(e[0], e[8], e[9]) for e in lst] == [("2", ", focused", ", top"), ("1", "", ""), ("0", "", "")],
              f"window list: 2 (focused, top), 1, 0 ({[e[0] for e in lst]})")

        s.point(60, 650)                                    # the console, where nothing covers it
        vm.button("left", True)
        vm.button("left", False)
        time.sleep(0.3)
        screen([w1, w2, con], 0, "press on the console: to the front, focused, covers the others")

        vm.send("window front 1", 1)
        screen([w2, con, w1], 1, "window front 1")

        vm.send("window front 2", 1)
        s.drag(200, 122, 548, 410)                          # title bar of 2 at (152,112): +348, +288
        w2.x, w2.y = 500, 400
        screen([con, w1, w2], 2, "window 2 dragged by its title bar over the console")

        # window 1 dragged over the console's cursor line (a drag prints nothing)
        t = ti.Terminal(COLS * 8, ROWS * 16)
        for b in vm.raw()[s.cut:]:
            t.put(b)
        line_y = 40 + B + T + t.row * 16                    # the line being typed on, on the screen
        ny = line_y - B - T - 8                             # its text area starts half a line above it
        s.drag(w1.x + 60, w1.y + 10, 60 + 60, ny + 10)
        w1.x, w1.y = 60, ny
        ti.type_us(vm, "qq the console is behind two windows")
        time.sleep(1)
        screen([con, w2, w1], 1, "text typed into the covered console shows only where it is visible")

        # the close box of window 1: pressed, 1 comes to the front and closes
        s.point(*close_box(w1))
        vm.button("left", True)
        vm.button("left", False)
        time.sleep(0.3)
        screen([con, w2], 2, "close box: window 1 closed, the console's line under it shown again")
        vm.tap("ret")
        time.sleep(1)

        m = vm.send("window close 0", 1)
        check("cannot be closed" in vm.since(m), "the console's window cannot be closed")
        m = vm.send("window close 2", 1)
        check("[WINDOW] 2 closed" in vm.since(m), "window close 2")
        screen([con], 0, "only the console left, focused again")

        for i in range(7):
            vm.send(f"window new 10 3 w{i}", 1)
        m = vm.send("window new 10 3 one too many", 1.5)
        check("cannot open another window (at most 8" in vm.since(m), "at most 8 windows")
        lst = listing(vm)
        check(len(lst) == 8, f"8 windows listed ({len(lst)})")
        for e in lst:
            if e[0] != "0":
                vm.send(f"window close {e[0]}", 0.5)
        time.sleep(1)
        screen([con], 0, "all closed again: only the console")
        st = status(vm)
        check(st is not None, "desktop status after closing them")
        check("FATAL" not in vm.output() and "RISC-V EXCEPTION" not in vm.output(), "stage 2: no kernel faults")
    finally:
        vm.stop()


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
    stage2(tmpdir)
    shutil.rmtree(tmpdir, ignore_errors=True)

    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED")
        sys.exit(1)
    print("All checks passed")


if __name__ == "__main__":
    main()
