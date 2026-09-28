# bongOS status
_Main-line status. Parallel-lane sessions don't edit this file; they track progress in their own milestone log._

**Last updated:** 2026-09-28 (M2.5 BIOS loader in progress -- steps 1-12 of 13 done: the BIOS boot
menu works, `make test` green including both BIOS GUI screenshot tests, only docs/PR polish left)

## Current milestone
**M2.5 BIOS loader** is in progress on branch `m2-5-bios-loader` -- see `docs/logs/M2.5.md` for
the full plan and running log. Design consulted with the `architect` subagent first (D-099
through D-112, plus `docs/specs/bios-boot.md`'s byte-level layouts), giving a 13-step
implementation order. Steps 1-7 are done and each individually verified against real QEMU (no
regressions on the UEFI path throughout):
1-4. Refactored shared loader code out of the UEFI-only path so BIOS stage2 can reuse it instead
   of a second hand-copied implementation: `boot/common/hw/` (serial, libc-shim, cpu), the shared
   menu-UI drawing/logging, the shared video-mode-selection rule (`bootvideo.c`), and the shared
   page-table/BootInfo-build core (`boothandoff.c`).
5. Every pure, host-testable BIOS-side module with no hardware coupling: `bootgpt.c` (a
   from-scratch GPT reader), `bootfat.c` (a from-scratch FAT32 reader with VFAT long-name
   support), `bootcrc32.c`, `bootheap.c`, `bootacpi.c` (RSDP scan), `bootkey.c` (serial arrow-key
   parsing), `memMapE820TypeToBootMem`, and `tools/mkimage/biosboot.c` (the stage1
   patch-block writer/stage2-header validator) with `--stage1`/`--stage2` mkimage flags. 233 host
   tests total (up from 172 at the start of this milestone).
6. A real `boot/bios/stage1.asm` (440-byte NASM MBR, D-100) and `boot/bios/stage2/entry.asm` --
   **boots successfully under real QEMU with SeaBIOS**, verified repeatedly including the
   multi-chunk disk-read path (a synthetic 130-sector fake stage2 read back via a QMP
   physical-memory dump, byte-exact). `mk/image.mk`'s `image` target now always installs a real
   BIOS bootloader instead of leaving that partition an empty hole.
7. A20 enable, the GDT, the protected-mode switch, a full 32-bit C environment for stage2
   (i386-cross-compiled `boot/common/hw/*.c`, linked via a new `stage2.ld`, D-104), and the
   real-mode thunk (`rmInt`/`rmIdle`, D-102) -- **stage2 can now make real BIOS calls from C**,
   proven with INT 12h. A real bug was found and fixed only by booting under real QEMU plus GDB
   (not by reasoning about the asm alone): the thunk's register-marshaling loads the caller's
   requested `RmRegs.ebp` into the live EBP register, clobbering `rmInt`'s own frame pointer; the
   epilogue's `mov esp, ebp` then used that clobbered value and jumped into the BIOS ROM's reset
   vector on return. Fixed by removing the now-redundant, now-wrong instruction (see the log's
   step-7-final entry for the full bisection story). This is the milestone's highest-risk work,
   now behind it.

8. E820 (INT 15h AX=E820h, via the thunk) -> `memMapNormalize()` -> `bootHeapInit()`
   (`boot/bios/stage2/e820.c`, `main.c`'s `memMapSelfTest()`), logging every normalized region and
   the resulting heap capacity over serial. Found and fixed a second real thunk bug along the way
   (`rm.asm`'s ES/DS segment loads ran *after* the caller's EAX load and reused `ax`, silently
   zeroing EAX right before the BIOS call whenever ES/DS were 0 -- the common case -- which made
   every INT 15h call with real input, not just E820, look "unsupported"; INT 12h's self-test
   never caught it since it passes no input registers). `qemu-tester`-confirmed: `make host-tests`
   233/233, `make test` (UEFI matrix + GUI) all pass, `make format-check` clean, and 3 repeated
   BIOS boots at each of 512 MiB/3072 MiB all PASS and byte-identical (zero flakiness) -- 9 E820
   regions/510 MiB heap at 512 MiB, 10 regions/2046 MiB heap at 3072 MiB (correctly excluding the
   >=4 GiB range).
9. Disk: `diskProbe()` (`boot/bios/stage2/disk.c`) builds a `BootBlockDev` over thunked INT 13h
   (AH=41h EDD check, AH=48h drive params, AH=42h extended read through a bounce buffer);
   `main.c`'s `diskSelfTest()` finds the ESP via the already-written `bootGptFindPartition()`,
   mounts FAT32, reads+parses `/bong/boot.cfg`, and logs the result -- proving GPT->FAT->boot.cfg
   end to end. Found and fixed a real i386-portability bug in already-written `bootfat.c` (two
   64-bit/32-bit divisions needing `__udivdi3`, undefined here since this loader links no
   compiler-rt -- added a portable `bootDivMod64()` helper instead) and brought a repo-wide
   `make format-check` failure (116 violations, all in this milestone's own earlier-step files)
   back to clean. `qemu-tester`-confirmed: `make host-tests` 239/239, `make format-check` exits 0,
   `make test` (UEFI + GUI) an *exact* screenshot match, and 3 repeated BIOS boots at each of
   512 MiB/3072 MiB all PASS and byte-identical -- the full disk/GPT/FAT/boot.cfg chain works at
   both memory sizes (`disk has 4194304 sectors`, ESP at LBA 0x1000-0x80fff, `/bong/boot.cfg is
   502 bytes`, `3 entries, timeout 3s, default entry 0`, `default kernel = /bong/kernel.elf`).
10. VBE: `vbeSetMode()` (`boot/bios/stage2/vbe.c`) enumerates modes via the thunk (INT 10h
    AX=4F00h/4F01h) and sets one (AX=4F02h) through the same shared `bootvideo.c` accept/pick
    rule UEFI's GOP path uses. Found and fixed a real bug: `BootVideoMode.reservedMask` was never
    populated from VBE's Rsvd fields, so every mode's combined mask topped out at bit 23 instead
    of 24-31 and `bootVideoAccept()` rejected all of them (0/93 modes accepted); fixed by reading
    the reserved-mask fields like every other channel (20/93 now accepted, auto-picks 2560x1600,
    matching what UEFI's own GOP picks under the same QEMU config). `qemu-tester`-confirmed:
    `make host-tests` 239/239, `make format-check` exits 0, `make test` (UEFI + GUI) all pass, 5
    BIOS boots (3 at 512 MiB, 2 at 3072 MiB) all PASS and MD5-identical within each memory size,
    both ending `loader: VBE mode 2560x1600 pitch 10240 phys 0x00000000fd000000`.
11. Kernel load, RSDP scan, random seed, the shared handoff (`boothandoff.c`), and the long-mode
    trampoline (`boot/bios/stage2/trampoline.asm`, new, D-111) -- **the first full BIOS boot to
    the kernel.** Consulted the `architect` subagent first (paging/boot-handoff work); it found
    five real gaps in the plan before any code was written (the GDT needed to move into the new
    page-aligned `.trampoline` section rather than staying in `.text16`; HHDM runs must be built
    from the raw, filtered E820 map, not the already-normalized one; `bootHandoffFinalMap()`'s
    real signature; a missing long-mode CPUID check that would have made the trampoline's own
    `wrmsr` `#GP`; `.bss` was never actually zeroed). `boot/bios/stage2/handoff.c` (new) is the
    real flow: E820/heap, disk/GPT/FAT32, boot.cfg, VBE, the kernel ELF, the boot stack, RSDP,
    random seed, the page tables/BootInfo (via the already-shared `boothandoff.c`), and the jump.
    `main.c` thinned to just the banner, mirroring UEFI's own thin entry point. A real assembler
    bug (not a runtime one) was found and fixed at the first build attempt: `gdtr`'s limit can't
    be computed as `gdtEnd - gdt` once the GDT lives in a different object file (symbol-minus-
    symbol across files has no ELF relocation) -- hard-coded the limit instead, since the GDT's
    size is a fixed architectural constant. **`make test` (the real target) passed clean on the
    first real QEMU attempt after that fix**: `MATRIX: PASS` (both `uefi-1cpu` and the new
    `bios-1cpu`), `GUI: PASS`, `countdown-smoke: PASS` -- and a direct KTEST/isa-debug-exit run
    showed `KTEST DONE passed=40 failed=0` including `bootinfo_valid` and `loader_reclaimed`,
    repeated 3x at 512 MiB and 1x at 3072 MiB, all byte-identical. Added `bios 1` to
    `tests/harness/matrix.conf` and `bios 1`/`bios 1 3072` to `matrix-full.conf`.
12. The BIOS menu's input loop (`boot/bios/stage2/menu.c`, new, D-110): `rmIdle()` +
    `bootkey.c`'s serial ANSI-escape parser + INT 16h local keyboard input + a BDA-tick-counter
    countdown, driving the same shared `bootmenu.c` state machine and menu-drawing code UEFI's
    menu uses. `tests/gui/run.sh --fw bios` (its `mkimage` call never had `--stage1`/`--stage2`
    before this) passes all 4 scripted scenarios; captured `tests/gui/ref/bios-{menu,kernel}.png`
    and confirmed them **pixel-identical** to the existing UEFI references via `imgdiff compare`
    (exit 0) -- D-110's shared-drawing promise, now verified rather than asserted. `mk/test.mk`'s
    `test` target now runs the BIOS GUI tests and a BIOS countdown-smoke too; `make test` passed
    twice end to end (`MATRIX`/`GUI` x2/both countdown-smokes all PASS).

Remaining (step 13, in `docs/logs/M2.5.md`'s Plan section): docs polish, a `reviewer` pass, fix
findings, check ROADMAP.md's M2.5 box, write the Summary, open the PR. See `docs/logs/M2.5.md`'s
"Next step" for the precise resumption point.

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

**Next milestone: M2.5 BIOS loader** (`needs-owner`, ROADMAP.md). Needs M1.4 and M2.1, both long
done -- picked over the higher-numbered M2.6/M3.1 (M2.6 needs M2.4+M2.5; M3.1 needs M2.4) per the
session protocol's "lowest-numbered unchecked milestone whose Needs are done" rule. Steps: a
440-byte NASM stage1 (INT 13h AH=42h reads of stage2 using mkimage-patched LBA/length) and a
stage2 (A20, E820, VBE mode pick, RSDP scan, 32-bit C with real-mode INT 13h/INT 10h thunks, the
GPT/FAT32 readers and boot menu reused from `boot/common`); loads the kernel+initrd, builds page
tables + BootInfo (`bootMethod = BIOS`), enters long mode; `mkimage` installs stage1/stage2; the
boot matrix gains BIOS (SeaBIOS) rows alongside UEFI, including the screenshot tests.

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

## Next step
**M2.5 BIOS loader** is in progress (branch `m2-5-bios-loader`, log `docs/logs/M2.5.md`), steps
1-12 of 13 done (see "Current milestone" above) -- **the BIOS boot menu works,
`qemu-tester`-confirmed**: `boot/bios/stage2/menu.c` (new) drives the same shared state machine/
drawing code UEFI's menu uses, and `tests/gui/ref/bios-{menu,kernel}.png` (newly captured) are
confirmed pixel-identical to the UEFI references. `make test` passed repeatedly end to end with
both firmwares' matrix/GUI/countdown-smoke all green, zero flakiness across every repeat. Next:
step 13 -- docs polish, a `reviewer` subagent pass (fix every Critical finding, fix or explain
every Should-fix), check ROADMAP.md's M2.5 box, write the log's Summary (release notes), update
STATUS.md for the next milestone, and open the PR (`needs-owner: yes`, per D-099/D-109/D-111's
owner-review flags, using `.github/pull_request_template.md`) -- see `docs/logs/M2.5.md`'s Plan
section for the full step list. Every implementation step is now done; step 13 is the milestone's
finishing checklist, not new risk.

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

## Parallel lanes (informational; updated by the main line when lanes merge)
| Milestone | Branch | State |
|---|---|---|
