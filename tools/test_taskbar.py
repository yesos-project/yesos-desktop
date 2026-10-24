#!/usr/bin/env python3
"""
Task bar test: a button per window along the bottom of the screen and a
minimise box in every title bar. Boots YesOS Desktop with a display,
keyboard and tablet, opens two windows besides the console and, with tablet
presses through QMP, minimises the focused one with its box (the focus goes
to the window below), restores it from its task bar button, minimises the
focused window from its own button, brings another to the front from its
button; `window min` / `window restore`; a press where a minimised window
was reaches what is under it. Every screenshot is compared pixel for pixel
with test_window's model (title bars, boxes, grips, task bar and its
buttons, focused / plain / minimised). Non-zero exit status on failure.
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

harness = ti.harness
failures = []
W, H, B, CLOSE = tw.W, tw.H, tw.B, tw.CLOSE


def check(cond, label):
    print(("  PASS  " if cond else "  FAIL  ") + label, flush=True)
    if not cond:
        failures.append(label)
    return cond


def compare(img, want, label):
    diff = int((img != want).any(axis=2).sum())
    check(diff == 0, f"{label}: {diff} pixel(s) differ")


def button(k, n):
    bw = min(tw.TASK_BTN_W, (W - tw.TASK_GAP) // n - tw.TASK_GAP)
    return tw.TASK_GAP + k * (bw + tw.TASK_GAP) + bw // 2, H - tw.TASKBAR_H + tw.TASKBAR_H // 2


def min_box(win):
    return win.x + win.w - B - CLOSE - 3 - (CLOSE + 4) + 7, win.y + B + 3 + 7


def states(vm):
    return {int(i): (f or "", t or "", mn or "") for i, *_, f, t, mn in
            re.findall(r"  (\d+): \".*?\" at \d+,\d+, \d+x\d+ pixels, \d+x\d+ characters(, focused)?(, top)?(, minimized)?",
                       vm.since(vm.send("window list", 1.5)))}


def main():
    tmpdir = tempfile.mkdtemp(prefix="yesos_taskbar_")
    disk = os.path.join(tmpdir, "disk.img")
    qmp = os.path.join(tmpdir, "qmp.sock")
    shot = os.path.join(tmpdir, "shot.ppm")
    shutil.copyfile(harness.SRC_DISK, disk)
    vm = ti.InputYesOS(disk, 40000 + os.getpid() % 20000, qmp)
    try:
        check(vm.wait_for("YesOS [", 90), "system boots with a display, keyboard and tablet")
        s = tw.Session(vm)
        s.point(1000, 600)
        con = tw.Win(0, 40, 40, tw.COLS, tw.ROWS, tw.TITLE, closable=False)
        vm.send("window new 30 6 Notes", 1.5)
        vm.send("window new 30 6 Second", 1.5)
        w1, w2 = tw.Win(1, 120, 80, 30, 6, "Notes"), tw.Win(2, 152, 112, 30, 6, "Second")
        wins = [con, w1, w2]

        def screen(order, focus, label):
            s.clear_terminal()
            con.content = s.client()
            compare(vm.screendump(shot), tw.compose(W, H, order, focus, s.ptr), label)

        def press(x, y):
            s.point(x, y)
            vm.button("left", True)
            vm.button("left", False)
            time.sleep(0.4)
            s.point(1000, 600)

        screen(wins, 2, "three windows, three task bar buttons, the newest focused")

        press(*min_box(w2))
        w2.minimized = True
        st = states(vm)
        check(st.get(2, ("",) * 3)[2] == ", minimized" and st.get(1, ("",))[0] == ", focused",
              f"its minimise box hides window 2, the focus goes to window 1 ({st})")
        screen([con, w1, w2], 1, "window 2 minimised: not drawn, its button dimmed, window 1's lit")

        press(*button(2, 3))
        w2.minimized = False
        st = states(vm)
        check(st.get(2, ("",) * 3) == (", focused", ", top", ""), f"its task bar button restores it, in front, focused ({st})")
        screen([con, w1, w2], 2, "window 2 restored from the task bar")

        press(*button(2, 3))
        w2.minimized = True
        check(states(vm).get(2, ("",) * 3)[2] == ", minimized", "the focused window's own button minimises it")

        press(*button(0, 3))                                  # the console's button: it comes to the front
        st = states(vm)
        check(st.get(0, ("",) * 3)[:2] == (", focused", ", top"), f"the console's button brings it to the front ({st})")
        screen([w1, w2, con], 0, "the console in front from its button, window 2 still minimised")

        m = vm.send("window min 1", 1.5)
        check("[WINDOW] 1 minimized" in vm.since(m), "window min 1")
        w1.minimized = True
        press(130, 90)                                        # where window 1's title bar was: the console is there
        st = states(vm)
        check(st.get(1, ("",) * 3)[2] == ", minimized" and st.get(0, ("",))[0] == ", focused",
              f"a press where a minimised window was reaches the console under it ({st})")
        m = vm.send("window restore 1", 1.5)
        check("[WINDOW] 1 restored" in vm.since(m), "window restore 1")
        w1.minimized = False
        screen([w2, con, w1], 1, "window 1 restored in front; window 2 still minimised")
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
