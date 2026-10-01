# tools/acpiextract: turns an ACPIDUMP v1 serial log into table files, and checks logs (M3.1, D-169,
# docs/specs/acpidump.md). A host tool (own clang, no cross flags), per ARCHITECTURE §0's host-tool
# exception. `make test` runs its --check mode on every matrix log.
ACPIEXTRACT_DIR := $(BUILD)/tools/acpiextract
ACPIEXTRACT_BIN := $(ACPIEXTRACT_DIR)/acpiextract

$(ACPIEXTRACT_BIN): $(wildcard tools/acpiextract/*.c) $(wildcard tools/acpiextract/*.h)
	@mkdir -p $(dir $@)
	clang -std=c17 -Wall -Wextra -Werror -O1 -o $@ $(wildcard tools/acpiextract/*.c)

.PHONY: acpiextract
acpiextract: $(ACPIEXTRACT_BIN)
