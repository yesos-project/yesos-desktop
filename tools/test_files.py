#!/usr/bin/env python3
"""
File browser test (windows, stage 3): boots YesOS Desktop with a display, a
virtio keyboard and tablet, and an HD Audio output recorded to a WAV file.
Files are put on the RedSea disk first (a text file with tabs, UTF-8, a
control character and a long line; a binary file; a WAV file) and from the
console (a directory with a text file). Through QMP the test opens the
browser, moves with the keys, enters a directory, opens text files with
Enter and with a double click, scrolls a viewer, plays WAV files in the
background (they must come out of the recording bit for bit, and the
browser must answer keys while a 3-second one plays), and checks that keys go to the
focused window and not to the console. Screenshots are compared pixel for
pixel with the screen composed by the test (test_window's model of the
windows, plus models of the browser and the viewer). Non-zero exit status
on failure.
"""
import os
import re
import shutil
import signal
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "core", "tools"))
import test_input as ti  # noqa: E402
import test_window as tw  # noqa: E402
import test_audio as ta  # noqa: E402
import redsea_injector  # noqa: E402

harness = ti.harness
failures = []
W, H = 1024, 768
FCOLS, FROWS, VCOLS, VROWS = 48, 20, 72, 24
TEXT, SEL_BG, SEL_FG, ST_BG, ST_FG = 0x202020, 0x2B4C7E, 0xFFFFFF, 0xD8D4C8, 0x404040


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


class FilesYesOS(ti.InputYesOS):
    """Display, keyboard, tablet (test_input) plus an HD Audio output recorded to a WAV file."""

    def __init__(self, disk, port, qmp, wav):
        orig = harness.subprocess.Popen

        def popen(cmd, *a, **kw):
            return orig(list(cmd) + [
                "-audiodev", f"wav,id=snd0,path={wav},out.frequency=48000,out.channels=2,out.format=s16",
                "-device", "intel-hda,id=hda0", "-device", "hda-output,bus=hda0.0,audiodev=snd0"], *a, **kw)

        harness.subprocess.Popen = popen
        try:
            super().__init__(disk, port, qmp)
        finally:
            harness.subprocess.Popen = orig

    def quit(self):
        self.p.send_signal(signal.SIGTERM)          # QEMU closes the WAV file properly
        try:
            self.p.wait(10)
        except Exception:
            self.p.kill()
            self.p.wait()


def cells(img, col, row, text, maxn, color):
    for i, ch in enumerate(text[:maxn]):
        tw.put_glyph(img, (col + i) * 8, row * 16, ord(ch), color)


def paper(cols, rows):
    img = np.zeros((rows * 16, cols * 8, 3), np.uint8)
    img[:] = ti.rgb(tw.PAPER)
    return img


def status(img, cols, rows, text):
    img[(rows - 1) * 16:rows * 16] = ti.rgb(ST_BG)
    cells(img, 1, rows - 1, text, cols - 2, ST_FG)


def browser_picture(path, entries, sel, top, msg):
    """Same as browser_draw() in src/files.c. entries: (name, is_dir, size)."""
    img = paper(FCOLS, FROWS)
    up = path != "/"
    shown = ([("..", None, 0)] if up else []) + entries
    for r in range(FROWS - 1):
        i = top + r
        if i >= len(shown):
            break
        fg = SEL_FG if i == sel else TEXT
        if i == sel:
            img[r * 16:(r + 1) * 16] = ti.rgb(SEL_BG)
        name, is_dir, size = shown[i]
        if is_dir is None:
            cells(img, 1, r, "..", FCOLS - 2, fg)
            continue
        s = "" if is_dir else str(size)
        room = FCOLS - 2 - (len(s) + 1 if s else 0)
        cells(img, 1, r, name, room, fg)
        if is_dir:
            if len(name) < room:
                cells(img, 1 + len(name), r, "/", 1, fg)
        else:
            cells(img, FCOLS - 1 - len(s), r, s, len(s), fg)
    status(img, FCOLS, FROWS, msg or f"{path}  {len(entries)} entries")
    return img


