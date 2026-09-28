# BIOS stage1/stage2 (ARCHITECTURE §5.6, D-099/D-100/D-101, docs/specs/bios-boot.md, ROADMAP
# M2.5). Included before mk/image.mk, which references $(BIOS_STAGE1_BIN)/$(BIOS_STAGE2_BIN) as
# `image` prerequisites.
#
# stage2 is a single flat NASM binary today (boot/bios/stage2/skeleton.asm): just enough to prove
# stage1 -> stage2 handoff works under real QEMU (prints over COM1 and halts). It has no A20/
# protected-mode switch, no C environment, and no real-mode thunk yet -- those replace this file
# incrementally as M2.5 continues; once stage2 gains a 32-bit C part (D-104), this becomes a real
# link step (nasm -f elf32 + the i386 build of boot/common + ld.lld -T stage2.ld + objcopy -O
# binary), not a single `nasm -f bin` invocation.
BOOT_BIOS_DIR := $(BUILD)/boot-bios

BIOS_STAGE1_BIN := $(BOOT_BIOS_DIR)/stage1.bin
BIOS_STAGE2_BIN := $(BOOT_BIOS_DIR)/stage2.bin

$(BIOS_STAGE1_BIN): boot/bios/stage1.asm
	@mkdir -p $(dir $@)
	nasm -f bin -o $@ $<
	@actual=$$(stat -c%s $@); \
	if [ "$$actual" != 440 ]; then \
	    echo "mk/bios.mk: $@ is $$actual bytes, must be exactly 440 (the protective MBR's " \
	         "boot-code area, docs/specs/bios-boot.md)"; \
	    rm -f $@; exit 1; \
	fi

$(BIOS_STAGE2_BIN): boot/bios/stage2/skeleton.asm
	@mkdir -p $(dir $@)
	nasm -f bin -o $@ $<
