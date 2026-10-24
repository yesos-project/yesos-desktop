#!/usr/bin/env python3
"""
Input test: boots YesOS Desktop with a bochs-display, a virtio keyboard and
a virtio tablet, and sends input events through QEMU's QMP socket
(`input-send-event`), as the window would.
- Keyboard: what is typed reaches the console (the shell's "Unknown command"
  line shows the exact text) with the US and the Spanish layout: shift, caps
  lock, AltGr, dead keys for accents, keypad, backspace over UTF-8, Ctrl
  ignored.
- Tablet: pointer position scaled from the device's axis range, clicks of
  each button, wheel.
- Screen (`screendump`, compared pixel for pixel with the picture computed
  here): the pointer arrow over a plain colour and the colour put back where
  it was; the console drawn by the on-screen terminal, replayed here from
  the bytes the serial port received (same stream), with scrolling.
Non-zero exit status on failure.
"""
import json
import os
import re
import shutil
import socket
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "core", "tools"))
sys.path.insert(0, HERE)
import yesos_harness as harness  # noqa: E402
import gen_font  # noqa: E402

failures = []
W, H = 1024, 768
FONT = gen_font.load()


def check(cond, label):
    print(("  PASS  " if cond else "  FAIL  ") + label)
    if not cond:
        failures.append(label)


class InputYesOS(harness.YesOS):
    def __init__(self, disk, host_port, qmp_path):
        orig = harness.subprocess.Popen
        extra = ["-device", "bochs-display", "-device", "virtio-keyboard-pci", "-device", "virtio-tablet-pci",
                 "-qmp", f"unix:{qmp_path},server=on,wait=off"]

        def popen(cmd, *a, **kw):
            return orig(list(cmd) + extra, *a, **kw)

        harness.subprocess.Popen = popen
        try:
            super().__init__(disk, host_port)
        finally:
            harness.subprocess.Popen = orig
        self.qmp_path = qmp_path
        self.q = None

    def raw(self):
        with self.lock:
            return bytes(self.buf)

    # ---- QMP ----
    def qmp(self, cmd, **args):
        if self.q is None:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(self.qmp_path)
            self.q = s.makefile("rw")
            self.q.readline()                                   # greeting
            self.qmp("qmp_capabilities")
        self.q.write(json.dumps({"execute": cmd, "arguments": args}) + "\n")
        self.q.flush()
        while True:
            r = json.loads(self.q.readline())
            if "return" in r or "error" in r:
                return r

    def events(self, *evs):
        r = self.qmp("input-send-event", events=list(evs))
        time.sleep(0.03)
        return r

    def key(self, qcode, down):
        return self.events({"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": qcode}}})

    def tap(self, qcode, *mods):
        for m in mods:
            self.key(m, True)
        self.key(qcode, True)
        self.key(qcode, False)
        for m in reversed(mods):
            self.key(m, False)

    def pointer(self, x, y):
        self.events({"type": "abs", "data": {"axis": "x", "value": x}},
                    {"type": "abs", "data": {"axis": "y", "value": y}})

    def button(self, name, down):
        self.events({"type": "btn", "data": {"down": down, "button": name}})

    def screendump(self, path):
        if os.path.exists(path):
            os.remove(path)
        time.sleep(0.5)
        self.qmp("screendump", filename=path)
        return read_ppm(path)


def read_ppm(path):
    data = open(path, "rb").read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(data[start:pos])
    pos += 1
    w, h = int(parts[1]), int(parts[2])
    return np.frombuffer(data[pos:pos + w * h * 3], dtype=np.uint8).reshape(h, w, 3).copy()


def rgb(v):
    return [(v >> 16) & 255, (v >> 8) & 255, v & 255]


def compare(got, want, label):
    if got.shape != want.shape:
        check(False, f"{label}: screenshot {got.shape[1]}x{got.shape[0]}, expected {want.shape[1]}x{want.shape[0]}")
        return
    bad = int(np.any(got != want, axis=2).sum())
    check(bad == 0, f"{label}: {bad} pixel(s) differ")


# Same arrow as src/display.c: 'X' black, '.' white, tip at (x, y).
ARROW = [
    "X          ", "XX         ", "X.X        ", "X..X       ", "X...X      ", "X....X     ",
    "X.....X    ", "X......X   ", "X.......X  ", "X........X ", "X.....XXXXX", "X..X..X    ",
    "X.X X..X   ", "XX  X..X   ", "X    X..X  ", "     X..X  ", "      X..X ", "      X..X ",
    "       XX  ",
]


