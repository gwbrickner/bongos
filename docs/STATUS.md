# bongOS status
_Main-line status. Parallel-lane sessions don't edit this file; they track progress in their own milestone log._

**Last updated:** 2026-09-25 (M1.4 in progress)

## Current milestone
M1.4: Framebuffer console + boot menu, in progress (see `docs/logs/M1.4.md`). M1.3's PR (#3)
merged to `main` at `54f1842`. Branch: `claude/affectionate-ritchie-cxvoi6` (see the note at the
top of the M1.4 log for why this isn't the usual `m1-4-...` name).

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

## Next step
Consult the `architect` subagent on the boot.cfg `[entry]` schema, PSF font handling, and the
screenshot-test approach (see `docs/logs/M1.4.md`'s Plan section), then implement M1.4 step 1
(loader GOP mode selection + `BootInfo.fb`).

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
- `BootInfo.bootDiskGuid`/`bootPartGuid` (D-056) have no milestone assigned to fill them yet; M1.3
  leaves both zero (valid under the v1 "zero means not provided" semantics recorded in D-064).
  Suggest M1.4 fills them via UEFI's PartitionInfo protocol + a BlockIo read of the GPT header,
  but flagging for the owner to confirm before M1.4 starts.

## Waiting on owner (hardware checks and other owner-only steps)
- Still open from M1.1: `libclang-rt-18-dev` (host-test sanitizers) and `gdb` (`make gdb`) were
  added to `tools/ci/install-deps.sh`. The cloud environment's cached setup script needs
  re-running once (Environment settings -> re-run setup, or it picks it up on the next cache
  invalidation) for `make host-tests`/`make gdb` to work in a fresh session without CI's own
  `sudo bash tools/ci/install-deps.sh` step. M1.2's host tests (GPT writer, UEFI GUIDs) were
  verified with a plain non-sanitized build in the meantime; CI itself runs the real
  `sudo bash tools/ci/install-deps.sh` step and should pass `make host-tests` normally.

## Parallel lanes (informational; updated by the main line when lanes merge)
| Milestone | Branch | State |
|---|---|---|
