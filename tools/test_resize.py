#!/usr/bin/env python3
"""
Resize test: windows change size in whole characters, by `window size` or
by dragging the grip in the bottom-right corner of their contents (tablet
events through QMP), and every screenshot is compared pixel for pixel with
the picture computed here (test_window's model, grips included):
  - the console, full of `help` text, shrinks to 60x10: the rows that end
    with the cursor's are kept, cut at the new width (test_input.Terminal
    replays the stream and resizes at the same point); then its grip is
    dragged 80 x 48 pixels: 70x13, nothing lost;
  - a plain window with text grows (the text stays top-left, paper around)
    and is clamped to 16x3 when asked for less;
  - the file browser re-lays itself out; an app's window cannot be resized.
Non-zero exit status on failure.
"""
import os
import re
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_input as ti  # noqa: E402
import test_window as tw  # noqa: E402
sys.path.insert(1, os.path.join(os.path.dirname(HERE), "core", "tools"))
import redsea_injector  # noqa: E402

harness = ti.harness
failures = []
W, H, B, T = tw.W, tw.H, tw.B, tw.T
WAIT = b"U0 Main() { while (1) { gfx_present(); SleepMS(100); } }\nMain();\n"


def check(cond, label):
    print(("  PASS  " if cond else "  FAIL  ") + label, flush=True)
    if not cond:
        failures.append(label)
    return cond


def compare(img, want, label):
    diff = int((img != want).any(axis=2).sum())
    check(diff == 0, f"{label}: {diff} pixel(s) differ")


def size_of(vm, wid):
    for i, title, x, y, pw, ph, c, r, *_ in tw.listing(vm):
        if int(i) == wid:
            return int(x), int(y), int(c), int(r)
    return None


def main():
    tmpdir = tempfile.mkdtemp(prefix="yesos_resize_")
    disk = os.path.join(tmpdir, "disk.img")
    qmp = os.path.join(tmpdir, "qmp.sock")
    shot = os.path.join(tmpdir, "shot.ppm")
    shutil.copyfile(harness.SRC_DISK, disk)
    redsea_injector.inject_bytes(disk, WAIT, "WAIT.HQ")
    vm = ti.InputYesOS(disk, 40000 + os.getpid() % 20000, qmp)
    try:
        check(vm.wait_for("YesOS [", 90), "system boots with a display, keyboard and tablet")
        s = tw.Session(vm)
        s.point(1000, 750)
        s.clear_terminal()
        vm.send("help", 2)                                    # more lines than the window: it scrolls

        # --- the console: shrink by command -------------------------------------
        m = vm.send("window size 0 60 10", 1.5)
        out = vm.since(m)
        check("[WINDOW] 0 is 60x10 characters" in out, "window size 0 60 10: the console is 60x10")
        raw = vm.raw()
        split = raw.index(b"[WINDOW] 0 is 60x10", s.cut)          # the resize happened before this print
        t = ti.Terminal(tw.COLS * 8, tw.ROWS * 16)
        fed = s.cut

        def feed(upto):
            nonlocal fed
            for b in vm.raw()[fed:upto]:
                t.put(b)
            fed = upto

        feed(split)
        t.resize(60, 10)
        feed(len(raw))
        con = tw.Win(0, 40, 40, 60, 10, tw.TITLE, closable=False)
        con.content = t.picture()
        compare(vm.screendump(shot), tw.compose(W, H, [con], 0, s.ptr),
                "the console shrunk: the last rows kept, cut at 60 columns, the grip drawn")
        check(size_of(vm, 0) == (40, 40, 60, 10), f"window list: the console at 40,40, 60x10 ({size_of(vm, 0)})")

        # --- the console: grow by dragging its grip -----------------------------
        feed(len(vm.raw()))                                       # the `window list` output, at 60x10
        gx, gy = con.x + con.w - B - 6, con.y + con.h - B - 6
        s.drag(gx, gy, gx + 80, gy + 48, steps=8)
        s.point(1000, 750)
        time.sleep(0.5)
        t.resize(70, 13)
        con = tw.Win(0, 40, 40, 70, 13, tw.TITLE, closable=False)
        con.content = t.picture()
        compare(vm.screendump(shot), tw.compose(W, H, [con], 0, s.ptr), "the console grown by its grip, text kept")
        check(size_of(vm, 0) == (40, 40, 70, 13), f"dragging the grip 80 x 48 pixels: 70x13 ({size_of(vm, 0)})")
        feed(len(vm.raw()))

        # --- a plain window -------------------------------------------------------------
        m = vm.send("window new 30 8 Notes", 1.5)
        check("[WINDOW] 1 \"Notes\" at 120,80, 30x8 characters" in vm.since(m), "window new 30 8 Notes")
        vm.send("window text 1 2 1 802020 kept top-left", 1)
        w1 = tw.Win(1, 120, 80, 30, 8, "Notes")
        w1.text(2, 1, 0x802020, "kept top-left")
        m = vm.send("window size 1 40 10", 1.5)
        check("[WINDOW] 1 is 40x10 characters" in vm.since(m), "window size 1 40 10")
        old = w1.content
        w1 = tw.Win(1, 120, 80, 40, 10, "Notes")
        w1.content[:old.shape[0], :old.shape[1]] = old
        feed(len(vm.raw()))
        con.content = t.picture()
        compare(vm.screendump(shot), tw.compose(W, H, [con, w1], 1, s.ptr),
                "the plain window grown: its text top-left, paper around")
        m = vm.send("window size 1 5 1", 1.5)
        check("[WINDOW] 1 is 16x3 characters" in vm.since(m), "asked for 5x1: clamped to the minimum, 16x3")

        # --- the browser and an app -------------------------------------------------
        vm.send("files /", 2)
        ids = [int(i) for i, *_ in tw.listing(vm)]
        bid = max(ids)
        m = vm.send(f"window size {bid} 30 6", 1.5)
        check(f"[WINDOW] {bid} is 30x6 characters" in vm.since(m), f"the file browser (window {bid}) resized to 30x6")
        m = vm.send("app run /WAIT.HQ", 2)
        r = re.search(r"\[APP\] WAIT.HQ running in window (\d+)", vm.since(m))
        aid = int(r.group(1)) if r else -1
        m = vm.send(f"window size {aid} 20 5", 1.5)
        check(f"window: window {aid} cannot be resized" in vm.since(m), "an app's window cannot be resized (its canvas is fixed)")
        vm.send(f"window close {aid}", 1.5)
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
