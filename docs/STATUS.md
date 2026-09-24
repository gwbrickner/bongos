# bongOS status
_Main-line status. Parallel-lane sessions don't edit this file; they track progress in their own milestone log._

**Last updated:** 2026-09-24 (M1.3: implemented, `make test` green, reviewer pass in progress)

## Current milestone
**M1.3: Kernel skeleton + real handoff** (`needs-owner`), in progress. See `docs/logs/M1.3.md`.
Branch: `claude/relaxed-curie-knpvwb` (see the note at the top of the log for why this isn't the
usual `m1-3-...` name).

Implemented per an `architect`-designed spec: `kernel.ld` + entry stub, serial/klog/panic, the
ktest framework, the ELF64 loader, the page-table builder (HHDM + kernel mapping + trampoline),
BootInfo validation, and memory-map conversion, plus the UEFI loader wired to load `kernel.elf`,
build page tables, exit boot services, and jump. `make host-tests` (52/52) and `make test`
(`KTEST PASS bootinfo_valid`, exit 33) both verified independently by the `qemu-tester` subagent,
not just self-reported by the implementing agent. A `reviewer` pass on the diff is running now.

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

## Next step
Wait for the `reviewer` subagent's findings on the M1.3 diff (already running); fix any Critical
findings and re-review. Then finish the milestone per CLAUDE.md: check ROADMAP.md's M1.3 box,
write `docs/logs/M1.3.md`'s Summary, and open the PR with `needs-owner: yes`.

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
