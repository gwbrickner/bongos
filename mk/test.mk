# make test / make test-full: the boot matrix (ARCHITECTURE §23), run through
# tests/harness/run-matrix.sh, against build/bongos-ktest.img (mk/image.mk): the same image as
# `make image` but with tests/harness/ktest-boot.cfg baked in, so the kernel runs `ktest=all` and
# reports PASS/FAIL through the real isa-debug-exit/KTEST protocol (D-063) instead of the
# --expect-serial banner-match bridge M1.2 used before the kernel existed (D-057). The release/USB
# image (build/bongos.img) never runs ktests or writes port 0xF4. `make test` also runs the M1.4
# GUI screenshot tests (D-070, tests/gui/run.sh) and a countdown smoke test against the shipped
# boot.cfg, on top of the ktest matrix.
.PHONY: test test-full _check-ktest-pass gui-test update-refs screenshot
test: image imgdiff acpiextract kaslr-reloc-check
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

test-full: image acpiextract kaslr-reloc-check
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
# M2.6 (KASLR + kernel RNG): the ktests its Done-when clauses rest on
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
                       hwrandom_matches_cpuid random_get_bytes_long_request
# M3.1 (ACPI tables): the ktests its Done-when clauses rest on, plus four log checks every matrix
# row must pass: the MCFG base line, one `MADT cpu` line per vCPU in the row (the roadmap's "boot log
# lists the CPUs from the MADT"; only matrix-full.conf's 4-CPU rows can tell "every CPU" from "the
# first CPU", until M3.5 adds 4-CPU rows to matrix.conf), `acpiextract --check` on the ACPIDUMP block
# the ktest image's `acpidump=1` makes the kernel print (docs/specs/acpidump.md), and the logged MCFG
# base equal to the first base in the dumped MCFG table (bytes 44..51, little-endian), so a log line
# with the right format but a wrong value cannot pass.
ACPI_REQUIRED_KTESTS := acpi_reclaimed pmm_reclaim_keeps_low_memory acpi_tables_loaded acpi_madt_lists_bsp acpi_mcfg_present acpi_fadt_sane \
                        acpi_tables_are_kernel_copies acpi_parse_rejects_corrupt \
                        acpi_read_phys_matches_copies acpi_read_phys_releases_kva \
                        acpi_read_phys_refuses_bad_ranges acpi_alloc_free_boundary \
                        acpi_reload_matches_and_frees acpi_read_phys_offsets_exact
# M3.2 (interrupt controllers): the ktests its Done-when clause rests on, the controller init log lines
# every row must show, and a negative check: the only irq/lapic/ioapic warning or error a clean boot
# may print is the single, expected "unhandled vector" line from irq_unhandled_vector_eoi (a second
# one would mean a stray or stale interrupt reached the kernel).
IRQ_REQUIRED_KTESTS := vmm_map_uc vmm_mmio_edges vmm_mmio_unmap_misuse paging_mmio_uc_pat irq_pic_masked irq_legacy_spurious_counted irq_lapic_state \
                       irq_vector_alloc_exhaust irq_register_rules irq_self_ipi_delivered \
                       irq_self_ipi_pending_while_if0 irq_spurious_vector_no_eoi \
                       irq_unhandled_vector_eoi irq_ioapic_masked_at_init \
                       irq_isa_route_matches_madt irq_ioapic_pit_routed \
                       irq_api_refused_in_handler irq_self_ipi_from_handler_redelivered \
                       irq_two_pending_vectors_priority_order irq_fixed_vectors_dispatch \
                       irq_route_misuse irq_isa_route_all_match_madt \
                       irq_trap_catch_refused_in_handler trap_catch_restores_if \
                       irq_lapic_reinit_neutralizes_leftovers
# M3.3 (timekeeping): the ktests its Done-when clause rests on (monotonic across 1M reads, the 100 ms
# one-shot timer, the wall clock against the RTC), the queue/clock/policy ktests, the calibration and
# RTC log lines every row must show, a negative check (a clean QEMU boot prints no `time:` warning or error;
# the "TSC is not invariant" note under a hypervisor is info-level, D-178) and the wall clock against the host
# (D-181): tests/harness/run-qemu.sh stamps the host's clock onto the kernel's `time: wall-check`
# line (build/logs/<row>.walltime) and the two must agree within 2 s.
TIME_REQUIRED_KTESTS := time_monotonic_1m_reads time_oneshot_100ms time_wall_matches_rtc \
                        time_timer_fires_once time_timer_cancel time_timer_rearm \
                        time_timer_periodic_from_callback time_timer_past_deadline_fires \
                        time_timer_order time_timer_misuse time_tsc_invariance_matches_cpuid \
                        time_tsc_matches_pmtimer time_hpet_calibration_agrees \
                        time_lapic_timer_mode_matches_cpuid \
                        time_timer_batch_sibling_rearm_cancel time_timer_batch_reinit_sibling \
                        time_timer_past_rearm_once_per_irq \
                        time_timer_far_deadlines time_timer_full_leaves_timer_unchanged \
                        time_timer_full_queue_one_batch time_timer_callback_cancels_self \
                        time_timer_init_misuse_panics time_timer_arm_from_other_irq \
                        time_timer_hw_tracks_root time_lapic_oneshot_count_edges
