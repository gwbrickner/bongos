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
# logic living outside tests/host/, same reasoning as gpt.c/bootcfg.c below. kernel/drivers/fbcon/
# fbcon.c: pure C on top of fbtext.c's primitive (no hardware I/O, no locks yet), so it's just as
# host-testable as fbtext.c itself -- only needs kernel/include on the path for uapi/status.h.
# kernel/core/ksym.c: the KSYM v1 decoder (D-075) is deliberately pure (no sections.h/ksnprintf
# dependency -- that split lives in ksym-symbolize.c, kernel-only), so it host-tests the same way.
# tools/ksyms/ksyms-encode.c: the encoder, so ksyms_test.c can round-trip encode->decode without a
# real ELF file (elf-read.c itself isn't needed here -- only its header, for the ElfFuncSymList
# type the encoder's API takes).
# kernel/mm/pmm-map.c and kernel/mm/buddy.c: the pmm's two pure cores (D-079..D-082, ROADMAP
# M2.2) -- no klog/panic/arch calls, so they host-test the same way as the rest of this list.
# kernel/mm/kva.c: the KVA allocator's pure extent core (D-088, ROADMAP M2.3) -- same reasoning.
# -DHOSTED switches kernel/include/page.h's Page-array base to `hostPageArrayBase`
# (kernel_buddy_test.c), since host tests have no real HHDM/page-array VA region to point into.
HOST_TEST_EXTRA_SRCS := tools/mkimage/gpt.c tools/mkimage/crc32.c boot/uefi/guids.c \
                        $(wildcard boot/common/*.c) $(CONSOLE_FONT_C) \
                        $(filter-out tools/imgdiff/main.c,$(wildcard tools/imgdiff/*.c)) \
                        kernel/drivers/fbcon/fbcon.c kernel/core/ksym.c tools/ksyms/ksyms-encode.c \
                        kernel/mm/pmm-map.c kernel/mm/buddy.c kernel/mm/kva.c
HOST_TEST_EXTRA_HDRS := tools/mkimage/gpt.h tools/mkimage/crc32.h $(wildcard boot/uefi/include/efi/*.h) \
                        $(wildcard boot/common/include/*.h) $(wildcard tools/imgdiff/*.h) \
                        kernel/drivers/fbcon/fbcon.h $(wildcard kernel/include/uapi/*.h) \
                        kernel/include/ksym.h tools/ksyms/elf-read.h tools/ksyms/ksyms-encode.h \
                        kernel/include/list.h kernel/include/page.h kernel/include/pmm.h \
                        kernel/mm/pmm-internal.h kernel/mm/kva-internal.h
HOST_TEST_EXTRA_INCLUDES := -Itools/mkimage -Iboot/uefi -Iboot/common/include -Iboot/common \
                            -Itools/imgdiff -Ikernel/drivers/fbcon -Ikernel/include -Itools/ksyms \
                            -Ikernel/mm

.PHONY: host-tests
host-tests: $(HOST_TEST_BIN)
	$(HOST_TEST_BIN)

$(HOST_TEST_BIN): $(HOST_TEST_SRCS) $(HOST_TEST_HDRS) $(HOST_TEST_EXTRA_SRCS) \
                  $(HOST_TEST_EXTRA_HDRS) $(BRANDING_HDR) Makefile mk/host-tests.mk
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
		-fno-sanitize-recover=all -DHOSTED -I$(HOST_TEST_DIR) -I$(BUILD)/include \
		$(HOST_TEST_EXTRA_INCLUDES) -o $@ $(HOST_TEST_SRCS) $(HOST_TEST_EXTRA_SRCS)
