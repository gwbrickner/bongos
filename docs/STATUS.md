# bongOS status
_The main line's dashboard. Parallel-lane sessions never edit this file; they track progress in
their own milestone log. Keep it under ~80 lines. Finished milestones get one line here, and
the details belong in `docs/logs/M<p>.<n>.md`._

**Last updated:** 2026-10-02 (M3.2 merged; M3.3 started)

## Next step
**M3.3 Timekeeping is in progress** on branch `m3-3-timekeeping` (log: `docs/logs/M3.3.md`). M3.2 merged as #20.
Next: step 1, the pure `kernel/core/time-core.{h,c}` plus `tests/host/kernel_time_core_test.c` and
`kernel_timer_heap_test.c` (`make host-tests`).

## Current milestone
M3.3 Timekeeping (step 1 of 6).

## Phase
2: Blue Dream (CPU and memory core)

## Done (the full writeups are in each log's Summary)
| Milestone | PR | What it delivered |
|---|---|---|
| M1.1 | [#1](https://github.com/gwbrickner/bongos/pull/1) | Repo skeleton, Makefile + `mk/*.mk`, host-test framework, CI |
| M1.2 | [#2](https://github.com/gwbrickner/bongos/pull/2) | Our own UEFI headers, a hello loader, a GPT disk image (`tools/mkimage`) |
| M1.3 | [#3](https://github.com/gwbrickner/bongos/pull/3) | Higher-half kernel, klog, `panic()`, the ktest framework, the real BootInfo handoff |
| M1.4 | [#4](https://github.com/gwbrickner/bongos/pull/4) | GOP boot menu, fbcon, `tools/imgdiff`, GUI screenshot tests |
| M2.1 | [#5](https://github.com/gwbrickner/bongos/pull/5) | GDT/TSS/IST, full IDT, symbolized backtraces (KSYM), UBSan, stack protector, `archTrapCatch` |
| M2.2 | [#6](https://github.com/gwbrickner/bongos/pull/6) | pmm: Page array, buddy allocator (DMA32/NORMAL), page cache, double-free detection |
| M2.3 | [#7](https://github.com/gwbrickner/bongos/pull/7) | Kernel page tables, W^X verifier, framebuffer WC, SMEP/SMAP/UMIP, loader reclaim, KVA |
| M2.4 | [#8](https://github.com/gwbrickner/bongos/pull/8) | Slab caches, kmalloc (16–8192 bytes), vmalloc with guard pages, `PMM_BUG_OWNED_PAGE` |
| M2.5 | [#11](https://github.com/gwbrickner/bongos/pull/11) | BIOS loader: stage1 MBR, stage2 with a real-mode thunk, E820/VBE, a GPT+FAT32 reader, the shared menu and handoff. One image boots both ways |
| M2.6 | [#16](https://github.com/gwbrickner/bongos/pull/16) | KASLR in both loaders (2 MiB slide, `--emit-relocs` relocation, `kaslr=off`), slide-aware symbolizer, `libs/crypto` (SHA-256, ChaCha20), kernel RNG (`randomGetBytes`) |
| M3.1 | [#19](https://github.com/gwbrickner/bongos/pull/19) | ACPI tables: RSDP/XSDT/RSDT loader, FADT/MADT/MCFG/HPET/IVRS parsers, tables copied through temporary KVA windows (works on BIOS and UEFI), ACPI_RECLAIM freed after the copy, `acpidump=1` + `tools/acpiextract`, stored QEMU q35 tables, D-166..D-170 |
| M3.2 | [#20](https://github.com/gwbrickner/bongos/pull/20) (merged) | 8259 remap+mask, local APIC (x2APIC or xAPIC over the new UC `vmmMapMmio`), IOAPICs with MADT overrides, vector allocator + `irq.h`, EOI-after-handler dispatch, IF=1 after `irqInit()`, 22 irq ktests, D-171..D-175 |

The boot matrix covers `uefi 1` and `bios 1`, plus 3072 MiB and 4-CPU rows in `make test-full`. The final
boot screens are in `docs/screenshots/`.

## Blockers
_(none)_

## Questions for owner
- **M3.3:** (1) The wall-clock ktest cannot take host time on the cmdline (it is fixed in the image before the firmware
  runs): accept D-181 (the harness timestamps the kernel's `time: wall-check` serial line, <= 2 s) and update the
  ROADMAP wording? (2) Panic on bare metal when the TSC is not invariant (ARCH 1.3 says required), warn only under a
  hypervisor? (3) Is the reference PC's RTC kept in UTC (no Windows dual-boot)? (4) With no PM timer and no HPET,
  boot panics until PIT/CPUID-0x15 fallbacks exist: OK? (5) The 100 ms one-shot ktest retries up to 3 times (D-182): OK?
- **M3.2:** (1) LINT1 is programmed NMI/unmasked per the MADT (an NMI still panics, D-074): OK, or keep it masked
  until a watchdog milestone? (2) LVT Error stays masked (no handler): OK? (3) An unregistered vector is logged once,
  counted and EOI'd in every build, never a panic: OK? (4) `irqUnrouteGsi` of a level pin stalls (sweep lead S4):
  fix at the first level-pin user? (5) CI may run KVM, so the x2APIC path may run there first: check its log for
  `lapic: mode=x2apic`.
- **M3.1:** (1) Checksum strictness: reject a bad-checksum table (D-167, as designed) or warn and use it
  like Linux? (2) OK to commit QEMU's table blobs (incl. its DSDT AML) under `tests/data/acpi/`? (3) OK to keep
  `acpidump=1` in `tests/harness/ktest-boot.cfg` (30-60 KiB extra serial per matrix row)? (4) Reclaiming
  ACPI_RECLAIM for good means Phase-2 AML serves `DataTableRegion`, and SSDTs it `Load`s/`LoadTable`s, from the
  kernel copies (anything in ACPI_RECLAIM is gone); accepted?
- `BootInfo.bootDiskGuid` and `bootPartGuid` (D-056) have no milestone that fills them yet,
  so both stay zero. Suggestion: use the UEFI PartitionInfo protocol plus a BlockIo
  GPT-header read (the BIOS loader already has a GPT reader, D-105), in M6.4 (which adds the
  kernel's own GPT scanner, per D-056).
- **M2.6:** accept 8 bits of KASLR entropy (512 MiB window, D-121)? Keep the canary on D-077's seed
  fold, or move it to `randomGetBytes` later (the serial-printed slide leaks ~8 bits of that seed)?
  Is falling back to an unslid boot on a relocation failure (D-120) acceptable, versus refusing?
- **M2.6 slide in logs:** the kernel prints its KASLR slide (the `kaslr: virtBase=` line and every
  backtrace header). Once a user-readable kernel log exists (logd/dmesg) that defeats KASLR against
  local users, so that milestone must make the log privileged or redact the slide. Agree?
- **M2.6 vector provenance (needs network):** `libs/crypto/test/crypto-vectors.h` could not be diffed
  against the RFC text (rfc-editor.org was denied by the proxy). Every field was cross-checked against
  independent transcriptions, but someone with network access should still diff it against RFC 8439
  2.3.2/2.4.2/A.1/A.2 and FIPS 180-4.

## Waiting on owner (hardware checks and other owner-only steps)
Optional hardware checks never block a merge; the full steps are in each milestone log ("Owner hardware check").
- **M3.2:** boot the USB stick on the reference PC; expect `lapic: mode=x2apic`, `ioapic:` line(s), `irq: interrupts enabled`,
  `kernel: init done`, and no irq/lapic/ioapic warnings.
- **M3.1:** boot with `acpidump=1` in `boot.cfg`'s `cmdline`; check the `acpi:` lines; ideally save the serial log.
- **M2.6:** boot UEFI and BIOS (CSM); compare the `kaslr: virtBase=` line; check `random: seeded (hw words n/8 ...)`.
- **M2.5:** enable CSM, boot the same stick in BIOS mode; report whether the menu and kernel screen appear.
- **M1.4:** `dd` the image to a USB stick and boot it; check the menu, arrow keys and Enter, and the logged resolution.
- **Default the main session to Sonnet** (D-117): add `"model": "sonnet"` to `.claude/settings.json` (owner-only change).
- **Re-run the cloud environment's setup script**, so fresh sessions get `libclang-rt-18-dev` and `gdb`. Until then run
  `sudo apt-get install -y libclang-rt-18-dev` in each new container (move the 403 PPAs out of
  `/etc/apt/sources.list.d/` if apt fails).

## Open leads (for the next `bug-sweeper` or `/milestone-sweep` to triage)
- `make analyze` on main reports 5 warnings: `kernel/include/list.h:50` (a possible NULL
  `prev` dereference), `kernel/test/kmalloc_test.c:68,143,365`, and
  `kernel/test/pmm_test.c:405`. The test-file hits are probably deliberate misuse, but none has
  been triaged yet.
- M2.5 deferred some items on purpose (D-114), including a PM-side diagnostic IDT in stage2
  and dual teletype+serial logging before VBE is set up.

## Parallel lanes (informational; the main line updates this when lanes merge)
| Milestone | Branch | State |
|---|---|---|
