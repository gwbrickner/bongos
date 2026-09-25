# make's kernel build: kernel/** -> build/kernel/kernel.elf (ARCHITECTURE §3/§5.1, ROADMAP
# M1.3/M2.1). Cross-compiled with clang's freestanding x86_64-elf target and linked by ld.lld
# against kernel/arch/x86_64/kernel.ld. mk/image.mk depends on $(KERNEL_ELF) and passes it to
# mkimage.
KERNEL_BUILD := $(BUILD)/kernel
KERNEL_LD    := kernel/arch/x86_64/kernel.ld
KERNEL_ELF   := $(KERNEL_BUILD)/kernel.elf

KERNEL_CC := clang
KERNEL_AS := nasm

# Debug (default, RELEASE=0): UBSan with our own kernel handlers (kernel/core/ubsan.c, D-075).
# Release: no sanitizers. $(filter 1,...) rather than ifeq: Makefile's `RELEASE ?= 0   # ...`
# leaves a trailing space on the unset-default value that a naive ifeq comparison would trip on.
KERNEL_UBSAN_FLAGS := -fsanitize=alignment,bool,builtin,array-bounds,enum,integer-divide-by-zero,nonnull-attribute,null,object-size,pointer-overflow,returns-nonnull-attribute,shift,signed-integer-overflow,unreachable,vla-bound -fno-sanitize-recover=all
# array-bounds (not the wider `bounds`, which also pulls in `local-bounds` and traps via a bare
# `ud2` -- indistinguishable from a real #UD) and no `function`/`vptr` (need C++ RTTI) or
# `float-cast-overflow` (the kernel is -mgeneral-regs-only, no FP).
KERNEL_UBSAN := $(if $(filter 1,$(RELEASE)),0,1)

# -I order: kernel/include (portable headers, ktest/klog/panic/uapi/sections/arch interface),
# kernel/arch/x86_64/include (the x86 arch backend behind kernel/include/arch/*.h, D-062),
# kernel (so e.g. "drivers/serial/uart16550.h" resolves without a relative path), and
# boot/common/include (the shared BootInfo ABI struct, ARCHITECTURE §5.3).
KERNEL_CFLAGS := --target=x86_64-unknown-elf -std=c17 -ffreestanding -nostdlib -mno-red-zone \
                 -mgeneral-regs-only -mcmodel=kernel -fno-pic -fno-omit-frame-pointer \
                 -mno-omit-leaf-frame-pointer -fstack-protector-strong \
                 -fno-asynchronous-unwind-tables -fno-unwind-tables \
                 -mstack-protector-guard=global -DKERNEL_UBSAN=$(KERNEL_UBSAN) \
                 -Ikernel/include -Ikernel/arch/x86_64/include -Ikernel -Iboot/common/include \
                 -I$(BUILD)/include -MMD -MP \
                 -Wall -Wextra -Werror
# Debug (default, RELEASE=0): -O1, plus UBSan and -fno-optimize-sibling-calls (a tail call would
# drop the frame pointer a backtrace needs, D-072). Release: -O2, no sanitizers (ARCHITECTURE §3).
KERNEL_CFLAGS += $(if $(filter 1,$(RELEASE)),-O2,-O1 -fno-optimize-sibling-calls $(KERNEL_UBSAN_FLAGS))

# kernel/core/ubsan.c implements the handlers KERNEL_UBSAN_FLAGS' checks call into, so it must
# never itself be instrumented with them (a handler tripping the same check it's reporting would
# recurse). Harmless in release builds too, where no sanitize flags are present to begin with.
$(KERNEL_BUILD)/kernel/core/ubsan.o: KERNEL_CFLAGS += -fno-sanitize=all

# Object files don't otherwise depend on KERNEL_CFLAGS, so toggling RELEASE (which changes
# -O1/-O2 and the UBSan flags) between two `make` invocations would silently reuse stale objects
# compiled with the other setting's flags (D-075). This stamp file is rewritten only when the
# flags actually change, and every kernel object depends on it.
KERNEL_CFLAGS_STAMP := $(KERNEL_BUILD)/cflags.stamp
$(shell mkdir -p $(KERNEL_BUILD))
$(shell [ -f $(KERNEL_CFLAGS_STAMP) ] && [ "$$(cat $(KERNEL_CFLAGS_STAMP))" = "$(KERNEL_CFLAGS)" ] \
	|| echo "$(KERNEL_CFLAGS)" > $(KERNEL_CFLAGS_STAMP))

