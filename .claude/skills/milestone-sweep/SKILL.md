---
name: milestone-sweep
description: Full bongOS bug sweep across every implemented subsystem after a milestone. Only run when the user explicitly invokes /milestone-sweep.
---

# Milestone sweep

Milestone: `$ARGUMENTS` (a milestone ID like `M2.4`). If none was given, use the most recent
milestone merged to main: the newest `git log origin/main --format=%s` subject that matches
`^M[0-9]+\.[0-9]+:`.

You are the **coordinator**. You don't hunt bugs yourself. You set up the sweep, hand each
subsystem to the `subsystem-hunter` subagent, and write the summary. That way each subsystem's
deep work runs in its own fresh context. This complements the per-milestone `bug-sweeper`
(which covers one diff); this skill covers **everything that exists**.

Reports go to `docs/sweeps/<milestone>-full/` (`<dir>` below).

## 1. Setup
1. Read `docs/BUG_HUNTING.md` and `CLAUDE.md` (Build and test) in full.
2. `git fetch origin main`, then create the branch `sweep/<milestone>` from `origin/main`.
3. `mkdir -p <dir>`.

## 2. Baseline (stop if this fails)
1. `make clean && make`, then `make format-check`, `make host-tests`, and `make test-full`.
2. `make clean && make RELEASE=1 && make RELEASE=1 test`, then `make clean && make`.
3. `make analyze`, then copy `build/analyze/report.txt` to `<dir>/static-analysis.txt`.
4. Write `<dir>/baseline.md`: the build status for each profile, the ktest and host-test
   pass/fail counts (count the `KTEST PASS` lines in `build/logs/*.serial.log`), and the
   analyzer warning count for each file.

If any build or existing test fails, **stop here**. Commit `baseline.md`, push, and report.
Sweeping a broken build wastes the run.

## 3. Scope
1. For each §6 subsystem, check whether it's actually implemented (use the path map in
   BUG_HUNTING §6). Mark the missing ones "not yet implemented" and skip them.
2. Find the previous milestone's merge commit (the next-older `M<p>.<n>:` subject on
   `origin/main`) and run `git diff --stat <thatCommit>..origin/main` to see which subsystems
   changed.
3. Order: **changed subsystems first**, then the rest in §6 order.
4. Write the plan to `<dir>/plan.md`, then commit and push.

## 4. Delegate
Take the subsystems in the plan **one at a time, never in parallel**. Fixes can touch shared
headers, and concurrent QEMU runs collide.

Invoke `subsystem-hunter` with:
- the subsystem name, its §6 section number, and its source paths
- the milestone ID
- the report path `<dir>/<subsystem>.md`
- the analyzer findings from `static-analysis.txt` that fall in its files

Wait for it to return. Then commit anything it left uncommitted and push, before starting the
next subsystem. If a hunter reports it couldn't finish, record that in the plan and move on.
Retry at most once.

## 5. Cross-cutting pass
After all the subsystems: `make clean && make`, then `make test-full`, then the release
profile (`make clean && make RELEASE=1 && make RELEASE=1 test`), then `make clean && make`.
Write the results to `<dir>/matrix.md`. Any new failure here gets logged as a bug but **not
fixed** in this run.

## 6. Summary
Write `<dir>/SUMMARY.md` containing:
1. A table per subsystem: items verified / unverified / bugs by severity / fixes committed /
   tests added.
2. Every open S1 and S2 bug, one line each.
3. The §12 release gate checklist, each item marked pass or fail.
4. The unverified items and why. These become the next session's to-do list.

Commit and push all the reports. **Don't merge into main.** The owner reviews the sweep
branch first. Offer to open a PR titled `Sweep <milestone>` with `needs-owner: yes`.

## Hard limits
- No refactoring, no new features, no style cleanups.
- Stop after the summary. Don't start extra work.
