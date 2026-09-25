# make host-tests: builds and runs tests/host/ (ARCHITECTURE §23), a tiny assert-based runner
# with constructor-registered tests. Runs natively on the host, no cross toolchain needed.
HOST_CC ?= clang
HOST_TEST_DIR := tests/host
HOST_TEST_BUILD := $(BUILD)/host-tests
HOST_TEST_SRCS := $(sort $(shell find $(HOST_TEST_DIR) -name '*.c'))
HOST_TEST_HDRS := $(sort $(shell find $(HOST_TEST_DIR) -name '*.h'))
HOST_TEST_BIN := $(HOST_TEST_BUILD)/host-tests

# Host-testable implementation code that lives with its own tool/subsystem rather than under
# tests/host/ itself (only the TEST() cases go there) -- grows as more of them get host tests
# (parsers, bongfs, crypto...). Their own directories are added to the include path so a test
# file can '#include "gpt.h"' without a relative path. boot/uefi/guids.c has no freestanding- or
# cross-target-specific code, so it builds fine with the host's native clang too.
# $(CONSOLE_FONT_C) (mk/font.mk): fbtext.c links against the generated font data (fontConsolePsf),
# not a wildcard match under boot/common/*.c, so it's listed explicitly; Make builds it first since
# it's its own target with its own rule. tools/imgdiff/*.c minus main.c (which defines its own
# `main`, conflicting with tests/host/main.c's): imgdiff's PPM/PNG/DEFLATE codec is host-testable
# logic living outside tests/host/, same reasoning as gpt.c/bootcfg.c below.
HOST_TEST_EXTRA_SRCS := tools/mkimage/gpt.c tools/mkimage/crc32.c boot/uefi/guids.c \
                        $(wildcard boot/common/*.c) $(CONSOLE_FONT_C) \
                        $(filter-out tools/imgdiff/main.c,$(wildcard tools/imgdiff/*.c))
HOST_TEST_EXTRA_HDRS := tools/mkimage/gpt.h tools/mkimage/crc32.h $(wildcard boot/uefi/include/efi/*.h) \
                        $(wildcard boot/common/include/*.h) $(wildcard tools/imgdiff/*.h)
HOST_TEST_EXTRA_INCLUDES := -Itools/mkimage -Iboot/uefi -Iboot/common/include -Iboot/common \
                            -Itools/imgdiff

.PHONY: host-tests
host-tests: $(HOST_TEST_BIN)
	$(HOST_TEST_BIN)

$(HOST_TEST_BIN): $(HOST_TEST_SRCS) $(HOST_TEST_HDRS) $(HOST_TEST_EXTRA_SRCS) \
                  $(HOST_TEST_EXTRA_HDRS) $(BRANDING_HDR) Makefile mk/host-tests.mk
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
		-fno-sanitize-recover=all -I$(HOST_TEST_DIR) -I$(BUILD)/include \
		$(HOST_TEST_EXTRA_INCLUDES) -o $@ $(HOST_TEST_SRCS) $(HOST_TEST_EXTRA_SRCS)
