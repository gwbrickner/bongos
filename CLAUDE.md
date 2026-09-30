# bongOS: instructions for Claude

bongOS is a from-scratch x86_64 desktop OS in C17 + NASM. Everything in the base system is
custom: bootloaders, kernel, libc, GUI, crypto. ARCHITECTURE §0 lists the only exceptions.

Main sessions run on **Sonnet**. Subagents that need deeper judgement run on **Opus** (roster
below). Follow these rules literally. When a rule and a document seem to disagree, stop and
ask (see "When something isn't specified"). Don't guess.

## Where things are
| File | What it is | When to read it |
|---|---|---|
| `docs/STATUS.md` | Where the main line is now, and the **Next step** | The start of every session (the SessionStart hook prints it) |
| `docs/ROADMAP.md` | Milestones: Needs, steps, **Done when** | Before starting a milestone, and before finishing it |
| `docs/ARCHITECTURE.md` | **The design source of truth.** It wins over code. | The sections your milestone touches (see its index) |
| `docs/DECISIONS.md` | `D-0xx` decision table, append-only | Before designing anything; after deciding anything |
| `docs/BUG_HUNTING.md` | Severity levels, triage, subsystem checklists | Before any debugging or bug fix |
| `docs/logs/M<p>.<n>.md` | The running log for each milestone | While working on that milestone |
| `docs/specs/` | Exact byte layouts and algorithms | When touching that format |
| `docs/AGENT_TASKS.md` | Optional low-priority side tasks for extra sessions | Only when the prompt points you at it |
| `AGENTS.md` | The rules for non-Claude agents, restricted to easy tasks the owner picks | Only if you edit it, or the owner asks about it |

The root has no copies of these files. Always use the `docs/` versions.

## Build and test
| Command | What it does |
|---|---|
| `make` / `make image` | Debug build (`-O1`, UBSan, `KERNEL_DEBUG`) and `build/bongos.img` |
| `make RELEASE=1` | Release build (`-O2`, no UBSan). **Run `make clean` before switching profiles.** Objects don't track flags, so without a clean you get a stale mixed build. |
| `make format` / `make format-check` | clang-format (4 spaces, 100 columns) |
| `make host-tests` | Host unit tests (`tests/host/`, ASan/UBSan) |
| `make test` | The quick boot matrix (`tests/harness/matrix.conf`) plus the GUI screenshot tests and the countdown smoke test |
| `make test-full` | The full matrix (`matrix-full.conf`), which includes a 3 GiB row |
| `make gui-test` / `make update-refs` (`FW=bios` for BIOS) | GUI tests only / regenerate reference PNGs (regenerating counts as weakening a test, see Hard rules) |
| `make analyze` | Clang static analyzer over the kernel. Report: `build/analyze/report.txt` |
| `make screenshot SHOT=<path>.png` | Boots `build/bongos.img` and saves the final screen as a PNG (`FW=bios` for BIOS) |
| `make debug` / `make gdb` / `make run` | **Humans only: interactive, never exit.** Agents use `tests/harness/run-qemu.sh ... --debug` (fault log, forces TCG) or `--gdb` + `gdb -batch` instead (BUG_HUNTING §4–5) |

- Harness: `tests/harness/run-qemu.sh --help`. Serial logs go to `build/logs/<name>.serial.log`.
- Tool setup: `tools/ci/install-deps.sh` (used by both the cloud environment and CI). If apt
  fails on an unrelated third-party PPA, move that file out of `/etc/apt/sources.list.d/` and
  rerun the script.
- **Only one QEMU-running job at a time.** Parallel runs collide on `build/run/` sockets and
  logs. Never run `make test` while a subagent is testing.
- **Timeouts:** `make test` takes about 2–3 minutes, longer than the default Bash timeout. Pass
  a 600000 ms timeout for `make test`, `make test-full`, release runs, and `make screenshot`.
- `make clean` deletes `build/logs/`. Read or copy the logs you need first.

## Subagents
| Agent | Model | Use it for |
|---|---|---|
| `architect` | Opus | **Before** implementing anything in paging, interrupts, SMP, scheduling, the syscall ABI, on-disk formats, the boot handoff, or crypto. Also when stuck (see "When stuck"). Read-only. |
| `bug-sweeper` | Opus | Deep verification: it tries to break the code, fixes the bugs it can reproduce, and gates the PR. It runs after risky steps and at the finish (see below). Edits and commits; doesn't push. |
| `reviewer` | Opus | The final diff review before the PR. Read-only. |
| `qemu-tester` | Haiku | Builds and runs tests, then summarizes the logs. Use it instead of reading long logs yourself. |
| `Explore` | Haiku | Fast read-only search: "where is X defined, and who calls Y" |
| `subsystem-hunter` | Sonnet | Used only by `/milestone-sweep`, one subsystem per run |

