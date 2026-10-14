# =============================================================================
# YesOS Desktop - for now the YesOS core and its serial console (the core is
# the `core` submodule). Features are added one at a time.
#   make            build yesos.elf
#   make run        boot it in QEMU on its own yesos_disk.img
#   make test       run tools/test_desktop.py
# Exit QEMU: Ctrl-A X
# =============================================================================
PRODUCT = desktop
CORE_MODULES =
PRODUCT_C_SRCS = $(wildcard src/*.c)
DISK_MB = 64

include core/core.mk

run: $(ELF) yesos_disk.img
	qemu-system-riscv64 $(QEMU_CPU) $(QEMU_DISK) -kernel $(ELF) -nographic

test: $(ELF)
	python3 tools/test_desktop.py

.PHONY: run test
