# bongOS status
_Main-line status. Parallel-lane sessions don't edit this file; they track progress in their own milestone log._

**Last updated:** 2026-09-24 (M1.1 in progress)

## Current milestone
M1.1: Repo skeleton, build system, CI. Branch `claude/amazing-hamilton-ck5lbm` (see
`docs/logs/M1.1.md` for why this session used that branch instead of `m1-1-repo-skeleton-build-ci`).
Log: `docs/logs/M1.1.md`.

## Phase
1: Acapulco Gold

## What works
- Planning docs: ARCHITECTURE, DECISIONS, ROADMAP.

## In progress
- M1.1: building the directory skeleton, top-level Makefile + `mk/*.mk` fragments, and the
  `tests/host/` runner. See `docs/logs/M1.1.md` for the plan and running log.

## Next step
Continue M1.1 per its "Next step" in `docs/logs/M1.1.md`.

## Blockers
_(none)_

## Questions for owner
_(none)_

## Waiting on owner (hardware checks and other owner-only steps)
- M1.1 added `libclang-rt-18-dev` (host-test sanitizers) and `gdb` (`make gdb`) to
  `tools/ci/install-deps.sh`. The cloud environment's cached setup script needs re-running
  once (Environment settings -> re-run setup, or it picks it up on the next cache
  invalidation) for `make host-tests`/`make gdb` to work in a fresh session without CI's
  own `sudo bash tools/ci/install-deps.sh` step.

## Parallel lanes (informational; updated by the main line when lanes merge)
| Milestone | Branch | State |
|---|---|---|