TIME_LOG_REGEX_FILE := tests/harness/time-log-regexes.txt
# M3.4 (D-183..D-188): the spinlock/preempt ktests run in both profiles; the lock validator exists
# (and so do its ktests) only in debug builds.
LOCK_REQUIRED_KTESTS := spin_trylock_semantics spin_irqsave_restores_if preempt_count_balance \
                        spin_unlock_unlocked_caught spin_ticket_wraparound \
                        spin_unlock_irqrestore_unlocked_caught spin_assert_held \
                        preempt_in_atomic_irqs_off klog_exception_in_section_does_not_hang
# M3.5 (SMP bring-up): grows as the sub-steps land.
SMP_REQUIRED_KTESTS := smp_cpulocal_bsp smp_online_matches_madt smp_call_function_all_cpus \
                       irq_handler_may_allocate \
                       smp_call_function_refused_with_irqs_off smp_tlb_shootdown_batched \
                       smp_stop_parks_cpus smp_gs_tables_per_cpu smp_cpu_state_matches_bsp \
                       smp_call_function_storm smp_tlb_shootdown_stress smp_pmm_concurrent_stress \
                       smp_cross_cpu_double_free_caught smp_ap_timer_fires_locally \
                       smp_tsc_monotonic smp_tsc_estimator_recovers_skew smp_stop_nmi_fallback
LOCKDEP_REQUIRED_KTESTS := lockdep_inversion_reported lockdep_irq_safe_unsafe_reported \
                           lockdep_expect_per_cpu lockdep_irq_unsafe_in_irq_reported \
                           lockdep_irq_safe_then_irqs_on_reported lockdep_class_recursion_reported \
                           lockdep_trylock_records_no_edge lockdep_out_of_order_release \
                           lockdep_sees_kernel_locks lockdep_not_held_reported \
                           lockdep_irqs_enabled_while_held_reported \
                           lockdep_transitive_inversion_reported lockdep_irq_segment_records_no_edge \
                           lockdep_trylock_same_class_allowed \
                           lockdep_trylock_irqs_on_then_irq_reported lockdep_repeat_order_adds_nothing \
                           lockdep_spininit_stack_locks lockdep_kernel_lock_order \
                           lockdep_expected_report_records_no_edge \
                           lockdep_disabled_release_drops_held_entry
