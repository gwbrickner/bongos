# bongOS status
_The main line's dashboard. Parallel-lane sessions never edit this file; they track progress in
their own milestone log. Keep it under ~80 lines. Finished milestones get one line here, and
the details belong in `docs/logs/M<p>.<n>.md`._

**Last updated:** 2026-10-02 (owner decisions D-171..D-176 recorded; M3.1 awaiting owner merge; M3.2 is next)

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
_(none: all ten earlier questions were answered on 2026-10-02, see D-171..D-176.)_

## Waiting on owner (hardware checks and other owner-only steps)
- **M3.1 hardware check** (optional; never blocks): boot the USB stick on the reference PC with `acpidump=1` in
  `boot.cfg`'s `cmdline`, check the `acpi:` lines (CPU list, MCFG base, no rejected tables), and ideally save the
  serial log for `tools/acpiextract`. Full steps are in `docs/logs/M3.1.md`, "Owner hardware check".
- **Allow two hosts in the environment's network policy** (D-176): `rfc-editor.org`, plus the NIST host serving
  FIPS 180-4. Environment menu in the session's title bar, then Edit, then Network access. After that, ask a session
  to diff `libs/crypto/test/crypto-vectors.h` against RFC 8439 2.3.2/2.4.2/A.1/A.2 and FIPS 180-4. Crypto stays
  EXPERIMENTAL until then.
- _(Declined, 2026-10-02: the `"model": "sonnet"` default in `.claude/settings.json`. Keep picking Sonnet with `/model sonnet`.)_
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
- **Loud unslid fallback (D-173):** the loaders still fall back silently-ish when relocation fails. Needs a `BootInfo`
  flag, a screen+serial warning, a kernel `klog` warning, and a forcing ktest. Boot-ABI change: `architect` first,
  `needs-owner`. Not scheduled; pick it up with the next boot-handoff change or as its own small step.

## Parallel lanes (informational; the main line updates this when lanes merge)
| Milestone | Branch | State |
|---|---|---|
