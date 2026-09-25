# make test / make test-full: the boot matrix (ARCHITECTURE §23), run through
# tests/harness/run-matrix.sh, against build/bongos-ktest.img (mk/image.mk): the same image as
# `make image` but with tests/harness/ktest-boot.cfg baked in, so the kernel runs `ktest=all` and
# reports PASS/FAIL through the real isa-debug-exit/KTEST protocol (D-063) instead of the
# --expect-serial banner-match bridge M1.2 used before the kernel existed (D-057). The release/USB
# image (build/bongos.img) never runs ktests or writes port 0xF4. `make test` also runs the M1.4
# GUI screenshot tests (D-070, tests/gui/run.sh) and a countdown smoke test against the shipped
# boot.cfg, on top of the ktest matrix.
.PHONY: test test-full _check-ktest-pass gui-test update-refs
test: image imgdiff
	tests/harness/run-matrix.sh tests/harness/matrix.conf --image $(KTEST_IMAGE)
	@$(MAKE) --no-print-directory _check-ktest-pass MATRIX=tests/harness/matrix.conf
	tests/gui/run.sh --fw uefi
	tests/harness/run-qemu.sh --image $(IMAGE) --name countdown-smoke --timeout 30 \
		--expect-serial "loader: timeout, booting default" --expect-serial "kernel: init done"

test-full: image
	tests/harness/run-matrix.sh tests/harness/matrix-full.conf --image $(KTEST_IMAGE)
	@$(MAKE) --no-print-directory _check-ktest-pass MATRIX=tests/harness/matrix-full.conf

# Re-runs the GUI tests alone (skips the ktest matrix and countdown smoke) -- useful while
# iterating on a screenshot test without waiting on the rest of `make test`.
gui-test: image imgdiff
	tests/gui/run.sh --fw uefi

# Captures fresh GUI test reference PNGs. The caller must view every regenerated PNG and record
# why in the milestone log before committing (CLAUDE.md: regenerating a reference to turn a
# failing test green counts as weakening it).
update-refs: image imgdiff
	tests/gui/run.sh --fw uefi --update-refs

# A QEMU exit code of 33 (run-qemu.sh) only proves *some* ktest passed -- if a test file were ever
# accidentally dropped, or a test renamed, `make test` would still report PASS on whatever's left,
# silently losing the actual ROADMAP Done-when guarantee. So explicitly grep every configuration's
# serial log for the literal PASS line of every ktest a Done-when clause depends on, and fail
# loudly if any is missing. M2.1 (D-072/D-073/D-074/D-075/D-076) adds the GDT/TSS/IDT, symbolize,
# and hardening tests to this list.
KTEST_REQUIRED := bootinfo_valid cpu_tables_loaded trap_int3_resumes trap_ud_caught \
                  trap_pf_reports_cr2 trap_df_on_ist1 stack_smash_detected \
                  ubsan_overflow_detected symbolize_known_function backtrace_walks_chain

_check-ktest-pass:
	@status=0; \
	while read -r fw cpus rest; do \
	    case "$$fw" in ''|\#*) continue ;; esac; \
	    name="$${fw}-$${cpus}cpu"; \
	    log="build/logs/$$name.serial.log"; \
	    clean="$$(tr -d '\r' < "$$log" 2>/dev/null)"; \
	    for t in $(KTEST_REQUIRED); do \
	        if ! printf '%s\n' "$$clean" | grep -qxF "KTEST PASS $$t"; then \
	            echo "make test: $$log does not contain 'KTEST PASS $$t' (ROADMAP Done-when guarantee not met)"; \
	            status=1; \
	        fi; \
	    done; \
	    if ! printf '%s\n' "$$clean" | grep -qE '^  #[0-9]+ 0x[0-9a-f]{16} ktestSmashVictim\+0x[0-9a-f]+/0x[0-9a-f]+$$'; then \
	        echo "make test: $$log's stack_smash_detected panic has no symbolized 'ktestSmashVictim' backtrace frame (ROADMAP M2.1 Done-when guarantee not met)"; \
	        status=1; \
	    fi; \
	done < "$(MATRIX)"; \
	exit $$status
