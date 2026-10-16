# YesOS Desktop (RISC-V)

The desktop product of YesOS, built on the YesOS core (the `core` git
submodule): Machine mode only, a locked Z3-proven PMP, an integer-only kernel
(no floating point, checked at every build), no Linux, libc or binary blobs.

Today it is the core, its serial console, the RedSea v4 disk, **HD Audio
output** (including WAV files played from the disk), a **framebuffer** with
a **desktop** of overlapping windows (the console, a **file browser** and a
text viewer), a
**virtio keyboard and tablet** (US or Spanish layout): no network services;
of the core's optional modules only `gfx` and `lineedit`. Features (apps in
PMP cells, more programs) are added here one at a time, each with its own
tests, brought over piece by piece from the old desktop experiments in
`yesos-lab`.

The audio processing in `yesos-lab` labelled "EML" (resampler, dither,
limiter) turned out to be conventional fixed-point code: it uses none of the
EML number system. It was brought over and measured once (yesos-desktop
commit `d900e40`) and then taken out again. The EML number system itself
(`eml64_t`, logarithmic) is evaluated in `yesos-lab` first, for accuracy and
instruction cost; it comes here only if it measures as worth having.

Design choices, a comparison with Linux, seL4, Zephyr and TempleOS, and what is
measured today: see the [yesos-core README](https://github.com/yesos-project/yesos-core#2-design-choices).

Requirements (clang, lld, llvm, QEMU, Rust, Python): see the
[yesos-core README](https://github.com/yesos-project/yesos-core#readme).

```bash
git clone --recurse-submodules https://github.com/yesos-project/yesos-desktop && cd yesos-desktop
make prover-setup        # once (Z3, in core/tools/.venv)
make                     # yesos.elf
make run                 # serial console (Ctrl-A X to quit), own yesos_disk.img, sound via PulseAudio
make run AUDIO=none      # the same without sound (or AUDIO=pipewire, alsa)
make run-gui             # the screen in a window (UI=gtk, or UI=sdl): type there or in the terminal
make test                # tools/test_*.py: desktop, audio, display, input, window, files, fileops
```

`python3 tools/test_desktop.py` (17 checks): it boots as YesOS Desktop with the
PMP locked and proven, its console has only the core's commands, nothing
answers on the network, a file written from the console survives a reboot,
UTF-8 text typed at the console is stored and read back (and backspace
removes a whole ñ), and fsck is clean.

## Audio (`src/hda.c`, `src/wav.c`, `src/audio_shell.c`)

- **Intel High Definition Audio**, found on the PCI bus through the core's
  `drivers/pci.c`. The controller is driven by polling (no interrupts).
- The codec is discovered, not assumed: its function group, output converter
  and output pin are found by asking the codec, and the amplifier is set to the
  step the codec reports as 0 dB, so the output level is exact.
- One stream, 16-bit stereo, from a power-of-two DMA ring of 16384 frames
  (341 ms at 48 kHz). The rate is any the codec reports among 8–192 kHz
  (QEMU's: 16, 22.05, 32, 44.1, 48, 88.2 and 96 kHz); tones use 48 kHz. The write position follows the DMA position; the ring is
  filled before the DMA starts and kept full of silence while a sound drains,
  so the DMA never replays old samples.
- **Tone generator:** a 32-bit phase accumulator reading a 1024-point Q15 sine
  table (generated on the host by `tools/gen_sine_q15.py`). Integer only.
- **Pisano counter:** beside the ring runs the Fibonacci recurrence modulo
  the ring size, which wraps after exactly one Pisano period,
  π(2^14) = 3·2^13 = 24576 steps; the period is verified at init. It is
  telemetry: the samples are placed linearly.

- **WAV files from RedSea** (`audio play <file>`): 16-bit PCM, mono or
  stereo, played at the file's own rate: no resampling, so the samples reach
  the codec unchanged; mono goes to both channels. The parser checks every
  chunk size against the file size and refuses what it cannot play (other
  sample sizes, compressed formats, rates the codec lacks) with a message.
  Limit: the file is read into memory whole (heap 128 MiB).
- **In the background** (`audio start <file>`, and the file browser): the
  call returns at once and the main loop keeps the ring filled (it runs
  every millisecond; the ring lasts 341 ms), so the desktop keeps working
  while it plays; the end is reported on the console and in the browser.
  `audio stop` stops it. One sound at a time: a new one, a tone or
  `audio play` stops the one in the background. If the main loop is held
  up for longer than the ring (a long command), the DMA plays old samples:
  that is detected from the clock and counted as an underrun.
  `audio play` and `audio tone` still wait until they have played.

```text
Audio       audio [status]   audio tone <Hz> <ms> [level %]   audio play <file.wav>
            audio start <file.wav>   audio stop
```

`python3 tools/test_audio.py` (38 checks) boots with QEMU's `intel-hda` and
`hda-output`, records the output into a WAV file and measures it:

| Tone | Duration | Frequency | Level | SINAD |
|---|---|---|---|---|
| 1000 Hz, 50 % | 1500.0 ms (asked 1500) | 1000.00 Hz | −9.03 dBFS (expected −9.03) | 55.6 dB |
| 440 Hz, 25 % | 1000.0 ms (asked 1000) | 440.00 Hz | −15.05 dBFS (expected −15.05) | 55.0 dB |

Both channels are identical and there are no underruns. The SINAD (signal
against noise and distortion) of about 55 dB is what a 1024-point table read
without interpolation gives.

WAV files, generated by the test and written into the RedSea disk image:

| File | Result |
|---|---|
| 48 kHz stereo, 0.5 s (a tone with noise on the left, full-range noise on the right, both extremes) | the recording contains all 24000 frames bit for bit |
| 44.1 kHz mono, 0.5 s (a sweep and both extremes), in a second run recorded at 44.1 kHz | all 22050 frames bit for bit, on both channels |
| a text file; an 8-bit WAV; a chunk longer than the file; 12345 Hz; a missing file | each refused with its reason, no kernel fault |

## Display (`src/display.c`)

- QEMU's **bochs-display** (PCI 1234:1111), found through the core's PCI
  enumerator, which assigns its BARs: the framebuffer (BAR0, 16 MiB) and the
  Bochs VBE "DISPI" registers (BAR2 + 0x500).
- 32 bits per pixel (0x00RRGGBB), 1024x768 at boot; `display mode` sets any
  mode the device accepts, read back from the registers (up to 4096x4096
  and the size of video memory); a refused mode falls back to 1024x768.
- Rectangle fill and a test pattern (colour bars, grey ramp, checkerboard,
  red frame). Integer only.
- **Text** in the core's 8x16 font (`fonts/yesos-8x16.txt` in the core):
  UTF-8 up to Latin-1, so Spanish text shows as typed (á é í ó ú ñ ¿ ¡ ü);
  a character the font lacks is drawn as `?`; clipped at the edges; a
  transparent or a painted background. The console accepts UTF-8 too.
  The font comes from the lab's (its own drawing in the style of the VGA
  font, not a copy) with real descenders for g j p q y, which stopped at
  the baseline, and the Spanish letters added.

```text
Display     display [status]   display test   display clear [rrggbb]   display mode <w> <h>
            display print <x> <y> <rrggbb> <text>
```

`python3 tools/test_display.py` (23 checks) boots with `-device
bochs-display`, draws from the console and takes screenshots through QEMU's
monitor (`screendump`): the black screen at boot, the test pattern at
1024x768, 800x600 and 1280x720, a solid colour, and Spanish text in four
colours with a missing glyph (→) and a line clipped at the right edge all
match the picture computed by the test **pixel for pixel**. A 9000x9000 mode is refused, and
without a display device the system boots, says so, and the commands refuse
cleanly. (It switches the on-screen terminal off first, to draw on its own.)

## Console on the screen (`src/terminal.c`)

Everything the console writes is also drawn on the display, in the
desktop's window (below) or, with `desktop off`, on the whole screen: the
core's kprintf hook sees it, and since core commit `4707457` the shell's echo of
typed characters goes through it too. 100x40 characters in the window (128x48
on the whole screen at 1024x768), in the
core's 8x16 font, with UTF-8, a cursor, scrolling, line wrap (backspace goes
back over it), tabs and the ANSI sequences the core prints (colours 30-37,
clear screen). It follows `display mode`. `term off` leaves the screen to
the `display` commands.

```text
Terminal    term [status]   term on   term off   term clear
```

## Desktop and windows (`src/desktop.c`, `src/surface.c`)

- A plain background and **overlapping windows**, up to 8. Window 0 holds
  the console and cannot be closed; others are opened with `window new`
  and written into with `window text`.
- Each window keeps its **contents in its own buffer** off the screen
  (`Surface`); the console draws into its window's buffer. The screen is
  **composed** only where something changed: background, the windows from
  the bottom up, the pointer on top. So text written into a window that
  others cover shows only where it is visible, and what a moved or closed
  window covered comes back.
- A **press** on a window brings it to the front and gives it the focus
  (its title bar turns blue; the others' are grey). Its **title bar drags**
  it; the **box** at the right of the title bar closes it. Windows always
  stay whole on the screen. Keys go to the focused window if its program
  takes them (the browser and the viewer do), else to the console.
- `desktop off` gives the whole screen to the console; `desktop on` puts
  the windows back. A mode change lays everything out again.

```text
Desktop     desktop [status]   desktop on   desktop off   desktop move <x> <y>
            window [list]   window new <cols> <rows> <title>   window text <id> <col> <row> <rrggbb> <text>
            window front <id>   window move <id> <x> <y>   window close <id>
```

`python3 tools/test_window.py` (35 checks, two boots) moves windows with
tablet events sent through QMP and compares each screenshot **pixel for
pixel** with the screen composed by the test's own model of the windows
(background, borders, title bars focused or not, titles cut before the
close box, close boxes, contents, the console replayed from the serial
stream, the pointer):

| Stage | What is checked |
|---|---|
| 1 | the console's window at boot, dragged, moved from the console with text typed at the new place, small overlapping moves; presses outside the title bar move nothing; a drag past the edge stops with the window whole; `desktop off`/`on`; 800x600 |
| 2 | three windows, the newest on top and focused; a press on the console brings it over the others; `window front`; a window dragged over the console; text typed into the console under another window; the close box; the console's window cannot be closed; at most 8 windows; all closed again |

Faults put in on purpose to see the test catch them: a background band
left unpainted after a move (3 comparisons fail, 98 350 pixels); window
contents drawn straight onto the screen instead of composed (the covered
console check fails, 4 064 pixels); the grey of a title bar one step off
(5 comparisons fail).

Not yet: resizing, minimising.

## File browser and viewer (`src/files.c`) — windows, stage 3

- `files [path]` opens a **browser** window on a directory of the RedSea
  disk (through the VFS, which also shows the RAM file system in the same
  tree): `..` first below the root, then the entries, sizes of files on
  the right, a selection bar and a status line.
- Up/Down/Home/End/Page Up/Page Down move, **Enter or a double click
  opens**, Backspace goes up. A directory is entered; a **`.wav` file is
  played in the background** (the desktop keeps working; the status line
  says when it has ended); any
  other file opens in a **viewer** window if it is text (no NUL byte, at
  most 1 MiB), else the status line says why not.
- **File operations**, the name typed in the status line (Esc cancels):
  **F5 copies** a file (to a name in the same folder, or a path such as
  `sub/copy.txt`), **F2 renames** it, **Delete** deletes a file or an empty
  folder after Enter, **F7 makes a folder**. A name that exists or is not
  valid is refused. A rename on the RedSea disk gives the data a second
  name, then removes the old one: cut in between, both names are left and
  nothing is lost. Folders are not copied or renamed yet; a folder that is
  not empty is not deleted (RedSea would leave its files behind).
- The **viewer** shows the lines cut at its width (no wrapping), tabs to 8
  columns, control characters as `?`; Up/Down/Page Up/Page Down/Home/End
  scroll; its status line says which lines are shown.

```text
Files       files [path]   files list
            in the browser: F5 copy  F2 rename  Delete delete  F7 new folder  Esc cancel
```

`python3 tools/test_files.py` (30 checks) puts a text file (tab, UTF-8, a
control character, a long line, 60 lines), a binary file and a WAV file on
the disk, makes a directory with a text file from the console, and drives
everything with keys and tablet events through QMP: the browser at `/`,
`docs/` entered with Enter, `hola.txt` opened, the viewer closed by its
box, Backspace back to `/`, `LONG.TXT` opened with a double click and
scrolled (Page Down, End, Up) — each screen **pixel for pixel** against
the test's models of the windows, the browser and the viewer. Letters
typed with the browser focused do not reach the console, and do after a
press on the console; `BIN.DAT` is refused as not text; `TONE.WAV` plays
and comes out of the recording **bit for bit**. A 3-second `LONG.WAV` is
started from the browser and, while it is still playing, the browser
answers two keys (the screen is compared pixel for pixel); it ends with no
underrun, comes out bit for bit, and the browser says it has ended;
`audio stop` stops it early. With the browser switched back to waiting
while it plays, six of these checks fail. A tab width of 4 instead of
8 in the viewer is caught (636 pixels).

`python3 tools/test_fileops.py` (24 checks): copy, rename and delete with
the keys through QMP, the listing and the files' contents after each step;
a name that exists, a name with `/` for a rename, Delete then Esc, a folder
that is not empty and renaming a folder are refused; a copy into a new
folder, the folder emptied and deleted; the question in the status line on
the screen pixel for pixel; `fsck` clean; after a reboot the disk holds
what it should. A rename that keeps the old name makes 5 checks fail.

Limits: the viewer does not wrap lines; at most 64 entries per directory
are listed; folders are not copied or renamed.

## Keyboard and pointer (`src/input.c`)

- **virtio-input** (PCI 1AF4:1052): QEMU's `virtio-keyboard-pci`,
  `virtio-tablet-pci` and `virtio-mouse-pci`, up to 4, each recognised by
  the event types it reports; polled from the main loop. Rewritten from the
  lab's driver: BARs from the core's PCI allocator (the lab used its own
  fixed window), rings in their natural layout (the lab's were packed, the
  bug that once made virtio-blk read torn indexes), the tablet's axis range
  read from the device.
- **Keyboard**: Linux key codes to UTF-8, typed into the console as if they
  came from the serial port. Layouts `us` and `es` (Spanish: ñ, ¿ ¡, ç, º ª,
  AltGr for @ # | € [ ] { }, and dead keys: ´ ¨ ` ^ followed by a vowel give
  á ü à â ..., followed by a space the accent itself). Shift, caps lock,
  keypad; Ctrl and Alt combinations type nothing. The layout is kept in
  `/input.cfg` on the RedSea disk and used again at the next boot.
- **Editing keys**: arrows, Home, End and Delete reach the console as the
  escape sequences a terminal sends, so the core's line editor (cursor
  inside the line, the last 16 commands with up and down) works the same
  from the window and from the serial port.
- **Pointer**: an arrow on the screen at the tablet's position (axis 0..max
  scaled to 0..width-1), or moved by a mouse; the pixels under it are put
  back when it moves, and all drawing happens under it. Clicks of the three
  buttons, where the last one happened, and the wheel are counted.

