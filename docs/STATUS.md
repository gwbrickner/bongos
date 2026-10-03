# bongOS status
_The main line's dashboard. Parallel-lane sessions never edit this file; they track progress in
their own milestone log. Keep it under ~80 lines. Finished milestones get one line here, and
the details belong in `docs/logs/M<p>.<n>.md`._

**Last updated:** 2026-10-02 (M3.3 merged; M3.4 finished, no PR yet)

## Next step
**M3.4 Locking + lock validator is finished** (log `docs/logs/M3.4.md`; three finish sweeps `SWEEP: PASS`, reviewer
`VERDICT: PASS`) on the session branch `claude/resume-bongos-dev-prrfwz`; **no PR has been opened** (the session was
told not to), `needs-owner`. Next: owner opens/merges the PR and answers the M3.4 questions below, then start **M3.5 SMP
bring-up** (Needs M3.3 and M3.4 are done).

## Current milestone
None in progress (M3.4 awaits the owner's PR and merge).

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
| M3.2 | [#20](https://github.com/gwbrickner/bongos/pull/20) | 8259 remap+mask, local APIC (x2APIC or xAPIC over the new UC `vmmMapMmio`), IOAPICs with MADT overrides, vector allocator + `irq.h`, EOI-after-handler dispatch, IF=1 after `irqInit()`, 22 irq ktests, D-171..D-175 |
| M3.3 | [#21](https://github.com/gwbrickner/bongos/pull/21) | Timekeeping: TSC calibrated against the PM timer (HPET optional), `timeMonotonicNs`, LAPIC timer (one-shot or TSC-deadline) + per-CPU 256-entry timer heap (`timerArm`/`timerCancel`), CMOS RTC `timeWallNs`, harness wall-clock-vs-host check, 25 ktests, D-176..D-182 |
| M3.4 | (not opened) | Locking: ticket `Spinlock` (irqsave, trylock), `preemptCount` via an interim `CpuSync`, `atomic.h`, the debug lock validator (classes by init site, order graph with both stacks on an inversion, recursion, IRQ-safety; ktest expect mode), klog/pmm/vmm/slab/vmalloc/random converted to real spinlocks, D-183..D-189 |

The boot matrix covers `uefi 1` and `bios 1`, plus 3072 MiB and 4-CPU rows in `make test-full`. The final
boot screens are in `docs/screenshots/`.

## Blockers
_(none)_

## Questions for owner
- **M3.4:** (1) D-183: a profile-dependent `Spinlock` size (8 bytes release, 32 debug), so `.kmod`s must be built with the kernel's profile: OK? (2) D-186: about 110 KiB of debug-only `.bss` for the validator tables: OK? (3) D-188 (supersedes D-081/D-088/D-094 text): keep D-173's ban on allocating in IRQ handlers until M3.5 fixes D-085(1) and the slab's two-section free? (4) Same-class nesting is reported as recursion until a real user needs a `spinLockNested`: OK? (5) The D-187 balance checks in `irqDispatch`/`archTrapCatch` panic, and the harness has no expected-panic mode, so they have no permanent test (each was shown to fire with a temporary ktest): add an expected-panic harness row later?
- `BootInfo.bootDiskGuid`/`bootPartGuid` (D-056) stay zero until M6.4 (UEFI PartitionInfo + GPT read); see `docs/logs/M2.6.md`.
- **Older milestone questions** (M2.6, M3.1, M3.2, M3.3) are still open; the full text moved to the "Owner questions" section of `docs/logs/M2.6.md`, `M3.1.md`, `M3.2.md` and `M3.3.md`.

## Waiting on owner (hardware checks and other owner-only steps)
Optional hardware checks never block a merge; the full steps are in each milestone log ("Owner hardware check").
- **M3.3:** boot the USB stick on the reference PC; report the `time: tsc ... Hz` line (expect ~4.5e9), `tsc invariant=`, the `time: hpet` line, `lapic timer mode=` and the `time: rtc` line vs real UTC; no `time:` warn/error lines expected. Also check the first CI/KVM run shows `lapic timer mode=tsc-deadline` and green time ktests (that path cannot run under TCG).
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
- M3.4 leads for M3.5: the validator's expect-mode state is global (another CPU's real report of the armed kind would be swallowed); the deferred IRQ-safe -> IRQ-unsafe dependency check (D-186) is the real SMP deadlock class and should land with SMP; an exception between `klogHeld = 0` and the raw release in klog's sink section would self-deadlock (nothing can raise one there today); `panicEnter` is now an atomic exchange but the rest of the panic path is single-CPU.
- M3.3 sweep leads: SIGTERM to `tests/harness/run-qemu.sh` leaves `timeout`/QEMU running until `--timeout` (pre-existing); `clockref.c` would map a PM-timer GAS with a space id other than 0/1 as MMIO (unreachable: the ACPI parser only accepts 0/1); `timerInit` on a still-armed timer corrupts the queue (documented, not checked).
- `make analyze` on main reports 5 warnings: `kernel/include/list.h:50` (a possible NULL
  `prev` dereference), `kernel/test/kmalloc_test.c:68,143,365`, and
  `kernel/test/pmm_test.c:405`. The test-file hits are probably deliberate misuse, but none has
  been triaged yet.
- M2.5 deferred some items on purpose (D-114), including a PM-side diagnostic IDT in stage2
  and dual teletype+serial logging before VBE is set up.

## Parallel lanes (informational; the main line updates this when lanes merge)
| Milestone | Branch | State |
|---|---|---|