def draw_arrow(img, x, y):
    for r, row in enumerate(ARROW):
        for c, ch in enumerate(row):
            px, py = x + c, y + r
            if ch != " " and 0 <= px < img.shape[1] and 0 <= py < img.shape[0]:
                img[py, px] = 0 if ch == "X" else 255


def glyph(cp):
    if cp == 0x20:
        return FONT[0x20]
    g = FONT.get(cp)
    return g if g is not None and any(g) else FONT[ord("?")]


class Terminal:
    """Replays src/terminal.c on a picture: the same byte stream, the same picture."""
    FG, BG = 0xE0E0E0, 0x000000
    PALETTE = [0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xE0E0E0]

    def __init__(self, w, h):
        self.img = np.zeros((h, w, 3), np.uint8)
        self.cols, self.rows = w // 8, h // 16
        self.col = self.row = 0
        self.wrap = False
        self.fg = self.FG
        self.esc, self.params, self.left, self.cp = 0, [], 0, 0
        self.scrolls = 0

    def cell(self, cp):
        g = glyph(cp)
        x0, y0 = self.col * 8, self.row * 16
        for r in range(16):
            for c in range(8):
                self.img[y0 + r, x0 + c] = rgb(self.fg if g[r] & (0x80 >> c) else self.BG)

    def resize(self, cols, rows):
        """terminal_resize() in src/terminal.c: the text rows that end with the
        cursor's are kept, cut at the new width; the cursor stays in range."""
        up = self.row - rows + 1 if self.row >= rows else 0
        img = np.zeros((rows * 16, cols * 8, 3), np.uint8)
        img[:] = rgb(self.BG)
        old = self.img[up * 16:up * 16 + rows * 16, :cols * 8]
        img[:old.shape[0], :old.shape[1]] = old
        self.img, self.cols, self.rows = img, cols, rows
        self.row -= up
        if self.col >= cols:
            self.col, self.wrap = cols - 1, False

    def newline(self):
        self.col, self.wrap = 0, False
        if self.row + 1 < self.rows:
            self.row += 1
        else:
            n = self.rows * 16
            self.img[0:n - 16] = self.img[16:n].copy()
            self.img[n - 16:n] = rgb(self.BG)
            self.scrolls += 1

    def glyph(self, cp):
        if self.wrap:
            self.newline()
        self.cell(cp)
        if self.col + 1 < self.cols:
            self.col += 1
        else:
            self.wrap = True

    def put(self, b):
        if self.esc == 1:
            self.esc = 2 if b == ord("[") else 0
            self.params = []
            return
        if self.esc == 2:
            if 0x30 <= b <= 0x39:
                if not self.params:
                    self.params = [0]
                self.params[-1] = self.params[-1] * 10 + b - 0x30
            elif b == ord(";"):
                self.params = (self.params or [0]) + [0]
            elif 0x40 <= b <= 0x7E:
                if b == ord("m"):
                    for p in self.params or [0]:
                        if p in (0, 39):
                            self.fg = self.FG
                        elif 30 <= p <= 37:
                            self.fg = self.PALETTE[p - 30]
                elif b == ord("J") and self.params[:1] == [2]:
                    self.img[:] = rgb(self.BG)
                    self.col = self.row = 0
                    self.wrap = False
                elif b == ord("H"):
                    self.col = self.row = 0
                    self.wrap = False
                self.esc = 0
            return
        if self.left:
            if b & 0xC0 == 0x80:
                self.cp = (self.cp << 6) | (b & 0x3F)
                self.left -= 1
                if self.left == 0:
                    self.glyph(self.cp)
                return
            self.left = 0
            self.glyph(ord("?"))
        if b >= 0x80:
            if 0xC0 <= b < 0xF8:
                self.left = 3 if b >= 0xF0 else 2 if b >= 0xE0 else 1
                self.cp = b & (0x3F >> self.left)
            else:
                self.glyph(ord("?"))
            return
        if b == 0x1B:
            self.esc = 1
        elif b == 0x0A:
            self.newline()
        elif b == 0x0D:
            self.col, self.wrap = 0, False
        elif b == 0x08:
            if self.wrap:
                self.wrap = False
            elif self.col > 0:
                self.col -= 1
            elif self.row > 0:
                self.row, self.col = self.row - 1, self.cols - 1
        elif b == 0x09:
            while True:
                self.glyph(0x20)
                if self.col % 8 == 0 or self.wrap:
                    break
        elif 32 <= b < 127:
            self.glyph(b)

    def picture(self):
        img = self.img.copy()
        x0, y0 = self.col * 8, self.row * 16 + 14
        img[y0:y0 + 2, x0:x0 + 8] = rgb(self.fg)                 # cursor
        return img


