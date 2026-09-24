# make test / make test-full: the boot matrix (ARCHITECTURE §23), run through
# tests/harness/run-matrix.sh, against build/bongos-ktest.img (mk/image.mk): the same image as
# `make image` but with tests/harness/ktest-boot.cfg baked in, so the kernel runs `ktest=all` and
# reports PASS/FAIL through the real isa-debug-exit/KTEST protocol (D-063) instead of the
# --expect-serial banner-match bridge M1.2 used before the kernel existed (D-057). The release/USB
# image (build/bongos.img) never runs ktests or writes port 0xF4.
.PHONY: test test-full _check-ktest-pass
test: image
	tests/harness/run-matrix.sh tests/harness/matrix.conf --image $(KTEST_IMAGE)
	@$(MAKE) --no-print-directory _check-ktest-pass MATRIX=tests/harness/matrix.conf

test-full: image
	tests/harness/run-matrix.sh tests/harness/matrix-full.conf --image $(KTEST_IMAGE)
	@$(MAKE) --no-print-directory _check-ktest-pass MATRIX=tests/harness/matrix-full.conf

# A QEMU exit code of 33 (run-qemu.sh) only proves *some* ktest passed -- if bootinfo_test.c were
# ever accidentally dropped, or bootinfo_valid renamed, `make test` would still report PASS on
# klog_format alone, silently losing the actual ROADMAP Done-when guarantee. So explicitly grep
# every configuration's serial log for the literal line the bootinfo_valid ktest prints on success,
# and fail loudly if it's missing from any of them.
_check-ktest-pass:
	@status=0; \
	while read -r fw cpus rest; do \
	    case "$$fw" in ''|\#*) continue ;; esac; \
	    name="$${fw}-$${cpus}cpu"; \
	    log="build/logs/$$name.serial.log"; \
	    if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qxF 'KTEST PASS bootinfo_valid'; then \
	        echo "make test: $$log does not contain 'KTEST PASS bootinfo_valid' (ROADMAP Done-when guarantee not met)"; \
	        status=1; \
	    fi; \
	done < "$(MATRIX)"; \
	exit $$status
