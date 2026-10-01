# bongOS status
_The main line's dashboard. Parallel-lane sessions never edit this file; they track progress in
their own milestone log. Keep it under ~80 lines. Finished milestones get one line here, and
the details belong in `docs/logs/M<p>.<n>.md`._

**Last updated:** 2026-10-01 (M3.1 finished and awaiting owner merge; M3.2 is next)

## Next step
Start **M3.2 Interrupt controllers** (its Need, M3.1, is done), after the owner merges the M3.1 PR (`needs-owner`).
1. `git fetch origin main && git switch -c m3-2-interrupt-controllers origin/main`
2. Copy `docs/logs/TEMPLATE.md` to `docs/logs/M3.2.md` and write its Plan from ROADMAP.md. Use `acpiGetInfo()->madt`
   (CPUs, I/O APICs, interrupt source overrides, LAPIC address) from M3.1; consult `architect` first (interrupts).

## Current milestone
None in progress (M3.1 is awaiting owner merge).

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

The boot matrix covers `uefi 1` and `bios 1`, plus 3072 MiB and 4-CPU rows in `make test-full`. The final
boot screens are in `docs/screenshots/`.

## Blockers
_(none)_

## Questions for owner
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
  independent transcriptions (Nettle, Mbed TLS, pyca, Linux testmgr, Crypto++, `a66c5b7`), but
  someone with network access should still diff it against RFC 8439 2.3.2/2.4.2/A.1/A.2 and FIPS 180-4.

## Waiting on owner (hardware checks and other owner-only steps)
- **M3.1 hardware check** (optional; never blocks): boot the USB stick on the reference PC with `acpidump=1` in
  `boot.cfg`'s `cmdline`, check the `acpi:` lines (CPU list, MCFG base, no rejected tables), and ideally save the
  serial log for `tools/acpiextract`. Full steps are in `docs/logs/M3.1.md`, "Owner hardware check".
- **Default the main session to Sonnet** (D-117). Adding `"model": "sonnet"` to
  `.claude/settings.json` is an owner-only change, because the agent isn't allowed to change
  its own settings. Until then, pick Sonnet when starting a session (`/model sonnet`).
- **Re-run the cloud environment's setup script** (Environment settings → re-run setup), so
  that fresh sessions get `libclang-rt-18-dev` and `gdb` from `tools/ci/install-deps.sh`.
  Until then, `make host-tests` needs `sudo apt-get install -y libclang-rt-18-dev` in each new
  container. If apt fails on the image's third-party PPAs (ondrej/php, deadsnakes; both 403),
  move those files out of `/etc/apt/sources.list.d/` first.
- **M1.4 hardware check:** `dd` the image to a USB stick and boot it. Confirm the boot menu
  appears and that the arrow keys and Enter work. Report the resolution it logs and whether
  scrolling is smooth. Full steps are in `docs/logs/M1.4.md`, "Owner hardware check".
- **M2.5 hardware check** (optional; never blocks): enable CSM, boot the same USB stick in
  legacy/BIOS mode, and report whether the menu and kernel screen appear, and at what
  resolution. If it doesn't boot, report the last thing visible. Full steps are in
  `docs/logs/M2.5.md`, "Owner hardware check".

- **M2.6 hardware check** (optional; never blocks): boot the USB stick twice (UEFI, and BIOS with CSM)
  and confirm the menu and kernel screen appear both times; with a serial cable, compare the
  `kaslr: virtBase=` line across the two boots and check `random: seeded (hw words n/8 ...)`. Full steps
  are in `docs/logs/M2.6.md`, "Owner hardware check".

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
