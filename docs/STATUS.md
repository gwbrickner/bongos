# bongOS status
_Main-line status. Parallel-lane sessions don't edit this file; they track progress in their own milestone log._

**Last updated:** 2026-09-24 (M1.1 done, PR open)

## Current milestone
None in progress. M1.1 is done and its PR is open (branch `claude/amazing-hamilton-ck5lbm`; see
`docs/logs/M1.1.md` for why this session used that branch instead of
`m1-1-repo-skeleton-build-ci`). **Next: M1.2: UEFI headers + hello loader + disk image**
(`needs-owner`), once M1.1 merges.

## Phase
1: Acapulco Gold

## What works
- Planning docs: ARCHITECTURE, DECISIONS, ROADMAP.
- M1.1: the ARCHITECTURE §2 directory skeleton, the top-level `Makefile` + `mk/*.mk` fragments,
  `branding.h` generation, `make format`/`format-check`, the `tests/host/` host-test framework,
  and `image`/`test`/`test-full`/`run`/`run-bios`/`debug`/`gdb` wired to the existing boot
  harness (no-op until M1.2 produces a bootable image). Reviewed (2 rounds, see
  `docs/logs/M1.1.md`); PR open, `needs-owner: yes`.

## Next step
Once M1.1's PR is reviewed/merged by the owner, start M1.2 (UEFI headers + hello loader + disk
image) following the session protocol in CLAUDE.md.

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
