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
HOST_TEST_EXTRA_SRCS := tools/mkimage/gpt.c tools/mkimage/crc32.c boot/uefi/guids.c
HOST_TEST_EXTRA_HDRS := tools/mkimage/gpt.h tools/mkimage/crc32.h $(wildcard boot/uefi/include/efi/*.h)
HOST_TEST_EXTRA_INCLUDES := -Itools/mkimage -Iboot/uefi

.PHONY: host-tests
host-tests: $(HOST_TEST_BIN)
	$(HOST_TEST_BIN)

$(HOST_TEST_BIN): $(HOST_TEST_SRCS) $(HOST_TEST_HDRS) $(HOST_TEST_EXTRA_SRCS) \
                  $(HOST_TEST_EXTRA_HDRS) $(BRANDING_HDR) Makefile mk/host-tests.mk
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
		-fno-sanitize-recover=all -I$(HOST_TEST_DIR) -I$(BUILD)/include \
		$(HOST_TEST_EXTRA_INCLUDES) -o $@ $(HOST_TEST_SRCS) $(HOST_TEST_EXTRA_SRCS)
