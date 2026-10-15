# =============================================================================
# YesOS Desktop - the YesOS core, its serial console and, one feature at a
# time, the desktop (today: HD Audio output). The core is the `core` submodule.
#   make            build yesos.elf
#   make run        boot it in QEMU on its own yesos_disk.img, with sound
#                   (AUDIO=pa by default; AUDIO=pipewire, alsa or none)
#   make test       run tools/test_desktop.py, test_audio.py and test_eml.py
# Exit QEMU: Ctrl-A X
# =============================================================================
PRODUCT = desktop
CORE_MODULES =
PRODUCT_C_SRCS = $(wildcard src/*.c)
DISK_MB = 64

include core/core.mk

AUDIO ?= pa
QEMU_AUDIO = -audiodev $(AUDIO),id=snd0 -device intel-hda,id=hda0 -device hda-output,bus=hda0.0,audiodev=snd0

run: $(ELF) yesos_disk.img
	qemu-system-riscv64 $(QEMU_CPU) $(QEMU_DISK) $(QEMU_AUDIO) -kernel $(ELF) -nographic

TESTS = test_desktop test_audio test_eml
test: $(ELF)
	@fail=0; for t in $(TESTS); do echo "== $$t"; python3 tools/$$t.py || fail=1; done; exit $$fail

.PHONY: run test
