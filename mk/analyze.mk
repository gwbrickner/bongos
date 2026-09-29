# make analyze: the Clang static analyzer over every kernel C source, with the current profile's
# kernel flags (in the default debug profile that includes UBSan's, so `#if KERNEL_UBSAN` code is
# analyzed; minus -Werror and dependency output). The report goes to build/analyze/report.txt.
# Exit status: 1 if clang failed on any file (that file went unanalyzed). Warnings are leads, not
# verdicts, so they only fail the target with ANALYZE_STRICT=1. Used by bug-sweeper,
# subsystem-hunter, and the milestone-sweep workflow (D-116); triage per docs/BUG_HUNTING.md §7.5.
ANALYZE_DIR := $(BUILD)/analyze
ANALYZE_CFLAGS := $(filter-out -Werror -MMD -MP,$(KERNEL_CFLAGS)) $(KERNEL_UBSAN_FLAGS) \
                  -Wno-unused-command-line-argument

.PHONY: analyze
analyze: branding $(CONSOLE_FONT_C)
	@mkdir -p $(ANALYZE_DIR)
	@: > $(ANALYZE_DIR)/report.txt; failed=0; \
	for f in $(KERNEL_C_SOURCES); do \
	    if ! $(KERNEL_CC) $(ANALYZE_CFLAGS) --analyze -Xanalyzer -analyzer-output=text \
	            -o /dev/null $$f >> $(ANALYZE_DIR)/report.txt 2>&1; then \
	        echo "analyze: clang failed on $$f (see the report)"; failed=$$((failed + 1)); \
	    fi; \
	done; \
	n=$$(grep -cE '^[^ ]+:[0-9]+:[0-9]+: warning:' $(ANALYZE_DIR)/report.txt); \
	e=$$(grep -cE '^[^ ]+:[0-9]+:[0-9]+: (fatal )?error:' $(ANALYZE_DIR)/report.txt); \
	echo "analyze: $$n warning(s), $$e error(s), $$failed failed file(s) across $(words $(KERNEL_C_SOURCES)) files; report: $(ANALYZE_DIR)/report.txt"; \
	[ "$$e" -eq 0 ] && [ "$$failed" -eq 0 ] || exit 1; \
	[ "$(ANALYZE_STRICT)" != 1 ] || [ "$$n" -eq 0 ]
