# tools/imgdiff: compares/converts PPM and PNG screenshots for the GUI test harness (ARCHITECTURE
# §23, D-070). A host tool (own clang, no cross flags), per ARCHITECTURE §0's host-tool exception.
# Its own from-scratch PNG/zlib/DEFLATE codec, reusing tools/mkimage/crc32.c for the PNG chunk
# CRCs (same IEEE reflected polynomial).
IMGDIFF_DIR := $(BUILD)/tools/imgdiff
IMGDIFF_BIN := $(IMGDIFF_DIR)/imgdiff
IMGDIFF_SOURCES := $(wildcard tools/imgdiff/*.c) tools/mkimage/crc32.c

$(IMGDIFF_BIN): $(IMGDIFF_SOURCES) $(wildcard tools/imgdiff/*.h) tools/mkimage/crc32.h
	@mkdir -p $(dir $@)
	clang -std=c17 -Itools/mkimage -Wall -Wextra -Werror -O1 -o $@ $(IMGDIFF_SOURCES)

.PHONY: imgdiff
imgdiff: $(IMGDIFF_BIN)
