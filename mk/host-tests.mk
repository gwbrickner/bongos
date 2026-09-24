# make host-tests: builds and runs tests/host/ (ARCHITECTURE §23), a tiny assert-based runner
# with constructor-registered tests. Runs natively on the host, no cross toolchain needed.
HOST_CC ?= clang
HOST_TEST_DIR := tests/host
HOST_TEST_BUILD := $(BUILD)/host-tests
HOST_TEST_SRCS := $(sort $(shell find $(HOST_TEST_DIR) -name '*.c'))
HOST_TEST_HDRS := $(sort $(shell find $(HOST_TEST_DIR) -name '*.h'))
HOST_TEST_BIN := $(HOST_TEST_BUILD)/host-tests

.PHONY: host-tests
host-tests: $(HOST_TEST_BIN)
	$(HOST_TEST_BIN)

$(HOST_TEST_BIN): $(HOST_TEST_SRCS) $(HOST_TEST_HDRS) $(BRANDING_HDR)
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
		-fno-sanitize-recover=all -I$(HOST_TEST_DIR) -I$(BUILD)/include -o $@ $(HOST_TEST_SRCS)
