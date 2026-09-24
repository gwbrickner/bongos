# Generates build/include/branding.h from branding/{name,version,codenames.tsv} via
# tools/gen-branding.sh (ARCHITECTURE §0, D-046, D-052).
BRANDING_HDR := $(BUILD)/include/branding.h

.PHONY: branding
branding: $(BRANDING_HDR)

$(BRANDING_HDR): branding/name branding/version branding/codenames.tsv tools/gen-branding.sh
	@mkdir -p $(dir $@)
	bash tools/gen-branding.sh > $@
