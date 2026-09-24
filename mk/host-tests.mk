# make host-tests: builds and runs tests/host/ (ARCHITECTURE §23), a tiny assert-based runner
# with constructor-registered tests. Runs natively on the host, no cross toolchain needed.
HOST_CC ?= clang
HOST_TEST_DIR := tests/host
HOST_TEST_BUILD := $(BUILD)/host-tests
HOST_TEST_SRCS := $(shell find $(HOST_TEST_DIR) -name '*.c')
HOST_TEST_BIN := $(HOST_TEST_BUILD)/host-tests

.PHONY: host-tests
host-tests: $(HOST_TEST_BIN)
	$(HOST_TEST_BIN)

$(HOST_TEST_BIN): $(HOST_TEST_SRCS)
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c17 -Wall -Wextra -Werror -I$(HOST_TEST_DIR) -o $@ $(HOST_TEST_SRCS)
