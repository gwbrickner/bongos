# make image -> build/bongos.img: cross-compiles the UEFI loader (boot/uefi/), builds
# tools/mkimage with the host toolchain, and runs it to produce a GPT disk image
# (ARCHITECTURE §5.1). BIOS boot support (stage1/stage2) lands in M2.5; until then the BIOS boot
# partition is reserved but empty, and `run-bios`/matrix rows stay off.
IMAGE := $(BUILD)/bongos.img
BOOT_UEFI_DIR := $(BUILD)/boot-uefi
MKIMAGE_DIR := $(BUILD)/tools/mkimage

# UEFI loader: PE32+ EFI application (ARCHITECTURE §3).
UEFI_SOURCES := $(wildcard boot/uefi/*.c)
UEFI_OBJECTS := $(patsubst boot/uefi/%.c,$(BOOT_UEFI_DIR)/%.o,$(UEFI_SOURCES))
UEFI_CC := clang
# -I$(BUILD)/include: branding.h. -MMD -MP: so editing a header under boot/uefi/include/efi/ (or
# branding.h) triggers a rebuild of whatever included it, not just editing the .c file itself.
UEFI_CFLAGS := --target=x86_64-unknown-windows -std=c17 -ffreestanding -fshort-wchar \
               -mno-red-zone -mgeneral-regs-only -fno-stack-protector \
               -I$(BUILD)/include -MMD -MP \
               -Wall -Wextra -Werror
UEFI_EFI := $(BOOT_UEFI_DIR)/BOOTX64.EFI

$(BOOT_UEFI_DIR)/%.o: boot/uefi/%.c $(BRANDING_HDR)
	@mkdir -p $(dir $@)
	$(UEFI_CC) $(UEFI_CFLAGS) -c -o $@ $<
-include $(UEFI_OBJECTS:.o=.d)

$(UEFI_EFI): $(UEFI_OBJECTS)
	lld-link /subsystem:efi_application /entry:efiMain /nodefaultlib /out:$@ $(UEFI_OBJECTS)

# tools/mkimage: a host tool (own clang, no cross flags), per ARCHITECTURE §0's host-tool
# exception. Also wants branding.h, for the root partition's on-disk name label.
MKIMAGE_SOURCES := tools/mkimage/main.c tools/mkimage/gpt.c tools/mkimage/crc32.c
MKIMAGE_BIN := $(MKIMAGE_DIR)/mkimage

$(MKIMAGE_BIN): $(MKIMAGE_SOURCES) tools/mkimage/gpt.h tools/mkimage/crc32.h $(BRANDING_HDR)
	@mkdir -p $(dir $@)
	clang -std=c17 -I$(BUILD)/include -Wall -Wextra -Werror -O1 -o $@ $(MKIMAGE_SOURCES)

.PHONY: image
image: branding $(UEFI_EFI) $(MKIMAGE_BIN)
	$(MKIMAGE_BIN) --output $(IMAGE) --efi $(UEFI_EFI)
