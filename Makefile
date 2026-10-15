# =============================================================================
# YesOS Desktop - the YesOS core, its serial console and, one feature at a
# time, the desktop (today: HD Audio output, WAV files from the RedSea disk,
# a framebuffer). The core is the `core` submodule.
#   make            build yesos.elf
#   make run        boot it in QEMU on its own yesos_disk.img, with sound
#                   (AUDIO=pa by default; AUDIO=pipewire, alsa or none);
#                   the console is this terminal, the screen is not shown
#   make run-gui    the same with the screen in a window (UI=gtk by default,
#                   or sdl); the console stays in this terminal
#   make test       run tools/test_desktop.py, test_audio.py, test_display.py
# Exit QEMU: Ctrl-A X
# =============================================================================
PRODUCT = desktop
CORE_MODULES =
PRODUCT_C_SRCS = $(wildcard src/*.c)
DISK_MB = 64

include core/core.mk

AUDIO ?= pa
QEMU_AUDIO = -audiodev $(AUDIO),id=snd0 -device intel-hda,id=hda0 -device hda-output,bus=hda0.0,audiodev=snd0

QEMU_SCREEN = -device bochs-display
UI ?= gtk

run: $(ELF) yesos_disk.img
	qemu-system-riscv64 $(QEMU_CPU) $(QEMU_DISK) $(QEMU_AUDIO) $(QEMU_SCREEN) -kernel $(ELF) -nographic

run-gui: $(ELF) yesos_disk.img
	qemu-system-riscv64 $(QEMU_CPU) $(QEMU_DISK) $(QEMU_AUDIO) $(QEMU_SCREEN) -kernel $(ELF) \
		-display $(UI) -serial mon:stdio

TESTS = test_desktop test_audio test_display
test: $(ELF)
	@fail=0; for t in $(TESTS); do echo "== $$t"; python3 tools/$$t.py || fail=1; done; exit $$fail

.PHONY: run run-gui test
