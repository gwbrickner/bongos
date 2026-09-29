# make analyze: the Clang static analyzer over every kernel C source, with the kernel's own flags
# (minus -Werror and dependency output), so it sees exactly the code the build compiles. Reports
# go to build/analyze/report.txt. It exits 0 whatever it finds (it reports the count) unless
# ANALYZE_STRICT=1. Used by
# the bug-sweeper and subsystem-hunter agents and the milestone-sweep workflow (D-100). Findings
# are leads, not verdicts: triage each one (real bug / false positive) per docs/BUG_HUNTING.md §7.
ANALYZE_DIR := $(BUILD)/analyze
ANALYZE_CFLAGS := $(filter-out -Werror -MMD -MP,$(KERNEL_CFLAGS)) -Wno-unused-command-line-argument

.PHONY: analyze
analyze: branding $(CONSOLE_FONT_C)
	@mkdir -p $(ANALYZE_DIR)
	@: > $(ANALYZE_DIR)/report.txt; \
	for f in $(KERNEL_C_SOURCES); do \
	    $(KERNEL_CC) $(ANALYZE_CFLAGS) --analyze -Xanalyzer -analyzer-output=text \
	        -o /dev/null $$f >> $(ANALYZE_DIR)/report.txt 2>&1; \
	done; \
	n=$$(grep -c 'warning:' $(ANALYZE_DIR)/report.txt); \
	echo "analyze: $$n warning(s) across $(words $(KERNEL_C_SOURCES)) files; report: $(ANALYZE_DIR)/report.txt"; \
	[ "$(ANALYZE_STRICT)" != 1 ] || [ "$$n" -eq 0 ]