# US layout: character -> (qcode, shift)
US = {}
for ch in "abcdefghijklmnopqrstuvwxyz":
    US[ch], US[ch.upper()] = (ch, False), (ch, True)
for plain, shifted, q in zip("1234567890", "!@#$%^&*()", "1234567890"):
    US[plain], US[shifted] = (q, False), (q, True)
for plain, shifted, q in (("-", "_", "minus"), ("=", "+", "equal"), ("[", "{", "bracket_left"),
                          ("]", "}", "bracket_right"), (";", ":", "semicolon"), ("'", '"', "apostrophe"),
                          ("`", "~", "grave_accent"), ("\\", "|", "backslash"), (",", "<", "comma"),
                          (".", ">", "dot"), ("/", "?", "slash")):
    US[plain], US[shifted] = (q, False), (q, True)
US[" "] = ("spc", False)


def type_us(vm, text):
    for ch in text:
        q, shift = US[ch]
        vm.tap(q, *(["shift"] if shift else []))


def unknown_line(vm, mark):
    """The text the shell received, from its "Unknown command" line."""
    vm.wait_for("YesOS [", 10, mark)
    m = re.search(r"Unknown command: '(.*)'\. Type", vm.since(mark))
    return m.group(1) if m else None


def enter_and_read(vm):
    mark = len(vm.output())
    vm.tap("ret")
    return unknown_line(vm, mark)


def status(vm):
    m = vm.send("input", 1.5)
    return vm.since(m)


