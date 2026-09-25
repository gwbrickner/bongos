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
# Debug (default, RELEASE=0): -O1 + UBSan. Release: -O2, no UBSan (ARCHITECTURE §3).
KERNEL_CFLAGS += $(if $(filter 1,$(RELEASE)),-O2,-O1)

# KERNEL_DEBUG (D-082), debug builds only: gates the pmm's extra write-after-free poisoning on top
# of its always-on double-free/misuse state-machine checks (kernel/mm/pmm.c), and
# kernel/include/list.h's NULL-the-links-on-remove hardening.
KERNEL_CFLAGS += $(if $(filter 1,$(RELEASE)),,-DKERNEL_DEBUG=1)

# UBSan (D-076), debug builds only: an explicit check list, never the bare `-fsanitize=undefined`
# group -- confirmed by direct compiler probe that clang 18 folds `function` into that group for
# C, which prefixes every function with an 8-byte type-hash checked on indirect calls; the kernel's
# hand-written asm entry points (kernelEntry, the trap stubs, ...) carry no such prefix, so an
# indirect call into one would read unmapped memory just before it. `local-bounds` is excluded too
# (emits a bare `ud2`, bypassing the handler entirely). kernel/core/ubsan.c (the handlers
# themselves) is compiled with -fno-sanitize=all instead, via its own override rule below --
# self-instrumenting the runtime that reports UBSan trips would be circular.
KERNEL_UBSAN_CHECKS := alignment,bool,builtin,bounds,enum,integer-divide-by-zero,nonnull-attribute,null,object-size,pointer-overflow,returns-nonnull-attribute,shift,signed-integer-overflow,unreachable,vla-bound
KERNEL_UBSAN_FLAGS := $(if $(filter 1,$(RELEASE)),,-fsanitize=$(KERNEL_UBSAN_CHECKS) -DKERNEL_UBSAN=1)

