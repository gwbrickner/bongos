---
name: milestone-sweep
description: Full bongOS bug sweep across every implemented subsystem after a milestone. Only run when the user explicitly invokes /milestone-sweep.
---

# Milestone Sweep

Milestone name: $ARGUMENTS
If no name was given, use the most recent tag: `git describe --tags --abbrev=0`.

You are the **coordinator**. You do not hunt bugs yourself. You set up, delegate each subsystem to the `subsystem-hunter` subagent, and write the summary. This keeps each subsystem's deep work in its own fresh context.

## 1. Setup
1. Read `docs/BUG_HUNTING.md` in full.
2. Create branch `sweep/<milestone>` from the current HEAD.
3. Create `docs/sweeps/<milestone>/`.

## 2. Baseline (stop if this fails)
1. Build debug and release profiles.
2. Build and run the `ktest` ISO headless in QEMU (§4.1 flags + `isa-debug-exit`, TCG, 120 s timeout).
3. Run static analysis (§7.4). Save raw output to `docs/sweeps/<milestone>/static-analysis.txt`.
4. Write `docs/sweeps/<milestone>/baseline.md`: build status, ktest pass/fail counts, static analysis finding counts.

If the build fails or any existing `ktest` fails, **stop here**, commit baseline.md, and report. Sweeping a broken build wastes the run.

## 3. Scope
1. For each subsystem in §6 (boot, int, pmm, vmm, heap, sched, syscall, drivers, smp), check whether it's actually implemented. Mark missing ones "not yet implemented" and skip them.
2. Find the previous milestone tag and run `git diff --stat <prevTag>..HEAD` to see which subsystems changed.
3. Order: **changed subsystems first**, then the rest in §6 order.
4. Write the plan to `docs/sweeps/<milestone>/plan.md`.

## 4. Delegate
For each subsystem in the plan, **one at a time, never in parallel** (fixes can touch shared headers, and concurrent QEMU runs collide):

Invoke the `subsystem-hunter` subagent with:
- subsystem name and its §6 section number
- milestone name
- report path: `docs/sweeps/<milestone>/<subsystem>.md`
- relevant static analysis findings for that subsystem's files

Wait for it to return before starting the next. If a subagent reports it couldn't finish, record that in the plan and move on. Retry at most once.

## 5. Cross-cutting pass
After all subsystems: rebuild the `ktest` ISO and run the config matrix from §4.2 (minimum: RAM 64M and 4G, `-smp 1` and `-smp 4`, debug and release). Write results to `docs/sweeps/<milestone>/matrix.md`. Any new failure here gets logged as a bug but **not fixed** in this run.

## 6. Summary
Write `docs/sweeps/<milestone>/SUMMARY.md` containing:
1. Table per subsystem: items verified / unverified / bugs by severity / fixes committed / tests added.
2. All open S1 and S2 bugs with one-line descriptions.
3. The §12 release gate checklist marked pass/fail.
4. Unverified items and why (these are the next session's to-do list).

Commit all reports. **Do not merge into main.** The user reviews the sweep branch first.

## Hard limits
- No refactoring, no new features, no style cleanups.
- Stop after the summary. Do not start extra work.
