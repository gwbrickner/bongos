# The console font (ARCHITECTURE §5.2/§19, ROADMAP M1.4, D-069): tools/mkfont compiles
# data/fonts/console.txt (the '#'/'.' glyph-grid source, hand-composed and original -- see
# data/fonts/README.md) into the checked-in PSF2 binary data/fonts/console.psf, and separately
# into a generated C array (build/gen/console-font.c) that the kernel/loader link in directly
# (kernel/loader wiring itself lands later in M1.4, once fbcon exists).
MKFONT_DIR := $(BUILD)/tools/mkfont
MKFONT_BIN := $(MKFONT_DIR)/mkfont
FONT_SRC   := data/fonts/console.txt
FONT_PSF   := data/fonts/console.psf
CONSOLE_FONT_C := $(BUILD)/gen/console-font.c

$(MKFONT_BIN): tools/mkfont/main.c
	@mkdir -p $(dir $@)
	clang -std=c17 -Wall -Wextra -Werror -O1 -o $@ tools/mkfont/main.c

.PHONY: font font-check
font: $(MKFONT_BIN) $(FONT_SRC)
	$(MKFONT_BIN) psf $(FONT_SRC) $(FONT_PSF)

# Regenerates into build/ rather than touching the checked-in file, then diffs -- so a stale
# data/fonts/console.psf (edited by hand, or console.txt changed without regenerating it) fails
# CI the same way a misformatted source file does (this is wired into `make format-check`).
BUILD_FONT_PSF := $(BUILD)/font-check/console.psf
$(BUILD_FONT_PSF): $(MKFONT_BIN) $(FONT_SRC)
	@mkdir -p $(dir $@)
	$(MKFONT_BIN) psf $(FONT_SRC) $@

font-check: $(BUILD_FONT_PSF)
	@cmp -s $(BUILD_FONT_PSF) $(FONT_PSF) || { \
		echo "font-check: $(FONT_PSF) is stale -- run 'make font' and commit the result"; \
		exit 1; \
	}

$(CONSOLE_FONT_C): $(MKFONT_BIN) $(FONT_PSF)
	@mkdir -p $(dir $@)
	$(MKFONT_BIN) c fontConsolePsf $(FONT_PSF) $@
