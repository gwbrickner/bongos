# make image -> build/bongos.img. Until M1.2 adds tools/mkimage and the UEFI loader, there is
# nothing bootable to produce; this stays a clean no-op so `make image`/`make test` can be wired
# up now and start doing real work the moment M1.2 lands (D-053). Once loader sources exist,
# this fragment must be replaced with a real build rule rather than silently no-op forever --
# the $(error) below catches that.
IMAGE := $(BUILD)/bongos.img
BOOT_SOURCES := $(wildcard boot/uefi/*.c boot/common/*.c)

.PHONY: image
image: branding
ifeq ($(strip $(BOOT_SOURCES)),)
	@echo "image: no bootable sources yet -- tools/mkimage and the UEFI loader arrive in M1.2 (see ROADMAP.md)"
else
	$(error mk/image.mk has no build rule for the sources now under boot/ -- update this fragment (M1.2))
endif