def viewer_lines(data):
    lines, at = [], 0
    while True:
        nl = data.find(b"\n", at)
        if nl < 0 or nl + 1 >= len(data):
            lines.append(data[at:])
            break
        lines.append(data[at:nl + 1])
        at = nl + 1
    return lines


def viewer_picture(name, data, top):
    """Same as viewer_draw() in src/files.c (valid UTF-8 input)."""
    img = paper(VCOLS, VROWS)
    lines = viewer_lines(data)
    rows = VROWS - 1
    for r in range(rows):
        i = top + r
        if i >= len(lines):
            break
        line = lines[i]
        if line.endswith(b"\n"):
            line = line[:-1]
        if line.endswith(b"\r"):
            line = line[:-1]
        col = 0
        for ch in line.decode():
            if col >= VCOLS:
                break
            if ch == "\t":
                col = (col // 8 + 1) * 8
                continue
            cp = ord(ch)
            if cp < 32 or cp == 127:
                cp = ord("?")
            tw.put_glyph(img, col * 8, r * 16, cp, TEXT)
            col += 1
    last = min(top + rows, len(lines))
    status(img, VCOLS, VROWS, f"{name}  lines {top + 1}-{last} of {len(lines)}")
    return img


def listing(vm):
    m = vm.send("files list", 1.5)
    out = vm.since(m)
    head = re.search(r"Files (\d+): (\S+), (\d+) entries, selected (\d+), status \"(.*)\"", out)
    entries = []
    for name, slash, size in re.findall(r"\n  ([^ \n]+?)(/?) (\d+)\r?(?=\n)", out):
        entries.append((name, slash == "/", int(size)))
    return head, entries


def windows(vm):
    m = vm.send("window list", 1.5)
    return {int(i): (int(x), int(y)) for i, x, y in re.findall(r"  (\d+): \".*?\" at (\d+),(\d+),", vm.since(m))}


def main():
    tmpdir = tempfile.mkdtemp(prefix="yesos_files_")
    disk = os.path.join(tmpdir, "disk.img")
    qmp = os.path.join(tmpdir, "qmp.sock")
    shot = os.path.join(tmpdir, "shot.ppm")
    wav = os.path.join(tmpdir, "out.wav")
    shutil.copyfile(harness.SRC_DISK, disk)

    long_txt = ("Line 1: plain text\n\tLine 2: after a tab\nLine 3: Año, señor, ¿qué tal? ¡Bien!\n"
                "Line 4: a control character \x01 here\n"
                "Line 5: " + "a long line, cut at the window's width " * 3 + "\n"
                + "".join(f"Line {i}: more\n" for i in range(6, 61))).encode()
    rng = np.random.default_rng(3)
    tone = (rng.integers(-20000, 20000, (12000, 2))).astype(np.int16)
    tone[0] = (12345, -23456)
    song = (rng.integers(-12000, 12000, (144000, 2))).astype(np.int16)      # 3 s
    song[0] = (23456, -12345)
    files = {"LONG.TXT": long_txt, "BIN.DAT": b"\x7fELF\x00\x01binary\x00data",
             "TONE.WAV": ta.wav_bytes(48000, 2, tone), "LONG.WAV": ta.wav_bytes(48000, 2, song)}
    for name, data in files.items():
        check(redsea_injector.inject_bytes(disk, data, name), f"{name} ({len(data)} bytes) put on the disk")

    vm = FilesYesOS(disk, 40000 + os.getpid() % 20000, qmp, wav)
    try:
        check(vm.wait_for("YesOS [", 90), "system boots with display, keyboard, tablet and audio")
        hola = "¡Hola! Texto en la carpeta docs."
        vm.send("mkdir /docs", 1)
        vm.send(f"write /docs/hola.txt {hola}", 1)
        s = tw.Session(vm)
        s.point(1000, 750)
        con = tw.Win(0, 40, 40, tw.COLS, tw.ROWS, tw.TITLE, closable=False)
        s.clear_terminal()

        def screen(wins, focus, label):
            con.content = s.client()
            compare(vm.screendump(shot), tw.compose(W, H, wins, focus, s.ptr), label)

        m = vm.send("files", 1.5)
        check("[FILES] window 1: /" in vm.since(m), "files: a browser window for /")
        head, root = listing(vm)
        names = [e[0] for e in root]
        check(head is not None and "docs" in names and "LONG.TXT" in names and "TONE.WAV" in names,
              f"the browser lists the disk: {names}")
        pos = windows(vm)
        br = tw.Win(1, *pos[1], FCOLS, FROWS, "Files: /")
        br.content = browser_picture("/", root, 0, 0, "")
        screen([con, br], 1, "browser at /: entries, sizes, selection bar, status line")

        # keys go to the focused browser, not to the console
        vm.tap("end")
        ti.type_us(vm, "qq")
        vm.tap("ret")                                       # Enter opens the last entry (TONE.WAV plays)
        time.sleep(1)
        check("Unknown command: 'qq" not in vm.output(), "letters typed with the browser focused do not reach the console")

        # back to the top, then down to docs/ and in
        head, _ = listing(vm)
        last = root[-1]
        vm.tap("home")
        for _ in range(names.index("docs")):
            vm.tap("down")
        vm.tap("ret")
        time.sleep(1)
        head, docs = listing(vm)
        check(head is not None and head.group(2) == "/docs" and [e[0] for e in docs] == ["hola.txt"],
              f"Enter on docs/: /docs with hola.txt ({head.group(2) if head else '?'}, {docs})")
        br.title = "Files: /docs"
        br.content = browser_picture("/docs", docs, 0, 0, "")
        vm.tap("down")
        br.content = browser_picture("/docs", docs, 1, 0, "")
        vm.tap("ret")                                       # opens hola.txt in a viewer
        time.sleep(1)
        pos = windows(vm)
        vid = max(pos)
        v1 = tw.Win(vid, *pos[vid], VCOLS, VROWS, "hola.txt")
        v1.content = viewer_picture("hola.txt", hola.encode(), 0)
        screen([con, br, v1], vid, "Enter on hola.txt: a viewer window with its text")

        # the viewer's close box; the browser gets the focus back; Backspace goes up
        s.point(*tw.close_box(v1))
        vm.button("left", True)
        vm.button("left", False)
        time.sleep(0.5)
        vm.tap("backspace")
        time.sleep(0.5)
        br.title = "Files: /"
        br.content = browser_picture("/", root, 0, 0, "")
        screen([con, br], 1, "viewer closed; Backspace: back to /")

        # double click on LONG.TXT
        row = names.index("LONG.TXT")
        cx, cy = br.x + tw.B + 40, br.y + tw.B + tw.T + row * 16 + 8
        s.point(cx, cy)
        for _ in range(2):
            vm.button("left", True)
            vm.button("left", False)
        time.sleep(1)
        pos = windows(vm)
        vid = max(pos)
        v2 = tw.Win(vid, *pos[vid], VCOLS, VROWS, "LONG.TXT")
        br.content = browser_picture("/", root, row, 0, "")
        v2.content = viewer_picture("LONG.TXT", long_txt, 0)
        screen([con, br, v2], vid, "double click on LONG.TXT: tab, UTF-8, control character, long line cut")
        vm.tap("pgdn")
        v2.content = viewer_picture("LONG.TXT", long_txt, 23)
        screen([con, br, v2], vid, "Page Down in the viewer")
        vm.tap("end")
        vm.tap("up")
        v2.content = viewer_picture("LONG.TXT", long_txt, 60 - 23 - 1)
        screen([con, br, v2], vid, "End, Up: the last lines")

        # a press on the console gives it the keys again
        s.point(60, 690)
        vm.button("left", True)
        vm.button("left", False)
        time.sleep(0.3)
        ti.type_us(vm, "qq console")
        mark = len(vm.output())
        vm.tap("ret")
        check(ti.unknown_line(vm, mark) == "qq console", "with the console focused, keys reach the console again")

        # BIN.DAT and TONE.WAV from the browser
        vm.send("window front 1", 1)
        vm.tap("home")
        for _ in range(names.index("BIN.DAT")):
            vm.tap("down")
        vm.tap("ret")
        time.sleep(1)
        head, _ = listing(vm)
        check(head is not None and head.group(5) == f"BIN.DAT: not text ({len(files['BIN.DAT'])} bytes)",
              f"BIN.DAT refused as not text ({head.group(5) if head else '?'})")
        vm.tap("home")
        for _ in range(names.index("TONE.WAV")):
            vm.tap("down")
        mark = len(vm.output())
        vm.tap("ret")
        check(vm.wait_for("[AUDIO] play done: /TONE.WAV, 12000 frames, 0 underrun(s)", 20, mark),
              "Enter on TONE.WAV plays it")
        time.sleep(0.5)
        head, _ = listing(vm)
        check(head is not None and head.group(5) == "TONE.WAV: played, 12000 frames",
              f"status after playing ({head.group(5) if head else '?'})")

        # a 3-second sound in the background: the browser keeps answering
        vm.send("term off", 1)                              # the console stops changing the screen
        vm.tap("home")
        for _ in range(names.index("LONG.WAV")):
            vm.tap("down")
        mark = len(vm.output())
        t0 = time.time()
        vm.tap("ret")
        vm.tap("up")                                        # keys while it plays
        vm.tap("up")
        busy = time.time() - t0
        sel = names.index("LONG.WAV") - 2
        out_now = vm.since(mark)
        check("[AUDIO] start /LONG.WAV: 48000 Hz, stereo, 144000 frames (3.000 s), in the background" in out_now,
              "Enter on LONG.WAV starts it in the background")
        shotimg = vm.screendump(shot)
        busy = time.time() - t0
        playing = "play done: /LONG.WAV" not in vm.since(mark)
        check(playing, f"still playing when the screen is taken ({busy:.2f} s after Enter)")
        # The console as the screen shows it: drawn up to `term off`.
        t = ti.Terminal(tw.COLS * 8, tw.ROWS * 16)
        raw = vm.raw()
        for b in raw[s.cut:raw.index(b"[TERM] off", s.cut)]:
            t.put(b)
        con.content = t.picture()
        br.content = browser_picture("/", root, sel, 0, "")       # moving clears the status message
        # stacking: console pressed (on top), then `window front 1`
        compare(shotimg, tw.compose(W, H, [v2, con, br], 1, s.ptr),
                "while it plays: the browser answered two keys (selection up twice)")
        check(vm.wait_for("[AUDIO] play done: /LONG.WAV, 144000 frames, 0 underrun(s)", 20, mark),
              "LONG.WAV played to the end with no underrun")
        time.sleep(0.5)
        head, _ = listing(vm)
        check(head is not None and head.group(5) == "LONG.WAV: played, 144000 frames",
              f"the browser says it has ended ({head.group(5) if head else '?'})")
        vm.send("term on", 1)

        m = vm.send("audio start /LONG.WAV", 1)
        time.sleep(0.5)
        m = vm.send("audio stop", 1)
        st = re.search(r"play stopped: /LONG.WAV after (\d+) frames", vm.since(m))
        check(st is not None and 0 < int(st.group(1)) < 144000, f"audio stop: stopped early ({st.group(0) if st else '?'})")

        m = vm.send("window close 1", 1)
        check("[WINDOW] 1 closed" in vm.since(m), "the browser closes (its memory freed by its program)")
        m = vm.send("files /nothing-here", 1)
        check("cannot open /nothing-here" in vm.since(m), "files on a missing directory is refused")
        check("FATAL" not in vm.output() and "RISC-V EXCEPTION" not in vm.output(), "no kernel faults")
    finally:
        vm.quit()
    rec = ta.read_wav(wav)
    check(ta.find_exact(rec, tone) >= 0, "TONE.WAV comes out of the recording bit for bit")
    check(ta.find_exact(rec, song) >= 0, "LONG.WAV, played in the background, comes out bit for bit")
    shutil.rmtree(tmpdir, ignore_errors=True)

    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED")
        sys.exit(1)
    print("All checks passed")


if __name__ == "__main__":
    main()