# fbcon (kernel/drivers/fbcon) links boot/common/fbtext.c's glyph-blit primitive and
# boot-status.c (for logging BootStatus failures), plus mk/font.mk's generated font data --
# the same files the loader itself links, so the loader's menu and the kernel's console draw
# with byte-identical glyphs. The generated KSYM blob (KSYMS_EMPTY_C/KSYMS_C, D-075 below) is
# deliberately NOT in this list -- it needs a different object per link pass, so it's added
# explicitly at each $(ld.lld) invocation instead of once here.
KERNEL_C_SOURCES := $(sort $(wildcard kernel/core/*.c) $(wildcard kernel/drivers/serial/*.c) \
                           $(wildcard kernel/drivers/fbcon/*.c) $(wildcard kernel/test/*.c) \
                           $(wildcard kernel/arch/x86_64/*.c) $(wildcard kernel/arch/x86_64/test/*.c) \
                           $(wildcard kernel/mm/*.c) \
                           boot/common/fbtext.c boot/common/boot-status.c) $(CONSOLE_FONT_C)
KERNEL_ASM_SOURCES := $(sort $(wildcard kernel/arch/x86_64/*.asm))

KERNEL_C_OBJECTS := $(patsubst %.c,$(KERNEL_BUILD)/%.o,$(KERNEL_C_SOURCES))
KERNEL_ASM_OBJECTS := $(patsubst %.asm,$(KERNEL_BUILD)/%.o,$(KERNEL_ASM_SOURCES))
KERNEL_OBJECTS := $(KERNEL_C_OBJECTS) $(KERNEL_ASM_OBJECTS)

# ubsan.c's own override must come before the generic pattern rule below it (Make prefers a more
# specific/explicit rule over a pattern rule for the same target) -- it can't be built with the
# very sanitizer flags it implements the handlers for.
$(KERNEL_BUILD)/kernel/core/ubsan.o: kernel/core/ubsan.c $(BRANDING_HDR)
	@mkdir -p $(dir $@)
	$(KERNEL_CC) $(KERNEL_CFLAGS) -fno-sanitize=all -c -o $@ $<

$(KERNEL_BUILD)/%.o: %.c $(BRANDING_HDR)
	@mkdir -p $(dir $@)
	$(KERNEL_CC) $(KERNEL_CFLAGS) $(KERNEL_UBSAN_FLAGS) -c -o $@ $<
-include $(KERNEL_C_OBJECTS:.o=.d)

$(KERNEL_BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(KERNEL_AS) -f elf64 -g -F dwarf -o $@ $<

# KSYM v1 (D-075, docs/specs/ksyms.md): the kernel's own final symbol addresses have to be
# embedded in that same binary, so it's linked twice. Pass 1 links with an empty placeholder blob
# (tools/ksyms empty); tools/ksyms gen reads pass 1's own symtab to build the real blob; pass 2
# (the real $(KERNEL_ELF)) links with that. `.ksyms` sits in .rodata *after* .ktests (kernel.ld),
# so no function's address differs between the two passes -- ksyms check re-verifies that by
# byte-comparing a fresh encoding of the final ELF's own symbols against what actually shipped in
# its .ksyms section, failing (and deleting) the build on any mismatch.
KSYMS_DIR   := $(BUILD)/tools/ksyms
KSYMS_BIN   := $(KSYMS_DIR)/ksyms
KSYMS_SRCS  := tools/ksyms/main.c tools/ksyms/elf-read.c tools/ksyms/ksyms-encode.c
KSYMS_HDRS  := tools/ksyms/elf-read.h tools/ksyms/ksyms-encode.h

$(KSYMS_BIN): $(KSYMS_SRCS) $(KSYMS_HDRS)
	@mkdir -p $(dir $@)
	clang -std=c17 -Wall -Wextra -Werror -O1 -o $@ $(KSYMS_SRCS)

KSYMS_EMPTY_C := $(BUILD)/gen/ksyms-empty.c
KSYMS_C       := $(BUILD)/gen/ksyms.c
KSYMS_EMPTY_OBJECT := $(patsubst %.c,$(KERNEL_BUILD)/%.o,$(KSYMS_EMPTY_C))
KSYMS_OBJECT        := $(patsubst %.c,$(KERNEL_BUILD)/%.o,$(KSYMS_C))
KERNEL_PASS1_ELF := $(KERNEL_BUILD)/kernel.pass1.elf

$(KSYMS_EMPTY_C): $(KSYMS_BIN)
	@mkdir -p $(dir $@)
	$(KSYMS_BIN) empty > $@

$(KERNEL_PASS1_ELF): $(KERNEL_OBJECTS) $(KSYMS_EMPTY_OBJECT) $(KERNEL_LD)
	@mkdir -p $(dir $@)
	ld.lld -T $(KERNEL_LD) -nostdlib -static --emit-relocs -z max-page-size=0x1000 \
		--build-id=none --orphan-handling=error -o $@ $(KERNEL_OBJECTS) $(KSYMS_EMPTY_OBJECT)

$(KSYMS_C): $(KSYMS_BIN) $(KERNEL_PASS1_ELF)
	@mkdir -p $(dir $@)
	$(KSYMS_BIN) gen $(KERNEL_PASS1_ELF) > $@

$(KERNEL_ELF): $(KERNEL_OBJECTS) $(KSYMS_OBJECT) $(KERNEL_LD) $(KSYMS_BIN)
	@mkdir -p $(dir $@)
	ld.lld -T $(KERNEL_LD) -nostdlib -static --emit-relocs -z max-page-size=0x1000 \
		--build-id=none --orphan-handling=error -o $@ $(KERNEL_OBJECTS) $(KSYMS_OBJECT)
	@$(KSYMS_BIN) check $@ || { rm -f $@; exit 1; }

.PHONY: kernel
kernel: branding $(KERNEL_ELF)
