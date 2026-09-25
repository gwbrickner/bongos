# make's kernel build: kernel/** -> build/kernel/kernel.elf (ARCHITECTURE §3/§5.1, ROADMAP M1.3).
# Cross-compiled with clang's freestanding x86_64-elf target and linked by ld.lld against
# kernel/arch/x86_64/kernel.ld. mk/image.mk depends on $(KERNEL_ELF) and passes it to mkimage.
KERNEL_BUILD := $(BUILD)/kernel
KERNEL_LD    := kernel/arch/x86_64/kernel.ld
KERNEL_ELF   := $(KERNEL_BUILD)/kernel.elf

KERNEL_CC := clang
KERNEL_AS := nasm

# -I order: kernel/include (portable headers, ktest/klog/panic/uapi/sections/arch interface),
# kernel/arch/x86_64/include (the x86 arch backend behind kernel/include/arch/*.h, D-062),
# kernel (so e.g. "drivers/serial/uart16550.h" resolves without a relative path), and
# boot/common/include (the shared BootInfo ABI struct, ARCHITECTURE §5.3).
KERNEL_CFLAGS := --target=x86_64-unknown-elf -std=c17 -ffreestanding -nostdlib -mno-red-zone \
                 -mgeneral-regs-only -mcmodel=kernel -fno-pic -fno-omit-frame-pointer \
                 -fstack-protector-strong -fno-asynchronous-unwind-tables -fno-unwind-tables \
                 -mstack-protector-guard=global \
                 -Ikernel/include -Ikernel/arch/x86_64/include -Ikernel -Iboot/common/include \
                 -I$(BUILD)/include -MMD -MP \
                 -Wall -Wextra -Werror
# Debug (default, RELEASE=0): -O1, no sanitizers yet (UBSan's runtime arrives in M2.1). Release:
# -O2, same otherwise (ARCHITECTURE §3).
KERNEL_CFLAGS += $(if $(filter 1,$(RELEASE)),-O2,-O1)

# fbcon (kernel/drivers/fbcon) links boot/common/fbtext.c's glyph-blit primitive and
# boot-status.c (for logging BootStatus failures), plus mk/font.mk's generated font data --
# the same files the loader itself links, so the loader's menu and the kernel's console draw
# with byte-identical glyphs.
KERNEL_C_SOURCES := $(sort $(wildcard kernel/core/*.c) $(wildcard kernel/drivers/serial/*.c) \
                           $(wildcard kernel/drivers/fbcon/*.c) $(wildcard kernel/test/*.c) \
                           $(wildcard kernel/arch/x86_64/*.c) $(wildcard kernel/arch/x86_64/test/*.c) \
                           boot/common/fbtext.c boot/common/boot-status.c) $(CONSOLE_FONT_C)
KERNEL_ASM_SOURCES := $(sort $(wildcard kernel/arch/x86_64/*.asm))

KERNEL_C_OBJECTS := $(patsubst %.c,$(KERNEL_BUILD)/%.o,$(KERNEL_C_SOURCES))
KERNEL_ASM_OBJECTS := $(patsubst %.asm,$(KERNEL_BUILD)/%.o,$(KERNEL_ASM_SOURCES))
KERNEL_OBJECTS := $(KERNEL_C_OBJECTS) $(KERNEL_ASM_OBJECTS)

$(KERNEL_BUILD)/%.o: %.c $(BRANDING_HDR)
	@mkdir -p $(dir $@)
	$(KERNEL_CC) $(KERNEL_CFLAGS) -c -o $@ $<
-include $(KERNEL_C_OBJECTS:.o=.d)

$(KERNEL_BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(KERNEL_AS) -f elf64 -g -F dwarf -o $@ $<

$(KERNEL_ELF): $(KERNEL_OBJECTS) $(KERNEL_LD)
	@mkdir -p $(dir $@)
	ld.lld -T $(KERNEL_LD) -nostdlib -static --emit-relocs -z max-page-size=0x1000 \
		--build-id=none --orphan-handling=error -o $@ $(KERNEL_OBJECTS)

.PHONY: kernel
kernel: branding $(KERNEL_ELF)