_check-ktest-pass: $(ACPIEXTRACT_BIN)
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
	    for t in $(IRQ_REQUIRED_KTESTS); do \
	        if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qxF "KTEST PASS $$t"; then \
	            echo "make test: $$log does not contain 'KTEST PASS $$t' (ROADMAP M3.2 Done-when guarantee not met)"; \
	            status=1; \
	        fi; \
	    done; \
	    if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qE '^\[info\] irq: 8259 remapped to 0x20/0x28 and masked \(pcat=[01]\)$$'; then \
	        echo "make test: $$log does not contain the '8259 remapped ... and masked' line (ROADMAP M3.2 item 1)"; \
	        status=1; \
	    fi; \
	    if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qE '^\[info\] lapic: mode=(x2apic|xapic) id=[0-9]+ version=0x[0-9a-f]{2} maxlvt=[0-9]+ base=0x[0-9a-f]+$$'; then \
	        echo "make test: $$log does not contain the 'lapic: mode=...' line (ROADMAP M3.2 item 2)"; \
	        status=1; \
	    fi; \
	    if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qE '^\[info\] ioapic: id=[0-9]+ addr=0x[0-9a-f]{8} version=0x[0-9a-f]{2} pins=[0-9]+ gsi=[0-9]+-[0-9]+$$'; then \
	        echo "make test: $$log does not contain an 'ioapic: id=...' line (ROADMAP M3.2 item 3)"; \
	        status=1; \
	    fi; \
	    if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qxF '[info] irq: interrupts enabled'; then \
	        echo "make test: $$log does not contain 'irq: interrupts enabled'"; \
	        status=1; \
	    fi; \
	    nunh=$$(tr -d '\r' < "$$log" 2>/dev/null | grep -cE '^\[(warn|error)\] (irq|lapic|ioapic): '); \
	    nunhExpected=$$(tr -d '\r' < "$$log" 2>/dev/null | grep -cE '^\[warn\] irq: unhandled vector [0-9]+ '); \
	    if [ "$$nunh" != 1 ] || [ "$$nunhExpected" != 1 ]; then \
	        echo "make test: $$log has $$nunh irq/lapic/ioapic warnings or errors ($$nunhExpected of them 'unhandled vector'); exactly one, the expected 'unhandled vector', is allowed"; \
	        status=1; \
	    fi; \
	    for t in $(TIME_REQUIRED_KTESTS); do \
	        if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qxF "KTEST PASS $$t"; then \
	            echo "make test: $$log does not contain 'KTEST PASS $$t' (ROADMAP M3.3 Done-when guarantee not met)"; \
	            status=1; \
	        fi; \
	    done; \
	    while read -r re; do \
	        case "$$re" in ''|\#*) continue ;; esac; \
	        if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qE "^\[info\] time: $$re"; then \
	            echo "make test: $$log has no 'time: $$re' line (ROADMAP M3.3 items 1-4)"; \
	            status=1; \
	        fi; \
	    done < $(TIME_LOG_REGEX_FILE); \
	    ntw=$$(tr -d '\r' < "$$log" 2>/dev/null | grep -cE '^\[(warn|error)\] time: '); \
	    if [ "$$ntw" != 0 ]; then \
	        echo "make test: $$log has $$ntw 'time:' warnings or errors; a clean QEMU boot has none"; \
	        status=1; \
	    fi; \
	    wt="build/logs/$$name.walltime"; \
	    if [ ! -s "$$wt" ]; then \
	        echo "make test: $$wt is missing: run-qemu.sh saw no 'time: wall-check' line, so the wall clock was never compared with the host (ROADMAP M3.3 item 5)"; \
	        status=1; \
	    else \
	        if ! awk '{ split($$NF, a, "="); d = $$1 - a[2]; if (d < 0) d = -d; printf "make test: %s: wall clock vs host: %.3f s\n", FILENAME, d; exit !(d <= 2.0) }' "$$wt"; then \
	            echo "make test: $$wt: the guest wall clock differs from the host's by more than 2 s (ROADMAP M3.3 item 5)"; \
	            status=1; \
	        fi; \
	    fi; \
	    for t in $(SMP_REQUIRED_KTESTS); do \
	        if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qxF "KTEST PASS $$t"; then \
	            echo "make test: $$log does not contain 'KTEST PASS $$t' (ROADMAP M3.5 Done-when guarantee not met)"; \
	            status=1; \
	        fi; \
	    done; \
	    if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qxE "\[info\] smp: $$cpus cpus online"; then \
	        echo "make test: $$log does not contain '[info] smp: $$cpus cpus online' (ROADMAP M3.5: every vCPU must come up)"; \
	        status=1; \
	    fi; \
	    nap=$$(tr -d '\r' < "$$log" 2>/dev/null | grep -cE '^\[info\] smp: cpu [0-9]+ apic-id=[0-9]+ online$$'); \
	    if [ "$$nap" != "$$((cpus - 1))" ]; then \
	        echo "make test: $$log has $$nap 'smp: cpu N apic-id=A online' lines, expected $$((cpus - 1)) (ROADMAP M3.5)"; \
	        status=1; \
	    fi; \
	    if [ "$$cpus" -gt 1 ]; then \
	        for re in '^\[info\] smp: tlb-stress readers='"$$((cpus - 1))"' flips=[0-9]+ reads=[0-9]+ bad=0$$' \
	                  '^\[info\] smp: pmm-stress cpus='"$$cpus"' ops=[0-9]+ bad=0$$'; do \
	            if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qE "$$re"; then \
	                echo "make test: $$log has no line matching '$$re' (ROADMAP M3.5 item 7: the stress ktests must have run on every CPU)"; \
	                status=1; \
	            fi; \
	        done; \
	    fi; \
	    nsw=$$(tr -d '\r' < "$$log" 2>/dev/null | grep -cE '^\[(warn|error)\] smp:'); \
	    if [ "$$nsw" != 0 ]; then \
	        echo "make test: $$log has $$nsw smp warn/error lines (ROADMAP M3.5: AP bring-up must be clean)"; \
	        status=1; \
	    fi; \
	    for t in $(LOCK_REQUIRED_KTESTS); do \
	        if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qxF "KTEST PASS $$t"; then \
	            echo "make test: $$log does not contain 'KTEST PASS $$t' (ROADMAP M3.4 Done-when guarantee not met)"; \
	            status=1; \
	        fi; \
	    done; \
	    if [ "$(RELEASE)" != 1 ]; then \
	        for t in $(LOCKDEP_REQUIRED_KTESTS); do \
	            if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qxF "KTEST PASS $$t"; then \
	                echo "make test: $$log does not contain 'KTEST PASS $$t' (ROADMAP M3.4 Done-when guarantee not met)"; \
	                status=1; \
	            fi; \
	        done; \
	        for re in '^LOCKDEP \(expected by ktest\): lock order inversion$$' \
	                  '^LOCKDEP \(expected by ktest\): inconsistent IRQ lock state$$' \
	                  '^LOCKDEP \(expected by ktest\): recursive locking$$' \
	                  '^LOCKDEP \(expected by ktest\): IRQ-safe lock reaches an IRQ-unsafe lock$$' \
	                  '^  #[0-9]+ 0x[0-9a-f]{16} lockdepTestTakeAB\+0x' \
	                  '^  #[0-9]+ 0x[0-9a-f]{16} lockdepTestTakeBA\+0x' \
	                  '^  #[0-9]+ 0x[0-9a-f]{16} lockdepTestIrqCallback\+0x' \
	                  '^\[info\] lockdep: summary: enabled=1 classes=[0-9]+ edges=[0-9]+ expected-reports=[0-9]+$$'; do \
	            if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qE "$$re"; then \
	                echo "make test: $$log has no line matching '$$re' (ROADMAP M3.4 items 2-3: the validator report with both stacks)"; \
	                status=1; \
	            fi; \
	        done; \
	        nld=$$(tr -d '\r' < "$$log" 2>/dev/null | grep -cE '^LOCKDEP: |^\[(warn|error)\] lockdep: '); \
	        if [ "$$nld" != 0 ]; then \
	            echo "make test: $$log has $$nld unexpected LOCKDEP report/warning lines (ROADMAP M3.4 Done-when: no false positives)"; \
	            status=1; \
	        fi; \
	    fi; \
	    for t in $(ACPI_REQUIRED_KTESTS); do \
	        if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qxF "KTEST PASS $$t"; then \
	            echo "make test: $$log does not contain 'KTEST PASS $$t' (ROADMAP M3.1 Done-when guarantee not met)"; \
	            status=1; \
	        fi; \
	    done; \
	    if ! tr -d '\r' < "$$log" 2>/dev/null | grep -qE '^\[info\] acpi: MCFG base=0x[0-9a-f]{16} seg=[0-9]+ bus=[0-9]+-[0-9]+$$'; then \
	        echo "make test: $$log does not contain an 'acpi: MCFG base=...' line (ROADMAP M3.1 Done-when guarantee not met)"; \
	        status=1; \
	    fi; \
	    ncpu=$$(tr -d '\r' < "$$log" 2>/dev/null | grep -cE '^\[info\] acpi: MADT cpu apic-id=[0-9]+ uid=[0-9]+ (enabled|online-capable)( x2apic)?$$'); \
	    if [ "$$ncpu" != "$$cpus" ]; then \
	        echo "make test: $$log lists $$ncpu 'acpi: MADT cpu' lines, expected $$cpus (ROADMAP M3.1 Done-when guarantee not met)"; \
	        status=1; \
	    fi; \
	    if ! $(ACPIEXTRACT_BIN) --check --require FACP,APIC,DSDT,MCFG,HPET "$$log" > /dev/null; then \
	        echo "make test: $$log has no valid ACPIDUMP block with FACP,APIC,DSDT,MCFG,HPET (ROADMAP M3.1 item 3)"; \
	        status=1; \
	    fi; \
	    mcfgdir=$$(mktemp -d); \
	    want=; \
	    if $(ACPIEXTRACT_BIN) -o "$$mcfgdir" "$$log" > /dev/null 2>&1 && [ -f "$$mcfgdir/MCFG-0.dat" ]; then \
	        want=$$(od -An -v -tx1 -j44 -N8 "$$mcfgdir/MCFG-0.dat" | tr -s ' \n' '\n' | grep . | tac | tr -d '\n'); \
	    fi; \
	    rm -rf "$$mcfgdir"; \
	    got=$$(tr -d '\r' < "$$log" 2>/dev/null | sed -nE 's/^\[info\] acpi: MCFG base=0x([0-9a-f]{16}) .*/\1/p' | head -n 1); \
	    if [ "$${#want}" != 16 ] || [ "$$got" != "$$want" ]; then \
	        echo "make test: $$log logs MCFG base=0x$$got, but the dumped MCFG table says 0x$$want (ROADMAP M3.1 Done-when guarantee not met)"; \
	        status=1; \
	    fi; \
	done < "$(MATRIX)"; \
	exit $$status
