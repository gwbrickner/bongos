# bongOS status
_Main-line status. Parallel-lane sessions don't edit this file; they track progress in their own milestone log._

**Last updated:** 2026-09-25 (M2.2 Physical memory manager merged (PR #6); M2.3 is next)

## Current milestone
None in progress. **M2.2 Physical memory manager** is done -- see `docs/logs/M2.2.md` for the full
writeup and its Summary section for the release notes. ROADMAP.md's M2.2 box is checked. The
`reviewer` subagent's first pass found one Critical finding (a real double-free bug: freeing the
*upper* half of a buddy pair could abandon its own head page in a stale allocated state, letting a
second free through undetected) and 8 Should-fix items; the Critical and 7 of the 8 Should-fix
items were fixed, the 8th recorded as a deliberate deferral (D-085, not a real risk within this
milestone's single-CPU/IF=0 scope). A second `reviewer` pass on the fixes found no further Critical
findings (VERDICT: PASS) and only minor nits, all addressed. `make test`/`make test-full` (21/21
ktests, including a new memory-diversity row exercising the NORMAL zone, D-084) pass clean with no
boot errors. Merged to `main` via [PR #6](https://github.com/gwbrickner/bongos/pull/6) (was
`needs-owner: yes`, D-045/§25 -- memory management, the boot-time page mapper in
`kernel/arch/x86_64/early-map.c`, a new interrupt-catch kind, and a ROADMAP milestone-steps
change, D-083). CI's `release.yml` auto-tags `v0.2.2` and publishes a GitHub Release from the
merge commit.

**M2.1 GDT, IDT, exceptions, hardening runtime**: merged to `main` via
[PR #5](https://github.com/gwbrickner/bongos/pull/5). ROADMAP.md's M2.1 box is checked. The
`reviewer` subagent found no Critical findings on the full milestone diff; all 8 Should-fix items
it raised were fixed (see the log's reviewer-round entry) -- a `ubsanReporting` recursion-guard
ordering bug, a UBSan RIP that resolved to the wrong function, a still-armed-catch race that could
misattribute an unrelated later fault, incomplete RFLAGS clearing on a caught-fault resume, an
`archTrapCatchResume` that depended on stack memory the very mechanism under test could corrupt
(fixed at the root, not just worked around), a missing automated check that a panic report's
backtrace is actually symbolized, and a missing contract comment on `trapDispatch`. Merged against
`main`, was `needs-owner: yes` (D-045/§25 -- interrupts, security-sensitive stack-protector/UBSan
runtimes, and a boot-ABI change to the kernel ELF's PT_LOAD count, D-073).

**Next milestone: M2.3 Kernel paging** (`needs-owner`, ROADMAP.md). Needs M2.2, now done. Steps:
build the kernel's own PML4 (HHDM, per-section kernel image permissions, pre-allocated kernel-half
PML4 entries 256-511); switch to it and remove the loader identity mapping; PAT setup (remap the
framebuffer WC); SMEP/SMAP/UMIP; `vmmMapKernel`/`vmmUnmapKernel` and the kernel virtual area
allocator; and step 6 (moved from M2.2, D-083): reclaim `LOADER_RECLAIM` memory via
`pmmAddFreeRange()` over the pmm's own `PmmMap.loaderReclaim[]`, once the kernel's own page tables
are live and CR4.PGE has been toggled -- and stop using `kernelBootInfo()`'s live pointer/the
loader's memory-map array once that reclaim runs. Consult the `architect` subagent first (paging is
on CLAUDE.md's consult-first list).

## Phase
1: Acapulco Gold

## What works
- Planning docs: ARCHITECTURE, DECISIONS, ROADMAP.
- M1.1 (merged, PR #1): the ARCHITECTURE §2 directory skeleton, the top-level `Makefile` +
  `mk/*.mk` fragments, `branding.h` generation, `make format`/`format-check`, the
  `tests/host/` host-test framework, and `image`/`test`/`test-full`/`run`/`run-bios`/`debug`/
  `gdb` wired to the existing boot harness.
- M1.2 (merged, PR #2): bongOS boots for the first time. `tools/mkimage` hand-builds a real GPT
  disk image (our own protective-MBR/GPT/CRC32 code) with a FAT32 ESP and a root partition under
  a newly-minted type GUID (D-056); a from-scratch UEFI loader built on our own minimal UEFI
  headers (D-007) prints a banner to ConOut and COM1, then halts. `make test` boots it under OVMF
  and checks the serial output (D-057's `--expect-serial`, a bridge until M1.3's kernel brings
  the real KTEST protocol).
- M1.3 (merged, PR #3): bongOS has a real kernel. `kernel.ld` links it higher-half with a W^X section
  layout; the entry stub installs a boot GDT and null IDT; the kernel has a 16550 serial driver,
  `klog`, `panic()`, and an in-kernel test framework (`KTEST()`, `ktest=` cmdline, isa-debug-exit
  PASS/FAIL reporting). `boot/common/` gained a from-scratch ELF64 loader, a page-table builder
  (HHDM + W^X kernel mapping + a trampoline identity page, 1 GiB/2 MiB pages where supported), a
  BootInfo builder matching ARCHITECTURE §5.3 byte-for-byte, and memory-map conversion/
  normalization. The UEFI loader now reads `boot.cfg`, loads `kernel.elf`, builds page tables,
  exits boot services with a proper retry loop, and jumps to the kernel. `make test` boots under
  OVMF and QEMU exits 33 with `KTEST PASS bootinfo_valid` in the serial log; the harness now greps
  for that literal line instead of trusting the exit code alone, so the Done-when guarantee is an
  enforced gate, not a coincidence. Reviewed (`architect` for the design, then two `reviewer`
  rounds -- the first found and fixed one Critical, an ELF-loader integer-overflow bug; the second
  passed clean after fixing 3 more Should-fix items -- see `docs/logs/M1.3.md`); PR open,
  `needs-owner: yes`.
- M1.4 (merged, PR #4): bongOS boots into a real graphical boot menu. The UEFI loader picks a
  GOP mode (auto or `resolution=`), draws an interactive menu (arrow keys/Enter/digits, mirrored
  to serial) driven by a new pure boot-menu state machine, and hands the kernel a real HHDM-mapped
  framebuffer (D-068: the one exception to D-059's "MMIO is never HHDM-mapped", 4 KiB/UC-/NX/
  global). `boot.cfg` gained a full `[Name]`-section grammar (D-067) with inheritance and
  line-numbered errors, validated both by the loader and at image-build time. The kernel's new
  `fbcon` driver mirrors every `klog` line onto the screen in color, using an original 8x16
  console font (D-069, `tools/mkfont`) and a glyph-blit primitive shared with the loader's menu.
  New `tools/imgdiff` (a from-scratch PPM/PNG/DEFLATE codec, no third-party dependency) and a
  QMP-scripting test harness (`tests/harness/qemu-script.py`, `tests/gui/`) screenshot-test the
  whole pipeline end to end (D-070) -- `make test` now includes it. Designed with the `architect`
  subagent (D-067 through D-070); implemented and verified in 7 independently-committed steps.
  Actually running the finished GUI harness against live QEMU caught and fixed two real bugs (a
  font-generation aliasing mistake, and a loader `ConIn->Reset()` ordering bug that could swallow
  a fast keypress) -- see `docs/logs/M1.4.md` for details. The `reviewer` subagent then found no
  Critical findings but 9 legitimate Should-fix items (an unverified PAT-cacheability assumption
  and a shallow framebuffer conflict scan, a real fbcon tab-handler hang at 1-column consoles,
  scroll/pixel-write performance, the menu wrongly skipping outright with no framebuffer, an
  ARCHITECTURE doc/behavior mismatch, 5 undocumented local decisions now D-071, a test-harness
  serial-drain bug that could hide a failing ktest's output, and a font glyph collision ('S'/'5'
  identical) -- all 9 fixed, verified individually and then together (`make format-check`,
  `make host-tests` 131/131, `make image`, `make test` including regenerated GUI references), see
  `docs/logs/M1.4.md`'s reviewer-round entries. PR #4 merged.
- M2.1 (merged, [PR #5](https://github.com/gwbrickner/bongos/pull/5)): bongOS has a real GDT/TSS with dedicated IST stacks for
  #DF/NMI/#MC and a full 256-vector IDT (D-072/D-074) -- every exception is reported with
  registers/CR2/control registers and a **symbolized** backtrace, then panics, except #BP, which
  resumes cleanly. Symbolization comes from a new embedded KSYM v1 symbol table (D-075,
  `tools/ksyms`, a two-pass kernel link) wired into every backtrace frame. Debug builds gained a
  from-scratch UBSan runtime (all 16 `__ubsan_handle_*` checks, D-076) and a stack-protector canary
  reseeded from `BootInfo.randomSeed` (D-077). The centerpiece is `archTrapCatch` (D-078): a
  ktest-only mechanism that deliberately triggers a real #PF/#UD/stack-smash/UBSan-trip and proves
  the kernel detects it, without ending the whole test run -- unlocking the milestone's 6
  prescribed ktests. Designed with the `architect` subagent (D-072 through D-078); implemented in
  5 independently-committed, independently-verified steps (`docs/logs/M2.1.md`). Building and
  testing it caught several real bugs beyond the architect's own design (a GDTR limit off-by-one,
  a frame-lifetime/tail-call bug in the backtrace ktest, an overflow bound that reached past its
  intended frame and corrupted the catch mechanism's own resume dependency). The `reviewer`
  subagent found no Critical findings on the full diff but 8 legitimate Should-fix items (a
  recursion-guard ordering bug, a UBSan RIP resolving to the wrong function, a still-armed-catch
  race, incomplete RFLAGS clearing, the resume-depends-on-stack-memory issue fixed at its root
  this time rather than just bounded around, a missing automated symbolized-backtrace check, a
  missing contract comment) -- all fixed, see `docs/logs/M2.1.md`'s reviewer-round entry.
- M2.2 (merged, [PR #6](https://github.com/gwbrickner/bongos/pull/6)): bongOS has a real physical memory manager. A sparse
  `Page` metadata array (D-079, one 64-byte entry per BootInfo-managed frame, widened to order-10
  envelopes) is bootstrapped by a one-shot bump allocator (D-080) that also verifies the loader
  actually kept its D-059 HHDM promise, region by region, before trusting it. A buddy allocator on
  top (D-081: orders 0-10, `DMA32`/`NORMAL` zones, block state living entirely in the Page array)
  has a BSP-only per-CPU page cache in front of it for order-0 allocations, plus always-on
  double-free/misuse detection (D-082) that extends `archTrapCatch` with a third catch kind,
  `TRAP_CATCH_KERNEL_BUG`. `LOADER_RECLAIM`'s reclaim moves to M2.3 (D-083). Designed with the
  `architect` subagent (D-079 through D-083); implementing it against real QEMU (not just host
  tests) caught two real bugs beyond the design itself -- an HHDM-check bug that hung the boot on
  the legacy VGA/BIOS memory hole (walked the *widened* Page-array spans instead of the raw
  BootInfo regions), and a buddy-allocator bookkeeping leak (`managedPages` was bumped on every
  free, not just a range's first-ever one). The `reviewer` subagent's first pass then caught a
  third, more serious bug beyond either of those: freeing the *upper* half of a buddy pair could
  abandon its own head page in a stale ALLOCATED state (since `buddyFreeBlock()` only ever writes
  the *final merged* head's Page), letting a second free of it through completely undetected --
  fixed, with a rewritten `pmm_double_free` ktest that deterministically reproduces exactly that
  scenario rather than relying on luck. 7 more Should-fix items were fixed (full-page write-after-
  free poison verification, a PAT-bit masking bug in the boot-time page mapper's large-page
  lookups, per-page stress-test tagging so an overlapping-block bug can't hide, NORMAL-zone-
  preference and `PMM_FLAG_ZERO` test coverage, harness greps naming each required ktest, and
  assorted stale/garbled contract comments); the 8th (validating outside the lock ahead of real
  concurrency, and a caller-trust gap in `pmmAddFreeRange`) recorded as a deliberate deferral,
  D-085, since M2.2 itself runs single-CPU with IF=0 and cannot trigger either risk. A second
  `reviewer` pass on the fixes found no further Critical findings (VERDICT: PASS); its remaining
  nits (a page leak in the new poison ktest, a missing LIFO-determinism assertion, comment
  wording) were also fixed. `make test`/`make test-full`: 21/21 ktests pass in both the default
  512 MiB config and a new memory-diversity row (D-084, `uefi 1 3072`) that's the only
  configuration in the harness actually exercising the NORMAL zone -- see `docs/logs/M2.2.md`.

## Next step
M2.2's [PR #6](https://github.com/gwbrickner/bongos/pull/6) merged to `main`. A fresh session (or
this one, if continuing) starts M2.3 following the normal session protocol -- create a branch,
copy `docs/logs/TEMPLATE.md` to `docs/logs/M2.3.md`, consult the `architect` subagent for the
kernel paging design (own PML4 layout, PAT reprogramming, the kernel virtual area allocator's API
shape, and the LOADER_RECLAIM reclaim step moved here from M2.2 per D-083), then implement
incrementally per the session protocol. See this file's "Next milestone" section above and
ROADMAP.md's own M2.3 section for the full step list.

## Blockers
_(none)_

## Questions for owner
- ~~ROADMAP.md's M2.2 step 4 LOADER_RECLAIM-timing question~~ -- design resolved by M2.2's
  `architect` consultation: the reclaim moved to M2.3 (D-083), ROADMAP.md's M2.2/M2.3 sections
  updated in the same PR. Still pending the owner's actual sign-off through that PR's
  `needs-owner: yes` review (a ROADMAP milestone-steps change), not yet a closed item.
- `BootInfo.bootDiskGuid`/`bootPartGuid` (D-056) still have no milestone assigned to fill them;
  M1.4 didn't touch this (it wasn't part of the architect's D-068 design or ROADMAP's M1.4 steps),
  so both remain zero. Still suggest a UEFI PartitionInfo protocol + BlockIo GPT-header read,
  whichever milestone the owner wants to assign it to (M6.4, which adds the kernel's own GPT
  scanner per D-056, looks like a natural fit).

## Waiting on owner (hardware checks and other owner-only steps)
- Still open from M1.1: `libclang-rt-18-dev` (host-test sanitizers) and `gdb` (`make gdb`) were
  added to `tools/ci/install-deps.sh`. The cloud environment's cached setup script still needs
  re-running once (Environment settings -> re-run setup, or it picks it up on the next cache
  invalidation) for `make host-tests`/`make gdb` to work in a *fresh* session without manual
  intervention. This M1.4 session hit the same gap and worked around it for itself by running
  `sudo apt-get install -y libclang-rt-18-dev` directly (confirmed `make host-tests` then runs
  for real, with ASan/UBSan, not the M1.2-era plain-build fallback) -- but that's a per-container
  fix that won't survive to the next session, so the underlying setup-script re-run is still
  needed. CI itself runs the real `sudo bash tools/ci/install-deps.sh` step and passes normally.
- M1.4's owner hardware check (ROADMAP M1.4): `dd` the image to a USB stick, boot it, confirm the
  boot menu appears and arrow keys/Enter work, then report the resolution logged and whether
  scrolling looks noticeably slow (expected until M2.3 remaps the framebuffer WC, D-068). Full
  instructions in `docs/logs/M1.4.md`'s "Owner hardware check" section.

## Parallel lanes (informational; updated by the main line when lanes merge)
| Milestone | Branch | State |
|---|---|---|
