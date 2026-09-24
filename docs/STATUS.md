# bongOS status
_Main-line status. Parallel-lane sessions don't edit this file; they track progress in their own milestone log._

**Last updated:** 2026-09-24 (M1.1 merged; M1.2 in progress)

## Current milestone
M1.2: UEFI headers + hello loader + disk image (`needs-owner`), branch
`m1-2-uefi-hello-loader`. Plan and log in `docs/logs/M1.2.md`.

## Phase
1: Acapulco Gold

## What works
- Planning docs: ARCHITECTURE, DECISIONS, ROADMAP.
- M1.1 (merged, PR #1): the ARCHITECTURE §2 directory skeleton, the top-level `Makefile` +
  `mk/*.mk` fragments, `branding.h` generation, `make format`/`format-check`, the
  `tests/host/` host-test framework, and `image`/`test`/`test-full`/`run`/`run-bios`/`debug`/
  `gdb` wired to the existing boot harness (no-op until M1.2 produces a bootable image).

## Next step
M1.2's code is written and `make test`/`make format-check` pass locally (see
`docs/logs/M1.2.md`'s 10:30 entry for the full list). The `architect` subagent is reviewing the
on-disk-format pieces (GPT/mkimage, D-056, D-057) before this locks in -- next: act on its
findings, run the `reviewer` subagent on the full diff, fix Critical findings, then open the PR.

## Blockers
_(none)_

## Questions for owner
_(none)_

## Waiting on owner (hardware checks and other owner-only steps)
- M1.1's PR is labeled `needs-owner` (it touches `.github/workflows/release.yml` and adds
  READMEs under `kernel/mm/`, `kernel/sync/`, `kernel/include/uapi/`, `libs/crypto/`,
  `libs/bongfs/`, all in pr-policy's sensitive-path list, even though none of them carry real
  code yet). Auto-merge is off; the owner needs to review and merge it.
- M1.1 added `libclang-rt-18-dev` (host-test sanitizers) and `gdb` (`make gdb`) to
  `tools/ci/install-deps.sh`. The cloud environment's cached setup script needs re-running
  once (Environment settings -> re-run setup, or it picks it up on the next cache
  invalidation) for `make host-tests`/`make gdb` to work in a fresh session without CI's
  own `sudo bash tools/ci/install-deps.sh` step.

## Parallel lanes (informational; updated by the main line when lanes merge)
| Milestone | Branch | State |
|---|---|---|
