# tools/imgdiff: compares/converts PPM and PNG screenshots for the GUI test harness (ARCHITECTURE
# §23, D-070). A host tool (own clang, no cross flags), per ARCHITECTURE §0's host-tool exception.
# Its own from-scratch PNG reader/writer, on top of libs/compress (inflate/deflate/zlib and the
# CRC-32 for the PNG chunks; moved there from this directory in M12.2, D-140).
IMGDIFF_DIR := $(BUILD)/tools/imgdiff
IMGDIFF_BIN := $(IMGDIFF_DIR)/imgdiff
IMGDIFF_SOURCES := $(wildcard tools/imgdiff/*.c) $(wildcard libs/compress/*.c)

$(IMGDIFF_BIN): $(IMGDIFF_SOURCES) $(wildcard tools/imgdiff/*.h) $(wildcard libs/compress/*.h) \
                kernel/include/uapi/status.h
	@mkdir -p $(dir $@)
	clang -std=c17 -Ilibs -Ikernel/include -Wall -Wextra -Werror -O1 -o $@ $(IMGDIFF_SOURCES)

.PHONY: imgdiff
imgdiff: $(IMGDIFF_BIN)
