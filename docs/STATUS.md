# bongOS status
_Main-line status. Parallel-lane sessions don't edit this file; they track progress in their own milestone log._

**Last updated:** 2026-09-24 (M1.2 merged; M1.3 starting)

## Current milestone
**M1.3: Kernel skeleton + real handoff** (`needs-owner`), in progress. See `docs/logs/M1.3.md`.
Branch: `claude/relaxed-curie-knpvwb` (see the note at the top of the log for why this isn't the
usual `m1-3-...` name).

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
Consult the `architect` subagent for the BootInfo/page-table/ELF-loader/ktest design, then
implement M1.3 per `docs/logs/M1.3.md`'s plan.

## Blockers
_(none)_

## Questions for owner
_(none)_

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