# fbcon (kernel/drivers/fbcon) links boot/common/fbtext.c's glyph-blit primitive and
# boot-status.c (for logging BootStatus failures), plus mk/font.mk's generated font data --
# the same files the loader itself links, so the loader's menu and the kernel's console draw
# with byte-identical glyphs. kernel/arch/x86_64/{cpu-tables,trap,ktests}.c (M2.1, D-072/D-076):
# GDT/TSS/IDT setup, exception dispatch, and the x86-specific ktests -- not picked up by the
# kernel/core or kernel/test wildcards below since they live under kernel/arch/x86_64/.
KERNEL_C_SOURCES := $(sort $(wildcard kernel/core/*.c) $(wildcard kernel/drivers/serial/*.c) \
                           $(wildcard kernel/drivers/fbcon/*.c) $(wildcard kernel/test/*.c) \
                           kernel/arch/x86_64/qemu.c kernel/arch/x86_64/cpu-tables.c \
                           kernel/arch/x86_64/trap.c kernel/arch/x86_64/ktests.c \
                           boot/common/fbtext.c boot/common/boot-status.c) $(CONSOLE_FONT_C)
KERNEL_ASM_SOURCES := kernel/arch/x86_64/entry.asm kernel/arch/x86_64/cpu-load.asm \
                      kernel/arch/x86_64/isr.asm kernel/arch/x86_64/jmp.asm \
                      kernel/arch/x86_64/ktest-helpers.asm

KERNEL_C_OBJECTS := $(patsubst %.c,$(KERNEL_BUILD)/%.o,$(KERNEL_C_SOURCES))
KERNEL_ASM_OBJECTS := $(patsubst %.asm,$(KERNEL_BUILD)/%.o,$(KERNEL_ASM_SOURCES))
# .ksyms.asm (below) is linked in separately per pass, not part of the common object set, since
# each pass needs a different KSYMS_BLOB.
KERNEL_OBJECTS := $(KERNEL_C_OBJECTS) $(KERNEL_ASM_OBJECTS)

$(KERNEL_BUILD)/%.o: %.c $(BRANDING_HDR) $(KERNEL_CFLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(KERNEL_CC) $(KERNEL_CFLAGS) -c -o $@ $<
-include $(KERNEL_C_OBJECTS:.o=.d)

$(KERNEL_BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(KERNEL_AS) -f elf64 -g -F dwarf -o $@ $<

# tools/mksyms (D-073): builds the embedded ksyms table from kernel.elf's own symtab. A host
# tool, plain clang, no cross flags -- see docs/specs/ksyms.md for the two-pass link this feeds.
MKSYMS_BIN := $(KERNEL_BUILD)/tools/mksyms
$(MKSYMS_BIN): tools/mksyms/main.c kernel/include/ksyms-format.h kernel/include/uapi/status.h
	@mkdir -p $(dir $@)
	clang -std=c17 -Ikernel/include -Wall -Wextra -Werror -O1 -o $@ tools/mksyms/main.c

KSYMS_ASM := kernel/arch/x86_64/ksyms.asm
KSYMS_EMPTY_BIN := $(KERNEL_BUILD)/ksyms-empty.bin
KSYMS_BIN        := $(KERNEL_BUILD)/ksyms.bin
KERNEL_PASS1_ELF := $(KERNEL_BUILD)/kernel.pass1.elf

$(KSYMS_EMPTY_BIN):
	@mkdir -p $(dir $@)
	: > $@

$(KERNEL_BUILD)/kernel/arch/x86_64/ksyms.pass1.o: $(KSYMS_ASM) $(KSYMS_EMPTY_BIN)
	@mkdir -p $(dir $@)
	$(KERNEL_AS) -f elf64 -g -F dwarf -DKSYMS_BLOB='"$(KSYMS_EMPTY_BIN)"' -o $@ $(KSYMS_ASM)

$(KERNEL_PASS1_ELF): $(KERNEL_OBJECTS) $(KERNEL_BUILD)/kernel/arch/x86_64/ksyms.pass1.o $(KERNEL_LD)
	@mkdir -p $(dir $@)
	ld.lld -T $(KERNEL_LD) -nostdlib -static --emit-relocs -z max-page-size=0x1000 \
		--build-id=none --orphan-handling=error -o $@ \
		$(KERNEL_OBJECTS) $(KERNEL_BUILD)/kernel/arch/x86_64/ksyms.pass1.o

$(KSYMS_BIN): $(MKSYMS_BIN) $(KERNEL_PASS1_ELF)
	@mkdir -p $(dir $@)
	$(MKSYMS_BIN) -o $@ $(KERNEL_PASS1_ELF)

$(KERNEL_BUILD)/kernel/arch/x86_64/ksyms.pass2.o: $(KSYMS_ASM) $(KSYMS_BIN)
	@mkdir -p $(dir $@)
	$(KERNEL_AS) -f elf64 -g -F dwarf -DKSYMS_BLOB='"$(KSYMS_BIN)"' -o $@ $(KSYMS_ASM)

$(KERNEL_ELF): $(KERNEL_OBJECTS) $(KERNEL_BUILD)/kernel/arch/x86_64/ksyms.pass2.o $(KERNEL_LD) \
               $(MKSYMS_BIN)
	@mkdir -p $(dir $@)
	ld.lld -T $(KERNEL_LD) -nostdlib -static --emit-relocs -z max-page-size=0x1000 \
		--build-id=none --orphan-handling=error -o $@ \
		$(KERNEL_OBJECTS) $(KERNEL_BUILD)/kernel/arch/x86_64/ksyms.pass2.o
	$(MKSYMS_BIN) --check $@

.PHONY: kernel
kernel: branding $(KERNEL_ELF)
