# `make format` / `make format-check`, using the repo's .clang-format. Tolerant of there being
# no C sources yet (true until M1.3); starts doing real work the moment a *.c/*.h file exists.
CLANG_FORMAT ?= clang-format
FORMAT_SRC_DIRS := boot kernel libs system apps cmds tools \
                   tests/host tests/user tests/fs tests/net tests/gui tests/audio
FORMAT_FILES := $(shell find $(FORMAT_SRC_DIRS) -type f \( -name '*.c' -o -name '*.h' \) 2>/dev/null)

.PHONY: format format-check
format:
	@if [ -n "$(FORMAT_FILES)" ]; then \
		$(CLANG_FORMAT) -i $(FORMAT_FILES); \
	else \
		echo "format: no C sources yet"; \
	fi

format-check:
	@if [ -n "$(FORMAT_FILES)" ]; then \
		$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES); \
	else \
		echo "format-check: no C sources yet (nothing to check)"; \
	fi
