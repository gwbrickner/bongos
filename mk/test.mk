# make test / make test-full: the boot matrix (ARCHITECTURE §23), run through
# tests/harness/run-matrix.sh, against build/bongos-ktest.img (mk/image.mk): the same image as
# `make image` but with tests/harness/ktest-boot.cfg baked in, so the kernel runs `ktest=all` and
# reports PASS/FAIL through the real isa-debug-exit/KTEST protocol (D-063) instead of the
# --expect-serial banner-match bridge M1.2 used before the kernel existed (D-057). The release/USB
# image (build/bongos.img) never runs ktests or writes port 0xF4. `make test` also runs the M1.4
# GUI screenshot tests (D-070, tests/gui/run.sh) and a countdown smoke test against the shipped
# boot.cfg, on top of the ktest matrix.
.PHONY: test test-full _check-ktest-pass gui-test update-refs screenshot
test: image imgdiff kaslr-reloc-check
	tests/harness/run-matrix.sh tests/harness/matrix.conf --image $(KTEST_IMAGE)
	@$(MAKE) --no-print-directory _check-ktest-pass MATRIX=tests/harness/matrix.conf
	tests/gui/run.sh --fw uefi
	tests/gui/run.sh --fw bios
	tests/harness/run-qemu.sh --image $(IMAGE) --name countdown-smoke --timeout 30 \
		--expect-serial "loader: timeout, booting default" --expect-serial "kernel: init done"
	tests/harness/run-qemu.sh --fw bios --image $(IMAGE) --name countdown-smoke-bios --timeout 30 \
		--expect-serial "loader: timeout, booting default" --expect-serial "kernel: init done"
	tests/harness/kaslr-check.sh --fw uefi
	tests/harness/kaslr-check.sh --fw bios

test-full: image kaslr-reloc-check
	tests/harness/run-matrix.sh tests/harness/matrix-full.conf --image $(KTEST_IMAGE)
	@$(MAKE) --no-print-directory _check-ktest-pass MATRIX=tests/harness/matrix-full.conf
	tests/harness/kaslr-check.sh --fw uefi
	tests/harness/kaslr-check.sh --fw bios

# Re-runs the GUI tests alone (skips the ktest matrix and countdown smoke) -- useful while
# iterating on a screenshot test without waiting on the rest of `make test`. FW selects the
# firmware (default uefi); `make gui-test FW=bios` runs the BIOS path instead.
FW ?= uefi
gui-test: image imgdiff
	tests/gui/run.sh --fw $(FW)

# Boots the normal image (build/bongos.img, no ktests) and saves its final screen as a PNG (D-115).
# The finish protocol (CLAUDE.md) runs `make screenshot SHOT=docs/screenshots/M<p>.<n>.png`.
SHOT ?= build/shots/final.png
screenshot: image imgdiff
	tests/harness/screenshot.sh --fw $(FW) --out '$(SHOT)'

# Captures fresh GUI test reference PNGs. The caller must view every regenerated PNG and record
# why in the milestone log before committing (CLAUDE.md: regenerating a reference to turn a
# failing test green counts as weakening it). FW selects the firmware (default uefi); `make
# update-refs FW=bios` captures tests/gui/ref/bios-*.png instead.
update-refs: image imgdiff
	tests/gui/run.sh --fw $(FW) --update-refs

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
                         paging_wx_verify paging_text_hhdm_alias_readonly vmm_map_unmap \
                         loader_reclaimed
# M2.4's Done-when clauses (ROADMAP.md): stress, alignment, redzone overflow detected, a guard page
# write faults -- plus the rest of kmalloc_test.c / vmalloc_test.c. Same reasoning: exit 33 alone
# wouldn't notice a dropped test file.
SLAB_REQUIRED_KTESTS := kmalloc_stress kmalloc_alignment kmalloc_double_free slab_ctor_dtor \
                        vmalloc_guard_page_faults vmalloc_map_free_no_leak \
                        vmalloc_interior_free_rejected vmalloc_not_vmalloc_page_rejected \
                        vmalloc_oom_rollback