When you call a subagent, give it everything it needs in the prompt: the milestone ID, the
base ref, which files and functions changed, and what you want back. It starts with no memory
of this conversation. Run `bug-sweeper` and `qemu-tester` in the foreground, and don't start
other QEMU work while they run. Commit your own work before you invoke `bug-sweeper` or
`subsystem-hunter`: the tree must be clean, so their commits contain only their own changes.

**After an editing subagent returns** (`bug-sweeper`, `subsystem-hunter`), run
`git status --porcelain` and `git log --oneline -5` before anything else. Its work should all
be committed. Treat any uncommitted change as a suspected leftover mutation or half-finished
fix. Read the diff, and revert it (`git checkout -- <file>`; for an untracked file, read it and
then `rm <path>`) unless it's clearly an intended test, report, or fix. Never use `git clean`.
Never commit it blind.

## Session protocol

### Starting
1. Read the whole of `docs/STATUS.md`. If a milestone is in progress, **resume it** from its
   **Next step**. Don't restart it.
2. Otherwise, pick the lowest-numbered unchecked ROADMAP milestone whose Needs are all done (or
   the one the owner named). Then:
   1. Create the branch `m<p>-<n>-<slug>` from an up-to-date `origin/main`.
   2. Copy `docs/logs/TEMPLATE.md` to `docs/logs/M<p>.<n>.md`, and fill in the Plan. Mark which
      steps are **risky** (see Working).
   3. Update STATUS.md (Current milestone and Next step), then commit and push.
3. **Parallel lanes** (`[parallel-ok]` milestones the owner started in a separate session): the
   same process, but **never edit STATUS.md**. The milestone log is your status file.

### Working
- Work one roadmap step at a time. Before writing code, read the ARCHITECTURE sections and the
  `D-0xx` entries it depends on. Match the surrounding code's style and comment density.
- **Risky steps** touch paging or TLB, interrupts or traps, assembly, allocators, locking, SMP,
  the scheduler, syscalls, the boot ABI, on-disk formats, or crypto. For those:
  1. Consult `architect` before you implement.
  2. When the step builds and passes `make test`, run `bug-sweeper` in **step** mode on the
     step's diff. Fix everything it reports before starting the next step.
- Write tests alongside the code, not after it. Each ROADMAP **Done when** clause needs a test
  that fails if the feature breaks. `make test` greps for required `KTEST PASS` lines
  (`mk/test.mk` `_check-ktest-pass`), so add new required ktests there.
- Never claim something works unless a command you ran in this session showed it. Quote the
  command and its result in the log.

