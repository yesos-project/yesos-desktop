#!/usr/bin/env python3
"""
File operations test: in the file browser, with keys sent through QMP,
copy (F5), rename (F2), delete (Delete, confirmed with Enter or cancelled
with Esc) and a new folder (F7) on the RedSea disk. Checks the listing and
the files' contents after each step, the refusals (a name that exists, a
name that is not valid, a folder that is not empty, renaming a folder), a
screenshot with a question in the status line pixel for pixel, fsck clean,
and the result still there after a reboot. Non-zero exit status on failure.
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
import test_files as tf  # noqa: E402

harness = ti.harness
failures = []
W, H = 1024, 768


def check(cond, label):
    print(("  PASS  " if cond else "  FAIL  ") + label)
    if not cond:
        failures.append(label)


def compare(got, want, label):
    # here, not test_files.compare: that one records failures in test_files' list
    bad = int((got != want).any(axis=2).sum()) if got.shape == want.shape else -1
    check(bad == 0, f"{label}: {bad} pixel(s) differ" if bad >= 0 else f"{label}: screenshot of another size")


def state(vm):
    head, entries = tf.listing(vm)
    return (head.group(2) if head else None, head.group(5) if head else None,
            int(head.group(4)) if head else -1, entries)


def names(entries):
    return [e[0] + ("/" if e[1] else "") for e in entries]


def select(vm, entries, name, up=True):
    """Select an entry by name with Home and Down (".." comes first below the root)."""
    vm.tap("home")
    for _ in range([e[0] for e in entries].index(name) + (1 if up else 0)):
        vm.tap("down")


def retype(vm, old, new):
    for _ in old:
        vm.tap("backspace")
    ti.type_us(vm, new)


def cat(vm, path):
    m = vm.send(f"cat {path}", 1.5)
    return vm.since(m)


def main():
    tmpdir = tempfile.mkdtemp(prefix="yesos_fileops_")
    disk = os.path.join(tmpdir, "disk.img")
    qmp = os.path.join(tmpdir, "qmp.sock")
    shot = os.path.join(tmpdir, "shot.ppm")
    shutil.copyfile(harness.SRC_DISK, disk)
    vm = ti.InputYesOS(disk, 40000 + os.getpid() % 20000, qmp)
    try:
        check(vm.wait_for("YesOS [", 90), "system boots")
        vm.send("mkdir /work", 1)
        vm.send("write /work/a.txt Hola mundo", 1)
        vm.send("write /work/z.txt otro", 1)
        m = vm.send("files /work", 1.5)
        check("[FILES] window 1: /work" in vm.since(m), "browser on /work")
        path, msg, sel, ents = state(vm)
        check(names(ents) == ["a.txt", "z.txt"], f"/work: {names(ents)}")

        # copy a.txt to b.txt; the question on the screen, pixel for pixel
        s = tw.Session(vm)
        s.point(1000, 750)
        con = tw.Win(0, 40, 40, tw.COLS, tw.ROWS, tw.TITLE, closable=False)
        select(vm, ents, "a.txt")
        vm.tap("f5")
        retype(vm, "a.txt", "b.txt")
        vm.send("term off", 1)                              # the console stops changing the screen
        img = vm.screendump(shot)
        pos = tf.windows(vm)
        br = tw.Win(1, *pos[1], tf.FCOLS, tf.FROWS, "Files: /work")
        br.content = tf.browser_picture("/work", ents, 1, 0, "Copy a.txt to: b.txt_")
        # The console's text is taken from the screenshot itself: test_window
        # checks it; here the browser and the frames are under test.
        con.content = img[40 + tw.B + tw.T:40 + tw.B + tw.T + tw.ROWS * 16, 40 + tw.B:40 + tw.B + tw.COLS * 8].copy()
        compare(img, tw.compose(W, H, [con, br], 1, s.ptr), "F5: 'Copy a.txt to: b.txt_' in the status line")
        vm.send("term on", 1)
        vm.tap("ret")
        time.sleep(0.5)
        path, msg, sel, ents = state(vm)
        check(names(ents) == ["a.txt", "b.txt", "z.txt"] and msg == "a.txt copied to b.txt",
              f"copied: {names(ents)}, \"{msg}\"")
        check(ents[1][2] == ents[0][2] and "Hola mundo" in cat(vm, "/work/b.txt"), "b.txt has a.txt's contents")
        check(sel == 2, f"the copy is selected ({sel})")

        # rename b.txt to c.txt
        vm.tap("f2")
        retype(vm, "b.txt", "c.txt")
        vm.tap("ret")
        time.sleep(0.5)
        path, msg, sel, ents = state(vm)
        check(names(ents) == ["a.txt", "c.txt", "z.txt"] and msg == "b.txt renamed to c.txt",
              f"renamed: {names(ents)}, \"{msg}\"")
        check("Hola mundo" in cat(vm, "/work/c.txt") and "Hola mundo" not in cat(vm, "/work/b.txt"),
              "c.txt has the contents, b.txt is gone")

        # refusals
        vm.tap("f2")
        retype(vm, "c.txt", "a.txt")
        vm.tap("ret")
        time.sleep(0.5)
        _, msg, _, ents2 = state(vm)
        check(msg == "a.txt already exists" and ents2 == ents, f"rename onto an existing name refused (\"{msg}\")")
        vm.tap("f2")
        retype(vm, "c.txt", "x/y")
        vm.tap("ret")
        time.sleep(0.5)
        _, msg, _, _ = state(vm)
        check(msg == "Not a valid name: x/y", f"a name with '/' refused for rename (\"{msg}\")")

        # delete: Esc cancels, Enter deletes
        select(vm, ents, "c.txt")
        vm.tap("delete")
        vm.tap("esc")
        time.sleep(0.3)
        _, msg, _, ents2 = state(vm)
        check(msg == "Cancelled" and "c.txt" in names(ents2), "Delete then Esc: cancelled, c.txt still there")
        vm.tap("delete")
        vm.tap("ret")
        time.sleep(0.5)
        _, msg, _, ents = state(vm)
        check(names(ents) == ["a.txt", "z.txt"] and msg == "c.txt deleted", f"Delete then Enter: {names(ents)}, \"{msg}\"")

        # new folder, a copy into it, refusals for a folder that is not empty
        vm.tap("f7")
        ti.type_us(vm, "sub")
        vm.tap("ret")
        time.sleep(0.5)
        _, msg, _, ents = state(vm)
        check("sub/" in names(ents) and msg == "Folder sub made", f"F7: {names(ents)}, \"{msg}\"")
        select(vm, ents, "a.txt")
        vm.tap("f5")
        retype(vm, "a.txt", "sub/a2.txt")
        vm.tap("ret")
        time.sleep(0.5)
        _, msg, _, ents = state(vm)
        check(msg == "a.txt copied to sub/a2.txt" and "Hola mundo" in cat(vm, "/work/sub/a2.txt"),
              f"copied into the folder (\"{msg}\")")
        select(vm, ents, "sub")
        vm.tap("delete")
        vm.tap("ret")
        time.sleep(0.5)
        _, msg, _, ents2 = state(vm)
        check(msg == "Not deleted: sub is not empty" and "sub/" in names(ents2), f"a folder that is not empty is kept (\"{msg}\")")
        vm.tap("f2")
        retype(vm, "sub", "sub2")
        vm.tap("ret")
        time.sleep(0.5)
        _, msg, _, _ = state(vm)
        check(msg == "Folders cannot be renamed yet", f"renaming a folder refused (\"{msg}\")")

        # empty the folder from inside it, go up, delete it
        vm.tap("ret")                                       # Enter on sub/
        time.sleep(0.5)
        path, _, _, sub = state(vm)
        check(path == "/work/sub" and names(sub) == ["a2.txt"], f"in /work/sub: {names(sub)}")
        select(vm, sub, "a2.txt")
        vm.tap("delete")
        vm.tap("ret")
        vm.tap("backspace")
        time.sleep(0.5)
        _, _, _, ents = state(vm)
        select(vm, ents, "sub")
        vm.tap("delete")
        vm.tap("ret")
        time.sleep(0.5)
        path, msg, _, ents = state(vm)
        check(path == "/work" and names(ents) == ["a.txt", "z.txt"] and msg == "sub deleted",
              f"the empty folder deleted: {names(ents)}, \"{msg}\"")
        m = vm.send("fsck", 5)
        check("fsck: CLEAN" in vm.since(m), "fsck: CLEAN")
        check("FATAL" not in vm.output() and "RISC-V EXCEPTION" not in vm.output(), "no kernel faults")
    finally:
        vm.stop()

    vm = ti.InputYesOS(disk, 40000 + (os.getpid() + 1) % 20000, qmp + "2")
    try:
        check(vm.wait_for("YesOS [", 90), "reboot on the same disk")
        vm.send("files /work", 1.5)
        _, _, _, ents = state(vm)
        check(names(ents) == ["a.txt", "z.txt"], f"after the reboot /work holds a.txt and z.txt ({names(ents)})")
        check("Hola mundo" in cat(vm, "/work/a.txt") and "Hola mundo" not in cat(vm, "/work/c.txt"),
              "a.txt intact, the deleted c.txt gone")
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
