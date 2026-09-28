# BIOS stage1/stage2 (ARCHITECTURE §5.6, D-099/D-100/D-101, docs/specs/bios-boot.md, ROADMAP
# M2.5). Included before mk/image.mk, which references $(BIOS_STAGE1_BIN)/$(BIOS_STAGE2_BIN) as
# `image` prerequisites.
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

# stage2's i386 build (D-104): boot/bios/stage2's own NASM (16-bit real-mode bring-up: the
# header, A20, the GDT, the protected-mode switch) and C (stage2Main onward) sources, plus the
# same shared boot/common/hw/*.c sources the UEFI loader uses (serial.c, libc-shim.c, cpu.c) --
# cross-compiled for i386 instead of x86_64-windows here. Linked with boot/bios/stage2/stage2.ld,
# then `objcopy -O binary` flattens the result into exactly what stage1 loads at 0x8000 (no ELF
# structure survives).
BIOS_OBJ_DIR := $(BOOT_BIOS_DIR)/obj
BIOS_CC := clang
BIOS_CFLAGS := --target=i386-unknown-elf -m32 -march=i686 -std=c17 -ffreestanding -fno-pic \
               -fno-pie -mgeneral-regs-only -fno-stack-protector -fcf-protection=none \
               -fno-asynchronous-unwind-tables -fno-unwind-tables -Os \
               -I$(BUILD)/include -Iboot/common/include -Iboot/bios/stage2 -MMD -MP \
               -Wall -Wextra -Werror

BIOS_STAGE2_C_SOURCES := $(wildcard boot/bios/stage2/*.c)
BIOS_STAGE2_HW_SOURCES := boot/common/hw/serial.c boot/common/hw/libc-shim.c boot/common/hw/cpu.c
BIOS_STAGE2_COMMON_SOURCES := boot/common/bootmem.c boot/common/memmap.c boot/common/bootheap.c \
                               boot/common/boot-status.c
BIOS_STAGE2_ASM_SOURCES := boot/bios/stage2/entry.asm boot/bios/stage2/rm.asm

BIOS_STAGE2_C_OBJECTS := $(patsubst boot/bios/stage2/%.c,$(BIOS_OBJ_DIR)/%.o,$(BIOS_STAGE2_C_SOURCES))
BIOS_STAGE2_HW_OBJECTS := $(patsubst boot/common/hw/%.c,$(BIOS_OBJ_DIR)/hw-%.o,$(BIOS_STAGE2_HW_SOURCES))
BIOS_STAGE2_COMMON_OBJECTS := $(patsubst boot/common/%.c,$(BIOS_OBJ_DIR)/common-%.o,$(BIOS_STAGE2_COMMON_SOURCES))
BIOS_STAGE2_ASM_OBJECTS := $(patsubst boot/bios/stage2/%.asm,$(BIOS_OBJ_DIR)/%.o,$(BIOS_STAGE2_ASM_SOURCES))
BIOS_STAGE2_OBJECTS := $(BIOS_STAGE2_ASM_OBJECTS) $(BIOS_STAGE2_C_OBJECTS) $(BIOS_STAGE2_HW_OBJECTS) \
                       $(BIOS_STAGE2_COMMON_OBJECTS)
BIOS_STAGE2_ELF := $(BOOT_BIOS_DIR)/stage2.elf

$(BIOS_OBJ_DIR)/%.o: boot/bios/stage2/%.c $(BRANDING_HDR)
	@mkdir -p $(dir $@)
	$(BIOS_CC) $(BIOS_CFLAGS) -c -o $@ $<
$(BIOS_OBJ_DIR)/hw-%.o: boot/common/hw/%.c $(BRANDING_HDR)
	@mkdir -p $(dir $@)
	$(BIOS_CC) $(BIOS_CFLAGS) -c -o $@ $<
$(BIOS_OBJ_DIR)/common-%.o: boot/common/%.c $(BRANDING_HDR)
	@mkdir -p $(dir $@)
	$(BIOS_CC) $(BIOS_CFLAGS) -c -o $@ $<
$(BIOS_OBJ_DIR)/%.o: boot/bios/stage2/%.asm
	@mkdir -p $(dir $@)
	nasm -f elf32 -g -F dwarf -o $@ $<
-include $(BIOS_STAGE2_C_OBJECTS:.o=.d) $(BIOS_STAGE2_HW_OBJECTS:.o=.d) $(BIOS_STAGE2_COMMON_OBJECTS:.o=.d)

$(BIOS_STAGE2_ELF): $(BIOS_STAGE2_OBJECTS) boot/bios/stage2/stage2.ld
	ld.lld -m elf_i386 -T boot/bios/stage2/stage2.ld -nostdlib -static --gc-sections \
		-o $@ $(BIOS_STAGE2_OBJECTS)

$(BIOS_STAGE2_BIN): $(BIOS_STAGE2_ELF)
	@mkdir -p $(dir $@)
	llvm-objcopy -O binary $< $@
