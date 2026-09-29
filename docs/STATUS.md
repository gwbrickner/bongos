# bongOS status
_Main-line status. Parallel-lane sessions don't edit this file; they track progress in their own milestone log._

**Last updated:** 2026-09-28 (M2.5 BIOS loader in progress -- steps 1-12 of 13 done; step 13's
`reviewer` pass complete (PASS, no Critical) and every Should-fix item fixed or explicitly
deferred via D-114; rebuild/retest confirmed fully green (`make test`/`make test-full`, both
firmwares, both memory sizes); a second `reviewer` re-review of the fix batch also complete
(PASS, no Critical, its own 4 Should-fix items fixed too); only the milestone's finishing
checklist -- ROADMAP box, Summary, this file's own milestone section, the PR -- is left)

## Current milestone
No milestone branch currently in progress -- M2.5 just finished (see below); M2.6 is next (see
"Next step").

**M2.5 BIOS loader** is done -- see `docs/logs/M2.5.md` for the full writeup and its Summary
section for the release notes. ROADMAP.md's M2.5 box is checked. Design consulted with the
`architect` subagent first (D-099 through D-112, plus `docs/specs/bios-boot.md`'s byte-level
layouts), giving a 13-step implementation order, all done. bongOS now boots on legacy BIOS/CSM
machines as well as UEFI: a 440-byte NASM stage1 MBR, a stage2 (A20, a real-mode<->PM switch, a
real-mode thunk letting 32-bit C call back into BIOS interrupts, E820, a from-scratch GPT+FAT32
reader with VFAT long-name support, VBE mode pick through the same shared selection rule UEFI's
GOP path uses, the same interactive boot menu byte-for-byte/pixel-for-pixel, RSDP scan, kernel
ELF load, and a long-mode trampoline into the same shared page-table/BootInfo builder UEFI uses),
and `mkimage` installing both loaders into one GPT image that boots either way. Implementation
found and fixed several real bugs only via booting under real QEMU (not host tests alone): a
thunk register-marshaling bug that clobbered its own frame pointer and jumped into the BIOS ROM's
reset vector; a second thunk bug where ES/DS loads silently zeroed EAX on every real BIOS call;
an i386-portability bug (a 64-bit division needing unavailable compiler-rt); `BootVideoMode`'s
reserved mask never being populated from VBE's fields (0/93 modes accepted until fixed). Reviewed
twice: the first `reviewer` pass found no Critical findings but 15 Should-fix items across the
whole diff (three real thunk-correctness bugs, a hard-coded PM stack address replaced with a
proper linker-script `.stack` section, BIOS-input-validation gaps, and FAT32/LFN-parsing
hardening against malformed volumes, all fixed, plus 6 new host tests); a second re-review of
those fixes found them sound and confirmed VERDICT: PASS, plus 4 more Should-fix items (a weak
regression test rewritten to actually exercise its guard, a real VBE 3.0 linear-framebuffer
correctness gap fixed rather than deferred, and doc/spec drift corrected). Three narrow items (a
PM-side diagnostic IDT and dual teletype+serial pre-VBE logging, both described in earlier
decisions but never actually built) are recorded as deliberate deferrals in D-114. `make
host-tests` 247/247 (up from 172 at the milestone's start); `make test`/`make test-full` pass
clean with no boot errors across both firmwares and both memory sizes, including exact BIOS GUI
screenshot matches; `make format-check` clean. PR pending (`needs-owner: yes` -- touches the boot
ABI, on-disk format parsing, and real-mode interrupt handling).

**M2.4 Slab, kmalloc, vmalloc** is done -- see `docs/logs/M2.4.md` for the full
writeup and its Summary section for the release notes. ROADMAP.md's M2.4 box is checked. Design
consulted with the `architect` subagent first (D-092 through D-098, several deliberately
simplified during implementation for a tractable, host-testable design -- see each entry). A real
bug was found and fixed via booting under real QEMU (not just host tests): a fresh `KERNEL_DEBUG`
slab carve's redzone fill could corrupt the free list's own bufctl array for several size classes,
since `slabComputeLayout()` didn't reserve room for object 0's own left redzone before the bufctl
array (D-093's `slabMinObjOffset()` now accounts for it; see the log's "(4)" entry for the full
bisection story). The `reviewer` subagent found no Critical findings on the full diff but 7
Should-fix items, all fixed: a latent `panicBug()`-while-`slabLock()`-held hazard around pmm/ctor/
dtor calls in slab growth/release (restructured to never hold the lock across those, matching
pmm.c's own D-082 pattern); `slabCacheDestroy()`/`slabAlloc()`/`slabCacheGetStats()` now validate
their cache pointer (bounds + in-use + a new `SLAB_CACHE_MAGIC`) instead of trusting it; a
`vmalloc_map_free_no_leak` test that only checked vmalloc's own counters (which don't prove a
frame was actually freed) now also checks the pmm's own page-level stats; added ktests for the
bufctl-only double-free path, D-095's `PMM_BUG_OWNED_PAGE`, `SLAB_BUG_VMALLOC_POINTER`/
`VMALLOC_BUG_NOT_VMALLOC`, and vmalloc's OOM partial-failure unwind; `vfree()`/`vmallocUnwind()`
now check `vmmUnmapKernel()`'s Status instead of ignoring it. Fixing that round's own new
`pmm_owned_page_rejected` ktest then caught a second real bug, again only via real QEMU: a
test-accounting gap where a slab grown mid-test (kmalloc-16's or vmalloc-area's own first backing
slab) looked like a 1-page leak -- fixed by warming both up after a clean `slabShrinkAll()`
baseline. 40 ktests (12 new this milestone) and 172 `make host-tests` cases pass. `make test`/
`make test-full` (including the memory-diversity matrix-full row) pass clean with no boot errors;
`make format-check` clean. [PR #8](https://github.com/gwbrickner/bongos/pull/8) open against
`main`, `needs-owner: yes` (D-045/§25 -- memory management, and a new always-on pmm check + a
KVA-allocator contract change to already-merged M2.2/M2.3 code).

**M2.3 Kernel paging** is done -- see `docs/logs/M2.3.md` for the full writeup
and its Summary section for the release notes. ROADMAP.md's M2.3 box is checked. Design consulted
with the `architect` subagent first (D-086 through D-091). Three `reviewer` rounds: the first
found no Critical findings but 12 Should-fix items (all fixed); the second, on those fixes, found
no Critical but judged 2 fixes only half-done and found 5 more real gaps -- including a genuine
W^X hole (`vmmMapKernel` could create a writable KVA-region alias of the kernel's own text/rodata,
which the HHDM-only W^X verifier would never see) -- plus a `make format-check` failure, all fixed;
the third confirmed those fixes and found only 5 small nits (4 fixed, 1 explicitly deferred as
speculative ahead of any real caller). All 28 ktests (7 new: `paging_text_write_faults`,
`paging_data_exec_faults`, `paging_fb_wc`, `paging_wx_verify`, `paging_text_hhdm_alias_readonly`,
`vmm_map_unmap`, `loader_reclaimed`) and all 162 `make host-tests` cases pass. `make test`/
`make test-full` (including the memory-diversity matrix-full row) pass clean with no boot errors;
`make format-check` clean. [PR #7](https://github.com/gwbrickner/bongos/pull/7) open against
`main`, `needs-owner: yes` (D-045/§25 -- memory management, security-sensitive CR3/PAT/CR4
changes, and a `kernelBootInfo()` contract change).

**M2.2 Physical memory manager** is done -- see `docs/logs/M2.2.md` for the full
writeup and its Summary section for the release notes. ROADMAP.md's M2.2 box is checked. The
`reviewer` subagent's first pass found one Critical finding (a real double-free bug: freeing the
*upper* half of a buddy pair could abandon its own head page in a stale allocated state, letting a
second free through undetected) and 8 Should-fix items; the Critical and 7 of the 8 Should-fix
items were fixed, the 8th recorded as a deliberate deferral (D-085, not a real risk within this
milestone's single-CPU/IF=0 scope). A second `reviewer` pass on the fixes found no further Critical
findings (VERDICT: PASS) and only minor nits, all addressed. `make test`/`make test-full` (21/21
ktests, including a new memory-diversity row exercising the NORMAL zone, D-084) pass clean with no
boot errors. [PR #6](https://github.com/gwbrickner/bongos/pull/6) open against `main`,
`needs-owner: yes` (D-045/§25 -- memory management,
the boot-time page mapper in `kernel/arch/x86_64/early-map.c`, a new interrupt-catch kind, and a
ROADMAP milestone-steps change, D-083).

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

**Next milestone: M2.6 KASLR + kernel RNG** (`needs-owner`, ROADMAP.md). Needs M2.4 and M2.5, both
now done -- the lowest-numbered unchecked milestone whose Needs are satisfied (M3.1 only needs
M2.4 but is higher-numbered). Steps: both loaders pick a 2 MiB-aligned slide inside the kernel
window from the random seed and apply the kernel ELF's `--emit-relocs` relocations
(`R_X86_64_64`, `R_X86_64_32S`), honoring `kaslr=off`; the kernel gains a real entropy pool
(RDSEED/RDRAND, the boot seed, interrupt timing later), a ChaCha20 CSPRNG, and `randomGetBytes`;
the symbolizer and panic output account for the slide. Done when: two boots produce different
`kernelVirtBase` (a new harness check) under both firmwares, the RNG passes a basic statistical
sanity test, ChaCha20 matches the RFC 8439 test vectors, and `kaslr=off` gives the fixed base.

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
- M2.2 ([PR #6](https://github.com/gwbrickner/bongos/pull/6) open, `needs-owner: yes`): bongOS has a real physical memory manager. A sparse
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
- M2.4 ([PR #8](https://github.com/gwbrickner/bongos/pull/8) open, `needs-owner: yes`): bongOS has a real kernel heap. A slab allocator
  (D-092..D-096) gives named object caches with constructors/destructors, an out-of-band free list
  (so poisoning and constructed state never collide), and one magazine per cache (today's BSP-only,
  D-094 -- the same honest single-CPU pattern M2.2's pmm page cache uses). `kmalloc`/`kfree` are 12
  fixed size classes (16-8192 bytes) on top; `vmalloc`/`vfree` (D-097) handle anything larger,
  eager and page-granular on M2.3's KVA allocator with a genuine unmapped guard page on each side.
  `KERNEL_DEBUG` builds add redzones and write-after-free poisoning, reported through the existing
  `panicBug()`/`TRAP_CATCH_KERNEL_BUG` mechanism (no new catch kind). The pmm gained
  `PMM_BUG_OWNED_PAGE` (D-095, always-on): freeing a page a slab or vmalloc still owns is now a
  caught kernel bug. The KVA allocator gained a `liveCount` admission cap (D-098) that provably
  keeps its free-extent table from ever filling on a legitimate free. Designed with the
  `architect` subagent; a real memory-corruption bug (a fresh debug-build carve's redzone fill
  overwriting its own free list) was found and fixed only by booting under real QEMU, not host
  tests alone. Two `reviewer` rounds found no Critical findings; 7 Should-fix items were fixed
  (see `docs/logs/M2.4.md`). 40 ktests (12 new) and 172 `make host-tests` cases pass; `make test`/
  `make test-full` pass clean with no boot errors; `make format-check` clean.
- M2.5 (PR pending, `needs-owner: yes`): bongOS boots on legacy BIOS/CSM machines, not just UEFI.
  A 440-byte NASM stage1 MBR (D-100) reads a new stage2: A20 enable, a real-mode<->protected-mode
  switch, a real-mode thunk (`rmInt`/`rmIdle`, D-102) letting 32-bit C call back into real BIOS
  interrupts, and a full C environment reusing almost all of the UEFI loader's shared code --
  E820, a from-scratch GPT+FAT32 reader (new, VFAT long-name support), VBE mode pick through the
  same selection rule the UEFI GOP path uses (D-109), the same interactive boot menu
  (pixel-identical, byte-identical log lines, D-110), RSDP scan, kernel ELF load, and a long-mode
  trampoline (D-111) into the exact same shared page-table/BootInfo builder UEFI uses
  (`bootMethod = BIOS`, D-108). `mkimage` installs both stage1 and stage2 into one GPT image that
  boots either way. Designed with the `architect` subagent (D-099..D-112); implementation found
  and fixed several real bugs only via booting under real QEMU (a thunk register-marshaling bug
  that clobbered its own frame pointer and jumped into the BIOS ROM reset vector; a second thunk
  bug silently zeroing EAX on every real BIOS call; an i386-portability division bug;
  `BootVideoMode.reservedMask` never being populated, rejecting every VBE mode until fixed). Two
  `reviewer` rounds: the first found no Critical findings but 15 Should-fix items (three
  thunk-correctness bugs, a hard-coded PM stack address replaced with a proper linker-script
  `.stack` section, BIOS-input-validation hardening, FAT32/LFN-parsing hardening against
  malformed volumes with 6 new host tests, all fixed); the second re-review confirmed those fixes
  and found 4 more (a weak regression test rewritten to actually exercise its guard, a real VBE
  3.0 linear-framebuffer correctness gap fixed rather than deferred, doc/spec drift corrected),
  all fixed. Three narrow items (a PM-side diagnostic IDT and dual teletype+serial pre-VBE
  logging, both described in earlier decisions but never actually built) recorded as deliberate
  deferrals in D-114 rather than silently dropped. `make host-tests` 247/247 (up from 172 at the
  milestone's start); `make test`/`make test-full` pass clean with no boot errors across both
  firmwares and both memory sizes, including exact BIOS GUI screenshot matches; `make
  format-check` clean -- see `docs/logs/M2.5.md`.

## Next step
M2.5 is fully done: both `reviewer` rounds PASS (no Critical findings, every Should-fix item
fixed or explicitly deferred via D-114), `make host-tests`/`make test`/`make test-full` all
green, ROADMAP.md's M2.5 box checked, this file and `docs/logs/M2.5.md`'s Summary updated. Opening
the PR against `main` (`needs-owner: yes`) is the last action of this session.

**Next milestone to start: M2.6 KASLR + kernel RNG** (see "Current milestone" above for the
step list) -- create branch `m2-6-kaslr-rng`, copy `docs/logs/TEMPLATE.md` to
`docs/logs/M2.6.md`, and consult the `architect` subagent first (KASLR relocation application and
the kernel RNG are both explicitly listed in CLAUDE.md's "consult before implementing" list --
syscall-ABI-adjacent boot handoff work and crypto).

M2.4 is done; [PR #8](https://github.com/gwbrickner/bongos/pull/8), #7 (M2.3), and #6 (M2.2) remain
open against `main`, all `needs-owner: yes`, waiting on the owner's review -- unrelated to M2.5's
own progress.

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
- M2.5's owner hardware check (ROADMAP M2.5, **optional**, never blocks merging): if the board's
  CSM and GPU allow legacy boot, enable CSM, `dd` the same image to a USB stick, and boot it in
  legacy/BIOS mode specifically (not UEFI). Report whether it comes up at all (some GPUs don't
  expose a usable framebuffer to a legacy INT 10h/VBE call the way they do to UEFI GOP), and if
  so whether the menu/kernel screen and logged resolution look right. If it doesn't boot, report
  the last thing visible on screen or over serial. The BIOS path is already fully verified under
  QEMU/SeaBIOS regardless of this check's outcome. Full instructions in `docs/logs/M2.5.md`'s
  "Owner hardware check" section.

## Parallel lanes (informational; updated by the main line when lanes merge)
| Milestone | Branch | State |
|---|---|---|