### Progress protocol (HARD RULE: sessions can die at any moment)
After **every working step**, and **at least every ~30 minutes**:
1. Append an entry to `docs/logs/M<p>.<n>.md` (use the template's entry format).
2. Update STATUS.md: Current milestone, the precise **Next step**, Blockers, and Questions.
   Parallel lanes update the log's Next step instead.
3. Commit (`M<p>.<n>: <what>`) and push.

- **Work in progress is allowed on milestone branches.** A commit may have failing tests. If it
  does, its message starts with `M<p>.<n>: WIP`, and the log entry names what fails and why.
  Only the PR is gated (see Finishing).
- The **Next step** must let a fresh session continue with no memory of this one. Name the
  file, the function, the failing test, and the command that shows the failure.
- Never end a turn with uncommitted or unpushed work. The Stop hook checks this.

### Finishing a milestone (the PR gate: every item, in order)
1. **Tests green.** `qemu-tester` runs `make format-check`, `make host-tests`, `make test`, and
   `make test-full` on HEAD. Fix any failure before going on.
2. **Deep sweep.** Run `bug-sweeper` in **finish** mode against `origin/main`. It checks both
   build profiles and the full matrix, runs `make analyze`, and tries to break every changed
   function. Fix whatever it leaves open, then run it again, until it returns `SWEEP: PASS`.
   If two rounds in a row return FAIL with the same open items, stop and follow "When stuck".
   Never open the PR on a FAIL.
3. **Review.** `reviewer` reviews `git diff origin/main...HEAD`. Fix every **Critical**, then
   review again. Fix every **Should-fix**, or write down in the log why not. If any code
   changed after the sweep passed, repeat step 1 and run `bug-sweeper` again in step mode on
   those changes.
4. **Screenshot.** Run `make screenshot SHOT=docs/screenshots/M<p>.<n>.png`. If the milestone
   touches the BIOS path, also run
   `make screenshot FW=bios SHOT=docs/screenshots/M<p>.<n>-bios.png`. Open each PNG with Read
   and check that it shows what the milestone should (for example, the new boot output).
   Commit them.
5. **Paperwork.** Check the milestone's box in ROADMAP.md. Write the log's **Summary** (it
   becomes the release notes). Update STATUS.md: done milestones get one line each, then the
   next milestone and any owner checks. Commit and push.
6. **PR.** Fill in `.github/pull_request_template.md`:
   - Title: `M<p>.<n>: <title>`.
   - The lines `Sweeper: PASS (docs/sweeps/M<p>.<n>.md)` and `Reviewer: PASS` go in, each on a
     line of its own and exactly as written, only when they're true.
     `.github/workflows/pr-policy.yml` auto-merges only when both lines match exactly.
   - Embed the screenshot with a commit-pinned URL:
     `![M<p>.<n>](https://github.com/gwbrickner/bongos/blob/<commit-sha>/docs/screenshots/M<p>.<n>.png?raw=true)`
   - Set `needs-owner: yes` if the roadmap marks the milestone that way, or if it touches a
     sensitive area: memory, interrupts/SMP, the scheduler, security/crypto, on-disk formats,
     the boot ABI, `.github/`, or `.claude/`.
7. **Owner hardware checks** never block merging. Write step-by-step instructions in the log,
   and list them in STATUS.md under "Waiting on owner".

### When something isn't specified
- **A small, local choice:** decide it, then add a `D-0xx` entry to DECISIONS.md (the next
  free number, one table row).
- **An architectural choice** (ABIs, on-disk formats, security, or other subsystems): ask
  `architect`, record the decision, update ARCHITECTURE.md in the same PR, and mark it
  `needs-owner: yes`.
- **Truly ambiguous, or it contradicts the docs:** add it to STATUS.md's "Questions for owner",
  and continue with the parts that aren't blocked.

### When stuck
- The same failure after 2 fix attempts: stop patching and follow `docs/BUG_HUNTING.md` §8
  (reproduce, minimize, root-cause). If you still can't explain it, ask `architect`, and give
  it the repro command, the log excerpt, and what you already tried.
- Still failing after the architect's advice plus 2 more attempts: **stop.** Put your findings,
  hypotheses, and exact repro steps under Blockers in STATUS.md, then commit and push.

## Conventions (full rules: ARCHITECTURE §4)
- **Names:** camelCase functions and variables, PascalCase types, UPPER_SNAKE constants and
  macros. Non-static functions get subsystem prefixes (`pmmAllocPages`, `vfsOpen`).
- **Never put the OS name in code.** It lives only in `branding/`.
- **Errors:** kernel functions return `Status`, and negative means error. The kernel has no
  errno. Check every `Status` you receive.
- **Contracts:** every non-static kernel function gets a comment: locks held, may-sleep,
  IRQ-safe, and failure modes.
- **Placement:** x86 specifics only in `kernel/arch/x86_64/`. Assembly only in `arch/` and
  `boot/`.

## Hard rules
- No third-party code in the base system. Ports go in `ports/`, and ARCHITECTURE §0 lists the
  allowed data files.
- Never weaken, skip, or delete a test to get a pass. That includes regenerating GUI reference
  PNGs, loosening tolerances, and dropping `_check-ktest-pass` lines. If a test itself is wrong,
  fix it and explain why in the log.
- Never force-push. Never rewrite `main`. Never push to `main` directly.
- The real-disk write guard (ROADMAP safety rule) stays on by default, always.
- Crypto stays labeled **EXPERIMENTAL** in docs and user-facing text.
- Never commit secrets or private keys. Signing keys belong to the owner (and to CI secrets).

## Bug hunting
Every debugging session and bug fix follows `docs/BUG_HUNTING.md`. After a milestone merges,
the owner may run `/milestone-sweep <M<p>.<n>>` for a sweep across all subsystems.
