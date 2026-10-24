#!/usr/bin/env python3
"""
Apps test: HolyQ programs from the disk run in an isolated PMP cell, each in
a window of its own (src/apps.c, the core's cells). Boots YesOS Desktop with
a display, keyboard and tablet, and:
  - PAINT.HQ draws, then waits for four events through input(): a typed
    'a', Enter, Up and a press at (50, 30) in its window; it prints each
    event and draws a square for it. The events must arrive exactly, and the
    window's contents must equal, pixel for pixel, the canvas computed here;
    when it ends the title says "(finished)" and the last frame stays;
  - LOOP.HQ animates, sleeping 1 s per frame (SleepMS): five console
    commands each answer within 0.3 s meanwhile (the app's sleep no longer
    holds hart 0; with the old busy wait the median is about half a second),
    and closing its window stops it ("stopped by the user") and frees the cell;
  - STEAL.HQ reads kernel memory: the PMP stops it (hardware fault), the
    title says "(fault)", and the desktop carries on.
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
import test_window as tw  # noqa: E402
sys.path.insert(1, os.path.join(os.path.dirname(HERE), "core", "tools"))
import redsea_injector  # noqa: E402

harness = ti.harness
failures = []
CW, CH = 320, 240

PAINT = b"""U0 Main() {
    gfx_clear(0x00102030);
    gfx_rect(10, 10, 100, 50, 0x00FF8000);
    gfx_present();
    I64 n = 0;
    I64 x = 10;
    while (n < 4) {
        I64 e = input();
        if (e != 0) {
            kprintf("EV %x\\n", e);
            gfx_rect(x, 180, 20, 20, 0x0000C000 + n * 64);
            x = x + 30;
            n = n + 1;
            gfx_present();
        }
        SleepMS(5);
    }
    kprintf("PAINT done\\n");
}
Main();
"""
LOOP = b"""U0 Main() {
    I64 t = 0;
    while (1) {
        gfx_clear(0x00000000);
        gfx_rect((t * 5) % 300, 100, 20, 20, 0x0000FFFF);
        gfx_present();
        SleepMS(1000);
        t = t + 1;
    }
}
Main();
"""
STEAL = b"""U0 Main() {
    gfx_clear(0x00400000);
    gfx_present();
    I64 p = 0x80000000;
    kprintf("stolen: %x\\n", p[0]);
}
Main();
"""


def check(cond, label):
    print(("  PASS  " if cond else "  FAIL  ") + label, flush=True)
    if not cond:
        failures.append(label)
    return cond


def paint_canvas():
    c = np.zeros((CH, CW, 3), dtype=np.uint8)
    c[:, :] = (0x10, 0x20, 0x30)
    c[10:60, 10:110] = (0xFF, 0x80, 0x00)
    for n in range(4):
        x = 10 + 30 * n
        c[180:200, x:x + 20] = (0x00, 0xC0, n * 64)
    return c


def windows(vm):
    return {int(i): (t, int(x), int(y)) for i, t, x, y, *_ in tw.listing(vm)}


def main():
    tmpdir = tempfile.mkdtemp(prefix="yesos_apps_")
    disk = os.path.join(tmpdir, "disk.img")
    qmp = os.path.join(tmpdir, "qmp.sock")
    shot = os.path.join(tmpdir, "shot.ppm")
    shutil.copyfile(harness.SRC_DISK, disk)
    for name, src in (("PAINT.HQ", PAINT), ("LOOP.HQ", LOOP), ("STEAL.HQ", STEAL)):
        redsea_injector.inject_bytes(disk, src, name)
    vm = ti.InputYesOS(disk, 40000 + os.getpid() % 20000, qmp)
    try:
        check(vm.wait_for("YesOS [", 90), "system boots with a display, keyboard and tablet")
        m = vm.send("app", 1.5)
        check("No app is running. 1 cell(s) available." in vm.since(m), "app: nothing running, one cell available")

        # --- PAINT: input events and the window's pixels -------------------------
        m = vm.send("app run /PAINT.HQ", 2)
        out = vm.since(m)
        r = re.search(r"\[APP\] PAINT.HQ running in window (\d+)", out)
        check(r is not None and "started on cell hart 3" in out, "app run /PAINT.HQ: running on cell hart 3 in a window")
        wid = int(r.group(1)) if r else -1
        win = windows(vm).get(wid)
        check(win is not None and win[0] == "PAINT.HQ - cell 3", f"its window is titled \"PAINT.HQ - cell 3\" ({win})")
        if win:
            _, wx, wy = win
            cx, cy = wx + tw.B, wy + tw.B + tw.T                     # contents' top-left on the screen
            m = vm.send("", 0.2)
            ti.type_us(vm, "a")
            time.sleep(0.3)
            vm.tap("ret")
            time.sleep(0.3)
            vm.tap("up")
            time.sleep(0.3)
            s = tw.Session(vm)
            s.point(cx + 50, cy + 30)
            vm.button("left", True)
            vm.button("left", False)
            s.point(1000, 750)                                       # the pointer out of the way
            vm.wait_for("PAINT done", 10, m)
            out = vm.since(m)
            evs = re.findall(r"EV ([0-9a-fA-F]+)", out)
            check([int(e, 16) for e in evs] == [0x61, 10, 0x110001, 0x80000000 | (50 << 16) | 30],
                  f"the app read exactly: 'a', Enter, Up, a press at (50, 30) ({evs})")
            check("PAINT.HQ finished in its cell" in out, "the app finished in its cell")
            time.sleep(0.5)
            img = vm.screendump(shot)
            got = img[cy:cy + CH, cx:cx + CW]
            diff = int(np.count_nonzero(np.any(got != paint_canvas(), axis=2)))
            check(diff == 0, f"its window shows the last frame pixel for pixel ({diff} pixel(s) differ)")
            check(windows(vm).get(wid, ("",))[0] == "PAINT.HQ (finished)", "the title says \"PAINT.HQ (finished)\"")
            vm.send(f"window close {wid}", 1)

        # --- LOOP: hart 0 stays free, closing stops the app ------------------------
        m = vm.send("app run /LOOP.HQ", 2)
        r = re.search(r"\[APP\] LOOP.HQ running in window (\d+)", vm.since(m))
        lid = int(r.group(1)) if r else -1
        check(r is not None, "app run /LOOP.HQ: an animation running")
        time.sleep(1.5)
        lat = []
        for _ in range(5):                                        # tight polling: the reply's latency
            m = len(vm.output())
            t0 = time.time()
            vm.p.stdin.write(b"uptime\r")
            vm.p.stdin.flush()
            while "Uptime" not in vm.output()[m:] and time.time() - t0 < 5:
                time.sleep(0.005)
            lat.append(time.time() - t0)
            time.sleep(0.37)                                      # land at different points of the app's sleep
        lat.sort()
        check(lat[-1] < 0.3, f"five console commands answer within 0.3 s while the app sleeps between frames "
                             f"(latencies {', '.join(f'{v:.3f}' for v in lat)} s)")
        m = vm.send(f"window close {lid}", 1.5)
        out = vm.since(m)
        f = re.search(r"LOOP.HQ stopped: stopped by the user", out)
        check(f is not None, "closing its window stops it: \"stopped by the user\"")
        m = vm.send("app", 1.5)
        check("No app is running. 1 cell(s) available." in vm.since(m), "the cell is free again")

        # --- STEAL: the PMP stops it -------------------------------------------------
        m = vm.send("app run /STEAL.HQ", 3)
        out = vm.since(m)
        r = re.search(r"\[APP\] STEAL.HQ running in window (\d+)", out)
        sid = int(r.group(1)) if r else -1
        vm.wait_for("STEAL.HQ stopped", 5, m)
        out = vm.since(m)
        check("STEAL.HQ stopped: hardware fault" in out and "stolen:" not in out,
              "an app reading kernel memory is stopped by the PMP (hardware fault), nothing printed")
        check(windows(vm).get(sid, ("",))[0] == "STEAL.HQ (fault)", "its title says \"STEAL.HQ (fault)\"")
        m = vm.send("desktop", 1.5)
        check("Desktop: on" in vm.since(m), "the desktop carries on")
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
