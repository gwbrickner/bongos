# bongOS status
_Main-line status. Parallel-lane sessions don't edit this file; they track progress in their own milestone log._

**Last updated:** 2026-09-25 (M1.4 merged; M2.1 in progress)

## Current milestone
**M2.1 GDT, IDT, exceptions, hardening runtime** -- in progress (see `docs/logs/M2.1.md`).
Consulting the `architect` subagent first (interrupts/exceptions per CLAUDE.md); implementation
not yet started.

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

## Next step
M2.1 is in progress (`docs/logs/M2.1.md`). Waiting on the `architect` subagent's design response
for the GDT/TSS/IDT layout, IST stack allocation, exception-stub generation, the C-level
TrapFrame/handler design, the compressed-symbol-table embedding approach, UBSan runtime scope,
and the ktest-recoverable-fault mechanism (how a ktest deliberately triggers #PF/#UD/a stack
smash and has the handler report PASS/FAIL and continue to the next ktest instead of panicking).
Once it returns: record any new D-0xx decisions, then implement step 1 (GDT+TSS+IDT) first.

## Blockers
_(none)_

## Questions for owner
- ROADMAP.md's M2.2 step 4 says to reclaim `LOADER_RECLAIM` memory "after switching stacks", but
  that memory holds the *live* page tables (and the loader's identity-mapped trampoline page)
  until the kernel builds and switches to its own CR3 in M2.3. Reclaiming it that early would free
  memory the CPU is still using for address translation. Flagged by the `architect` subagent while
  designing M1.3's handoff; proposed fix is to move that reclaim to M2.3 (after the kernel's own
  page tables are live and CR4.PGE has been toggled, which M2.3 needs anyway since the loader's
  HHDM/kernel mappings are marked Global). No code changed yet -- this needs an owner decision on
  updating ROADMAP.md's M2.2 wording before that milestone starts.
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
