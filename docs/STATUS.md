# bongOS status
_Main-line status. Parallel-lane sessions don't edit this file; they track progress in their own milestone log._

**Last updated:** 2026-09-24 (M1.2 done, PR open)

## Current milestone
None in progress. M1.2 is done and its PR is open (branch `m1-2-uefi-hello-loader`; see
`docs/logs/M1.2.md`). **Next: M1.3: Kernel skeleton + real handoff** (`needs-owner`), once M1.2
merges.

## Phase
1: Acapulco Gold

## What works
- Planning docs: ARCHITECTURE, DECISIONS, ROADMAP.
- M1.1 (merged, PR #1): the ARCHITECTURE §2 directory skeleton, the top-level `Makefile` +
  `mk/*.mk` fragments, `branding.h` generation, `make format`/`format-check`, the
  `tests/host/` host-test framework, and `image`/`test`/`test-full`/`run`/`run-bios`/`debug`/
  `gdb` wired to the existing boot harness.
- M1.2 (PR open): bongOS boots for the first time. `tools/mkimage` hand-builds a real GPT disk
  image (our own protective-MBR/GPT/CRC32 code) with a FAT32 ESP and a root partition under a
  newly-minted type GUID (D-056); a from-scratch UEFI loader built on our own minimal UEFI
  headers (D-007) prints a banner to ConOut and COM1, then halts. `make test` boots it under OVMF
  and checks the serial output (D-057's `--expect-serial`, a bridge until M1.3's kernel brings
  the real KTEST protocol). Reviewed (`architect` + two `reviewer` rounds, see
  `docs/logs/M1.2.md`); PR open, `needs-owner: yes`.

## Next step
Once M1.2's PR is reviewed/merged by the owner, start M1.3 (kernel skeleton + real handoff)
following the session protocol in CLAUDE.md.

## Blockers
_(none)_

## Questions for owner
_(none)_

## Waiting on owner (hardware checks and other owner-only steps)
- M1.2's PR is labeled `needs-owner` (it touches on-disk formats -- the GPT partition layout and
  D-056's new type GUIDs -- and the UEFI boot loader/headers, which the M1.3 handoff will build
  on). Auto-merge is off; the owner needs to review and merge it.
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