# These two only exist in debug builds (kmalloc_test.c compiles them under KERNEL_DEBUG), so a
# RELEASE=1 build can't be required to pass them; debug (the default, `make test`) still does.
ifneq ($(RELEASE),1)
SLAB_REQUIRED_KTESTS += kmalloc_redzone_overflow kmalloc_poison_detects_uaf
endif
# M2.6 (KASLR + kernel RNG): only the ktests that exist so far; later steps append their own
# (bootinfo_rejects_bad carries the kernel-window bound on kaslrSlide). Every matrix log must also
# show the kernel's kaslr line (kernelMain), so a kernel that stops reporting its slide cannot
# pass, and the slide header backtracePrint() puts before every backtrace (ROADMAP M2.6 item 3:
# panic output accounts for the slide; the trap ktests always print at least one backtrace), and
# the loader's own `kaslr: slide=` line: the matrix boots build/bongos-ktest.img, whose boot.cfg
# (tests/harness/ktest-boot.cfg) leaves KASLR at its default (on), so a loader that fell back
# ("kaslr: disabled") or stopped sliding fails here (tests/harness/kaslr-check.sh, run by `make
# test`/`test-full` after the matrix, checks the slide values across several boots and kaslr=off).
# The crypto primitives' vector ktests (kernel/test/crypto_test.c, vectors in
# libs/crypto/test/crypto-vectors.h) are required too: they are the CSPRNG's foundation, and so are
# the RNG's own ktests (kernel/test/random_test.c) plus the `random: seeded` log line every boot
# must print (kernelMain -> randomInit: a kernel that stops seeding its RNG cannot pass).
M26_REQUIRED_KTESTS := ksym_slide_accounted bootinfo_rejects_bad kaslr_slide_consistent \
                       kaslr_relocs_applied chacha20_rfc8439_block chacha20_rfc8439_encrypt \
                       sha256_fips180_vectors random_drbg_fast_key_erasure \
                       random_add_entropy_reseeds random_sanity \
                       random_drbg_key_erased_before_output random_boot_seed_wiped \
                       hwrandom_matches_cpuid random_boot_unique
# random_boot_unique prints the first output of the key randomInit derived; every matrix log must
# have exactly one such fingerprint and no two logs may share one, so an RNG that ignores the boot
# seed, the TSC and the hardware words (identical output every boot) cannot pass (M2.6 finish
# sweep: nothing else could see it, the global key is private).
_check-ktest-pass:
	@status=0; fps=""; \
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
	    if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qE '^\[info\] vmm: framebuffer 0x[0-9a-f]+ mapped WC in the HHDM$$'; then \
	        echo "make test: $$log does not contain 'vmm: framebuffer ... mapped WC ...' (ROADMAP M2.3 Done-when guarantee not met -- every harness boot has a framebuffer)"; \
	        status=1; \
	    fi; \
	    for t in $(PAGING_REQUIRED_KTESTS); do \
	        if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qxF "KTEST PASS $$t"; then \
	            echo "make test: $$log does not contain 'KTEST PASS $$t' (ROADMAP M2.3 Done-when guarantee not met)"; \
	            status=1; \
	        fi; \
	    done; \
	    for t in $(SLAB_REQUIRED_KTESTS); do \
	        if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qxF "KTEST PASS $$t"; then \
	            echo "make test: $$log does not contain 'KTEST PASS $$t' (ROADMAP M2.4 Done-when guarantee not met)"; \
	            status=1; \
	        fi; \
	    done; \
	    for t in $(M26_REQUIRED_KTESTS); do \
	        if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qxF "KTEST PASS $$t"; then \
	            echo "make test: $$log does not contain 'KTEST PASS $$t' (ROADMAP M2.6 Done-when guarantee not met)"; \
	            status=1; \
	        fi; \
	    done; \
	    if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qE '^\[info\] kaslr: virtBase=0x[0-9a-f]{16} slide=0x[0-9a-f]{16}$$'; then \
	        echo "make test: $$log does not contain the kernel's 'kaslr: virtBase=... slide=...' line (ROADMAP M2.6 Done-when guarantee not met)"; \
	        status=1; \
	    fi; \
	    if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qE '^loader: kaslr: slide=0x[0-9a-f]{16} base=0x[0-9a-f]{16} relocs=[0-9]+$$'; then \
	        echo "make test: $$log does not contain the loader's 'kaslr: slide=... base=... relocs=...' line (ROADMAP M2.6 Done-when guarantee not met; KASLR is on by default)"; \
	        status=1; \
	    fi; \
	    if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qE '^\[info\] random: seeded'; then \
	        echo "make test: $$log does not contain the kernel's 'random: seeded ...' line (ROADMAP M2.6 Done-when guarantee not met)"; \
	        status=1; \
	    fi; \
	    if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qE '^  kaslr slide 0x[0-9a-f]{16} \(link address = address - slide\)$$'; then \
	        echo "make test: $$log does not contain backtracePrint's 'kaslr slide 0x...' header (ROADMAP M2.6 Done-when guarantee not met)"; \
	        status=1; \
	    fi; \
	    fp=$$(tr -d '\r' < "$$log" 2>/dev/null | sed -n 's/^\[info\] random: ktest boot fingerprint 0x\([0-9a-f]\{16\}\)$$/\1/p'); \
	    if [ "$$(printf '%s\n' "$$fp" | grep -c .)" != 1 ]; then \
	        echo "make test: $$log does not contain exactly one 'random: ktest boot fingerprint 0x...' line (ROADMAP M2.6 Done-when guarantee not met)"; \
	        status=1; \
	    else \
	        fps="$$fps $$fp"; \
	    fi; \
	done < "$(MATRIX)"; \
	dups=$$(printf '%s\n' $$fps | sort | uniq -d); \
	if [ -n "$$dups" ]; then \
	    echo "make test: two matrix boots printed the same RNG fingerprint ($$dups): the kernel RNG is not seeded from per-boot entropy (ROADMAP M2.6 Done-when guarantee not met)"; \
	    status=1; \
	fi; \
	exit $$status