```text
Input       input [status]   input layout us|es
```

`python3 tools/test_input.py` (35 checks) boots with a display, a
virtio keyboard and a virtio tablet, and sends events through QEMU's QMP
socket (`input-send-event`), as the window does:

| What is sent | Result |
|---|---|
| US layout: letters, shifted symbols, digits | the shell receives exactly the typed line |
| backspace, caps lock (and shift with it), Ctrl+H, Alt+H, keypad | `qq abcDEFg12+3*4-/.` |
| Spanish layout: ñ Ñ ¡ ¿, dead keys with vowels and with space, ç º ª, shifted digits, AltGr | `ñÑ¡¿?áÉüÜòî´´nçÇºª"·/=@#\|€[]{}+*<>-_;:ÑA`, exact |
| backspace after á and ñ | each removed whole |
| left, Home, End, Delete, up/down from the virtio keyboard | the line edited as expected; history recalled |
| `input layout es`, reboot on the same disk | the boot says `layout es`; ñ and ó type as Spanish |
| tablet at 4 positions, corners included | the pixel expected from the axis range |
| clicks (left twice, right, middle), wheel 3 up 1 down | counted, at the pointer, all released; wheel 2 |
| pointer over a plain colour, moved, in the corner, text drawn across it | **pixel for pixel**: arrow on top, old place repainted, clipped |
| typed text, command output, two `help` (the screen scrolls) | the screen equals the serial stream replayed by the test's own terminal, **pixel for pixel** |

Honest limits:
- Only the boot hart draws on the screen; output of the other harts reaches
  the serial port only (counted as "dropped" by `term`). No scrollback.
- Function keys, Page Up/Down and Ctrl shortcuts are not mapped; no
  keyboard LEDs.
- The font covers ASCII, the Latin-1 letters of Spanish and the Western
  languages' accented vowels, and €; anything else shows as `?`.
- The pointer moves, raises and closes windows; no window has a program of its own yet.
- Tested with QEMU's devices only; a real board would need its own drivers.

The other console commands are the core's (see its README).

## Support YesOS

YesOS is developed in the open and funded by donations. They pay for
development time and for real RISC-V boards, so the missing storage and
network drivers can be written and tested outside the emulator.

[Donate on Liberapay](https://liberapay.com/yesos-project/donate)

## License
MIT. In memory of Terry A. Davis (1969–2018).

YesOS is inspired by his TempleOS (its HolyC became HolyQ here, and the RedSea
filesystem) and is not affiliated with it.
