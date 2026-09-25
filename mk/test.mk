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

# A QEMU exit code of 33 (run-qemu.sh) only proves *some* ktest passed -- if bootinfo_test.c were
# ever accidentally dropped, or bootinfo_valid renamed, `make test` would still report PASS on
# klog_format alone, silently losing the actual ROADMAP Done-when guarantee. So explicitly grep
# every configuration's serial log for the literal line the bootinfo_valid ktest prints on success,
# and fail loudly if it's missing from any of them.
#
# Same reasoning for M2.1's other Done-when clause, "panic output includes a symbolized
# backtrace" (ROADMAP.md): trap_ud_caught passing only proves archTrapCatch redirected execution
# correctly, not that the printed report's backtrace was actually symbolized rather than a raw
# "?" -- so also grep for the exact frame #0 line trapPrintReport's backtracePrint call prints for
# that test (kernel/arch/x86_64/test/trap_test.c's trapUdTrigger, kernel/core/backtrace.c's
# printFrame format), confirmed against a real serial log before being pinned down here.
#
# Same reasoning again for M2.2's Done-when clauses (ROADMAP.md): exit 33 alone only proves *some*
# ktest passed, so if kernel/test/pmm_test.c ever got dropped from the build, a matrix row would
# still silently report PASS. Grep for each of its 4 required ktests (D-079..D-082) by name, plus
# the meminfo self-check's "OK" line (pmmPrintMeminfo(), kernel/mm/pmm.c) confirming the printed
# totals actually matched the BootInfo map rather than just having printed *something*.
PMM_REQUIRED_KTESTS := pmm_alloc_free_stress pmm_no_leak pmm_zone_correctness pmm_double_free
# Same reasoning again for M2.3's Done-when clauses (ROADMAP.md, D-086..D-090): the ktests proving
# W^X is enforced (not just printed) and that LOADER_RECLAIM was actually reclaimed.
PAGING_REQUIRED_KTESTS := paging_text_write_faults paging_data_exec_faults paging_fb_wc \
                         vmm_map_unmap loader_reclaimed
_check-ktest-pass:
	@status=0; \
	while read -r fw cpus mem; do \
	    case "$$fw" in ''|\#*) continue ;; esac; \
	    if [ -n "$$mem" ]; then name="$${fw}-$${cpus}cpu-$${mem}m"; else name="$${fw}-$${cpus}cpu"; fi; \
	    log="build/logs/$$name.serial.log"; \
	    if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qxF 'KTEST PASS bootinfo_valid'; then \
	        echo "make test: $$log does not contain 'KTEST PASS bootinfo_valid' (ROADMAP Done-when guarantee not met)"; \
	        status=1; \
	    fi; \
	    if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qE '^  #0 0x[0-9a-f]{16} trapUdTrigger\+0x'; then \
	        echo "make test: $$log does not contain a symbolized 'trapUdTrigger+0x...' backtrace frame (ROADMAP Done-when guarantee not met)"; \
	        status=1; \
	    fi; \
	    for t in $(PMM_REQUIRED_KTESTS); do \
	        if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qxF "KTEST PASS $$t"; then \
	            echo "make test: $$log does not contain 'KTEST PASS $$t' (ROADMAP M2.2 Done-when guarantee not met)"; \
	            status=1; \
	        fi; \
	    done; \
	    if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qxF '[info] meminfo: check: MemTotal + Reclaimed == MemManaged + PageArray + LowReserved + Unmapped: OK'; then \
	        echo "make test: $$log does not contain a passing meminfo self-check (ROADMAP M2.2 Done-when guarantee not met)"; \
	        status=1; \
	    fi; \
	    if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qE '^\[info\] vmm: W\^X verified: '; then \
	        echo "make test: $$log does not contain 'vmm: W^X verified: ...' (ROADMAP M2.3 Done-when guarantee not met)"; \
	        status=1; \
	    fi; \
	    for t in $(PAGING_REQUIRED_KTESTS); do \
	        if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qxF "KTEST PASS $$t"; then \
	            echo "make test: $$log does not contain 'KTEST PASS $$t' (ROADMAP M2.3 Done-when guarantee not met)"; \
	            status=1; \
	        fi; \
	    done; \
	done < "$(MATRIX)"; \
	exit $$status
