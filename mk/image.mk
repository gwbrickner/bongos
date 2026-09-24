# make image -> build/bongos.img. Until M1.2 adds tools/mkimage and the UEFI loader, there is
# nothing bootable to produce; this stays a clean no-op so `make image`/`make test` can be wired
# up now and start doing real work the moment M1.2 lands (per the M1.1 "Done when" note).
IMAGE := $(BUILD)/bongos.img

.PHONY: image
image: branding
	@if [ -f $(IMAGE) ]; then \
		echo "image: $(IMAGE) already built"; \
	else \
		echo "image: no bootable sources yet -- tools/mkimage and the UEFI loader arrive in M1.2 (see ROADMAP.md)"; \
	fi