def main():
    tmpdir = tempfile.mkdtemp(prefix="yesos_input_")
    disk = os.path.join(tmpdir, "disk.img")
    qmp = os.path.join(tmpdir, "qmp.sock")
    shot = os.path.join(tmpdir, "shot.ppm")
    shutil.copyfile(harness.SRC_DISK, disk)
    vm = InputYesOS(disk, 40000 + os.getpid() % 20000, qmp)
    try:
        check(vm.wait_for("YesOS [", 90), "system boots with a display, a virtio keyboard and a virtio tablet")
        out = vm.output()
        check("[ OK ] INPUT            : virtio-input, 1 keyboard(s), 1 pointer(s), layout us" in out,
              "boot summary: one keyboard and one pointer")
        check("[ OK ] TERMINAL         : console on the screen, 100x40 characters" in out,
              "boot summary: console on the screen, 100x40 (in the desktop's window)")
        res = status(vm)
        check("QEMU Virtio Keyboard, keyboard" in res and "QEMU Virtio Tablet, tablet" in res,
              "input status names both devices")
        check("x 0..32767, y 0..32767" in res, "tablet axis range read from the device: 0..32767")

        # ---- keyboard, US layout ----
        text = "qq Hello, World! @#$%^&*_+-=[]{};:'\"\\|,.<>/?`~ 0123456789"
        type_us(vm, text)
        got = enter_and_read(vm)
        check(got == text, f"US layout: typed text arrives exactly ({got!r})")

        type_us(vm, "qq abx")
        vm.tap("backspace")
        type_us(vm, "c")
        vm.tap("caps_lock")
        type_us(vm, "def")
        vm.tap("g", "shift")                                     # shift with caps lock: lower case
        vm.tap("caps_lock")
        vm.tap("h", "ctrl")                                      # Ctrl+H: nothing
        vm.tap("h", "alt")                                       # Alt+H: nothing
        for q in ("kp_1", "kp_2", "kp_add", "kp_3", "kp_multiply", "kp_4", "kp_subtract", "kp_divide", "kp_decimal"):
            vm.tap(q)
        mark = len(vm.output())
        vm.tap("kp_enter")
        got = unknown_line(vm, mark)
        check(got == "qq abcDEFg12+3*4-/.", f"backspace, caps lock, shift, Ctrl/Alt ignored, keypad ({got!r})")

        # ---- keyboard, Spanish layout ----
        m = vm.send("input layout es", 1)
        check("keyboard layout es (Spanish)" in vm.since(m), "input layout es")
        seq = [("q",), ("q",), ("spc",),
               ("semicolon",), ("semicolon", "shift"),                           # ñ Ñ
               ("equal",), ("equal", "shift"), ("minus", "shift"),               # ¡ ¿ ?
               ("apostrophe",), ("a",), ("apostrophe",), ("e", "shift"),         # á É
               ("apostrophe", "shift"), ("u",), ("apostrophe", "shift"), ("u", "shift"),   # ü Ü
               ("bracket_left",), ("o",), ("bracket_left", "shift"), ("i",),     # ò î
               ("apostrophe",), ("spc",), ("apostrophe",), ("n",),               # ´ then ´n
               ("backslash",), ("backslash", "shift"), ("grave_accent",), ("grave_accent", "shift"),   # ç Ç º ª
               ("2", "shift"), ("3", "shift"), ("7", "shift"), ("0", "shift"),   # " · / =
               ("2", "alt_r"), ("3", "alt_r"), ("1", "alt_r"), ("e", "alt_r"),   # @ # | €
               ("bracket_left", "alt_r"), ("bracket_right", "alt_r"),            # [ ]
               ("apostrophe", "alt_r"), ("backslash", "alt_r"),                  # { }
               ("bracket_right",), ("bracket_right", "shift"), ("less",), ("less", "shift"),   # + * < >
               ("slash",), ("slash", "shift"), ("comma", "shift"), ("dot", "shift"),          # - _ ; :
               ("caps_lock",), ("semicolon",), ("a",), ("caps_lock",)]           # Ñ A
        for k in seq:
            vm.tap(k[0], *k[1:])
        got = enter_and_read(vm)
        want = "qq ñÑ¡¿?áÉüÜòî´´nçÇºª\"·/=@#|€[]{}+*<>-_;:ÑA"
        check(got == want, f"Spanish layout with dead keys and AltGr ({got!r})")

        for k in (("q",), ("q",), ("spc",), ("semicolon",), ("apostrophe",), ("a",)):
            vm.tap(k[0], *k[1:])
        vm.tap("backspace")                                      # removes the whole 'á' (2 bytes)
        vm.tap("backspace")                                      # and the whole 'ñ'
        vm.tap("n")
        got = enter_and_read(vm)
        check(got == "qq n", f"backspace removes whole UTF-8 characters ({got!r})")
        m = vm.send("input layout us", 1)
        check("keyboard layout us, saved in /input.cfg" in vm.since(m), "input layout us: saved on the disk")

        # ---- editing keys: the shell's line editor, from the virtio keyboard ----
        type_us(vm, "qq abc")
        vm.tap("left")
        vm.tap("left")
        type_us(vm, "X")
        vm.tap("home")
        type_us(vm, "Y")
        vm.tap("end")
        type_us(vm, "Z")
        got = enter_and_read(vm)
        check(got == "Yqq aXbcZ", f"left, Home, End: {got!r}")
        type_us(vm, "qq abcd")
        vm.tap("left")
        vm.tap("left")
        vm.tap("delete")
        got = enter_and_read(vm)
        check(got == "qq abd", f"Delete: {got!r}")
        vm.tap("up")
        vm.tap("up")
        vm.tap("down")
        got = enter_and_read(vm)
        check(got == "qq abd", f"up, up, down: the last line again: {got!r}")

        res = status(vm)
        k = re.search(r"Keys    : (\d+) presses, (\d+) characters typed", res)
        check(k is not None and int(k.group(2)) > 100, f"key counters: {k.group(0) if k else '?'}")

        # ---- tablet ----
        for vx, vy in ((16384, 8192), (0, 0), (32767, 32767), (1000, 31000)):
            vm.pointer(vx, vy)
            res = status(vm)
            ex, ey = vx * (W - 1) // 32767, vy * (H - 1) // 32767
            check(f"Pointer : {ex},{ey}," in res, f"tablet {vx},{vy} -> pointer at pixel {ex},{ey}")
        vm.pointer(8000, 20000)
        px, py = 8000 * (W - 1) // 32767, 20000 * (H - 1) // 32767
        for name in ("left", "left", "right", "middle"):
            vm.button(name, True)
            time.sleep(0.1)
            vm.button(name, False)
        res = status(vm)
        check(f"buttons 0, clicks left 2 right 1 middle 1, last click at {px},{py}" in res,
              "clicks: left 2, right 1, middle 1, at the pointer, all released")
        for name in ("wheel-up", "wheel-up", "wheel-up", "wheel-down"):
            vm.button(name, True)
            vm.button(name, False)
        res = status(vm)
        check(", wheel 2" in res, f"wheel: 3 up, 1 down -> {re.search(r'wheel -?[0-9]+', res).group(0) if re.search(r'wheel -?[0-9]+', res) else '?'}")

        # ---- the pointer on the screen ----
        vm.send("term off", 1)
        vm.send("display clear 336699", 1.5)
        for (vx, vy), label in (((16384, 16384), "pointer drawn over the picture"),
                                ((4000, 6000), "pointer moved: the old place repainted"),
                                ((32767, 32767), "pointer at the bottom-right corner, clipped")):
            vm.pointer(vx, vy)
            want = np.full((H, W, 3), rgb(0x336699), np.uint8)
            draw_arrow(want, vx * (W - 1) // 32767, vy * (H - 1) // 32767)
            compare(vm.screendump(shot), want, label)
        vm.pointer(8000, 4000)
        vm.send("display print 0 0 ffcc00 " + "#" * 40, 1.5)     # text drawn across the pointer
        want = np.full((H, W, 3), rgb(0x336699), np.uint8)
        g = FONT[ord("#")]
        for i in range(40):
            for r in range(16):
                for c in range(8):
                    if g[r] & (0x80 >> c):
                        want[r, i * 8 + c] = rgb(0xFFCC00)
        draw_arrow(want, 8000 * (W - 1) // 32767, 4000 * (H - 1) // 32767)
        compare(vm.screendump(shot), want, "text drawn under the pointer: the pointer stays on top")

        # ---- the console on the screen (whole screen; the window: test_window.py) ----
        vm.send("desktop off", 1.5)
        vm.pointer(32767, 0)
        start = len(vm.raw())
        vm.send("term on", 1.5)
        raw = vm.raw()
        cut = raw.index(b"term on\n", start) + len(b"term on\n")        # the screen is cleared after this
        type_us(vm, "qq the console, typed in the window")
        vm.tap("ret")
        time.sleep(1)
        vm.send("help", 3)
        vm.send("help", 3)                                          # long enough to scroll
        type_us(vm, "qq last line")
        time.sleep(1)
        t = Terminal(W, H)
        for b in vm.raw()[cut:]:
            t.put(b)
        want = t.picture()
        draw_arrow(want, W - 1, 0)
        check(t.scrolls > 0, f"the output scrolled the terminal ({t.scrolls} lines)")
        compare(vm.screendump(shot), want, "terminal: typed text, command output and scrolling, pixel for pixel")
        vm.tap("ret")
        time.sleep(1)
        m = vm.send("term", 1)
        st = re.search(r"Terminal: on, 128x48 characters at 0,0, cursor at (\d+),(\d+); \d+ characters drawn, (\d+) scrolls, (\d+) dropped",
                       vm.since(m))
        check(st is not None and st.group(4) == "0", f"term status: {st.group(0) if st else vm.since(m)!r}")

        m = vm.send("display mode 800 600", 2)
        check("mode 800x600" in vm.since(m), "display mode 800x600 with the terminal on")
        m = vm.send("term", 1)
        check("on, 100x37 characters" in vm.since(m), "terminal follows the new mode: 100x37")
        check("FATAL" not in vm.output() and "RISC-V EXCEPTION" not in vm.output(), "no kernel faults")
        m = vm.send("input layout es", 1.5)
        check("saved in /input.cfg" in vm.since(m), "input layout es saved before rebooting")
    finally:
        vm.stop()

    # ---- the layout survives a reboot ----
    vm = InputYesOS(disk, 40000 + (os.getpid() + 1) % 20000, qmp + "2")
    try:
        check(vm.wait_for("YesOS [", 90), "second boot on the same disk")
        check("1 pointer(s), layout es" in vm.output(), "boot summary: layout es, read from /input.cfg")
        for k in (("q",), ("q",), ("spc",), ("semicolon",), ("apostrophe",), ("o",)):
            vm.tap(k[0], *k[1:])
        got = enter_and_read(vm)
        check(got == "qq ñó", f"the Spanish layout is in use after the reboot ({got!r})")
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
