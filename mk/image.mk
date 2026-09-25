# make image -> build/bongos.img: cross-compiles the UEFI loader (boot/uefi/ + boot/common/),
# cross-compiles the kernel (mk/kernel.mk), builds tools/mkimage with the host toolchain, and runs
# it to produce a GPT disk image (ARCHITECTURE §5.1). Also builds build/bongos-ktest.img, the same
# image with tests/harness/ktest-boot.cfg baked in instead of boot/boot.cfg (mk/test.mk uses it).
# BIOS boot support (stage1/stage2) lands in M2.5; until then the BIOS boot partition is reserved
# but empty, and `run-bios`/matrix rows stay off.
IMAGE := $(BUILD)/bongos.img
KTEST_IMAGE := $(BUILD)/bongos-ktest.img
BOOT_UEFI_DIR := $(BUILD)/boot-uefi
MKIMAGE_DIR := $(BUILD)/tools/mkimage

# UEFI loader: PE32+ EFI application (ARCHITECTURE §3), boot/uefi/*.c plus the shared
# boot/common/*.c loader code (ELF64 loader, page-table builder, BootInfo builder, memory-map
# conversion, boot.cfg parser -- ARCHITECTURE §2), all built for the same x86_64-windows target.
UEFI_SOURCES := $(wildcard boot/uefi/*.c)
UEFI_OBJECTS := $(patsubst boot/uefi/%.c,$(BOOT_UEFI_DIR)/%.o,$(UEFI_SOURCES))
BOOT_COMMON_SOURCES := $(wildcard boot/common/*.c)
BOOT_COMMON_OBJECTS := $(patsubst boot/common/%.c,$(BOOT_UEFI_DIR)/common/%.o,$(BOOT_COMMON_SOURCES))
UEFI_TRAMPOLINE_OBJECT := $(BOOT_UEFI_DIR)/trampoline.o
UEFI_CC := clang
UEFI_AS := nasm
# -I$(BUILD)/include: branding.h. -Iboot/common/include: the shared BootInfo/ELF/paging/memmap/
# boot.cfg headers (ARCHITECTURE §2). -mno-stack-arg-probe: without it, any frame over 4 KiB (the
# page-table pool bookkeeping and the static memory-map buffers in handoff.c both get close)
# emits a call to `__chkstk`, which nothing on this freestanding target provides.
# -MMD -MP: so editing a header under boot/uefi/include/efi/, boot/common/include/, or
# branding.h triggers a rebuild of whatever included it, not just editing the .c file itself.
UEFI_CFLAGS := --target=x86_64-unknown-windows -std=c17 -ffreestanding -fshort-wchar \
               -mno-red-zone -mgeneral-regs-only -fno-stack-protector -mno-stack-arg-probe \
               -I$(BUILD)/include -Iboot/common/include -MMD -MP \
               -Wall -Wextra -Werror
UEFI_EFI := $(BOOT_UEFI_DIR)/BOOTX64.EFI

$(BOOT_UEFI_DIR)/%.o: boot/uefi/%.c $(BRANDING_HDR)
	@mkdir -p $(dir $@)
	$(UEFI_CC) $(UEFI_CFLAGS) -c -o $@ $<
$(BOOT_UEFI_DIR)/common/%.o: boot/common/%.c $(BRANDING_HDR)
	@mkdir -p $(dir $@)
	$(UEFI_CC) $(UEFI_CFLAGS) -c -o $@ $<
-include $(UEFI_OBJECTS:.o=.d) $(BOOT_COMMON_OBJECTS:.o=.d)

# The trampoline is assembled separately (nasm -f win64, i.e. COFF) rather than through the C
# compiler, same reasoning as kernel/arch/x86_64/entry.asm: the final CR3 swap can't be C (SDM
# Vol 3A §4.10.4.1's CR4.PGE toggle, and no C compiler will emit code that touches zero memory
# between `mov cr3` and `mov rsp`).
$(UEFI_TRAMPOLINE_OBJECT): boot/uefi/trampoline.asm
	@mkdir -p $(dir $@)
	$(UEFI_AS) -f win64 -o $@ $<

$(UEFI_EFI): $(UEFI_OBJECTS) $(BOOT_COMMON_OBJECTS) $(UEFI_TRAMPOLINE_OBJECT)
	lld-link /subsystem:efi_application /entry:efiMain /nodefaultlib /out:$@ $^

# tools/mkimage: a host tool (own clang, no cross flags), per ARCHITECTURE §0's host-tool
# exception. Also wants branding.h, for the root partition's on-disk name label, and
# boot/common's bootcfg.c/boot-status.c/bootmem.c to validate --boot-cfg at build time (D-067) --
# the same parser the loader itself runs, built here for the host instead of x86_64-windows.
MKIMAGE_SOURCES := tools/mkimage/main.c tools/mkimage/gpt.c tools/mkimage/crc32.c \
                   boot/common/bootcfg.c boot/common/boot-status.c boot/common/bootmem.c
MKIMAGE_BIN := $(MKIMAGE_DIR)/mkimage

$(MKIMAGE_BIN): $(MKIMAGE_SOURCES) tools/mkimage/gpt.h tools/mkimage/crc32.h $(BRANDING_HDR) \
                $(wildcard boot/common/include/*.h)
	@mkdir -p $(dir $@)
	clang -std=c17 -I$(BUILD)/include -Iboot/common/include -Wall -Wextra -Werror -O1 -o $@ \
		$(MKIMAGE_SOURCES)

# build/boot.cfg: boot/boot.cfg.in with @BRANDING_NAME@ substituted (D-067/D-046 -- the OS name
# lives only in branding/, so the checked-in template can't hardcode it).
BOOT_CFG_GENERATED := $(BUILD)/boot.cfg

$(BOOT_CFG_GENERATED): boot/boot.cfg.in branding/name tools/gen-boot-cfg.sh
	@mkdir -p $(dir $@)
	bash tools/gen-boot-cfg.sh > $@

.PHONY: image
image: branding $(UEFI_EFI) $(MKIMAGE_BIN) $(KERNEL_ELF) $(BOOT_CFG_GENERATED)
	$(MKIMAGE_BIN) --output $(IMAGE) --efi $(UEFI_EFI) --kernel $(KERNEL_ELF) \
		--boot-cfg $(BOOT_CFG_GENERATED)
	$(MKIMAGE_BIN) --output $(KTEST_IMAGE) --efi $(UEFI_EFI) --kernel $(KERNEL_ELF) \
		--boot-cfg tests/harness/ktest-boot.cfg
