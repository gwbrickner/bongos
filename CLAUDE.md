# bongOS: instructions for Claude

bongOS is a from-scratch x86_64 desktop OS in C17 + NASM. Everything in the base system is
custom: bootloaders, kernel, libc, GUI, crypto. The only exceptions are listed in
ARCHITECTURE §0.

## The documents (read before acting)
- `docs/ARCHITECTURE.md`: **the source of truth for design.** Read the sections your milestone touches.
- `docs/DECISIONS.md`: every decision and why. Append-only.
- `docs/ROADMAP.md`: milestones with steps and "Done when" checks.
- `docs/STATUS.md`: where the main line stands right now. The SessionStart hook prints it.
- `docs/logs/M<p>.<n>.md`: the running log for each milestone.

## Session protocol

### Starting
1. Read STATUS.md. If a milestone is in progress, **resume it** from its "Next step". Don't restart it.
2. Otherwise, pick the lowest-numbered unchecked ROADMAP milestone whose Needs are done (or the one the owner named). Then:
   - create branch `m<p>-<n>-<slug>`
   - copy `docs/logs/TEMPLATE.md` to `docs/logs/M<p>.<n>.md` and fill in the plan
   - update STATUS.md
   - commit and push
3. **Parallel lanes** (`[parallel-ok]` milestones the owner started in a separate session): same process, but **don't edit STATUS.md**. Your milestone log is your status file. This avoids merge conflicts with the main line.

### Progress protocol (HARD RULE: sessions can die from usage limits at any moment)
After **every working step** (something compiles, a test passes, a sub-step of the milestone
is done), and **at least every ~30 minutes of work**:
1. Append an entry to `docs/logs/M<p>.<n>.md` covering what changed, why, test results, and the next step.
2. Update `docs/STATUS.md`: the in-progress item, the precise **Next step**, blockers, and questions. (Parallel lanes: update the "Next step" in your log instead.)
3. `git commit` with a message like `M<p>.<n>: <what>`, then `git push`.

Never end a turn with uncommitted or unpushed work. The Stop hook checks this. The "Next
step" must be specific enough that a fresh session with no memory can continue from it:
name the file, the function, and the failing test.

### Finishing a milestone
1. All "Done when" checks pass, run by the `qemu-tester` subagent (`make test`, plus `make test-full` or `make host-tests` where the milestone says so).
2. The `reviewer` subagent reviews the diff. Fix every **Critical** finding, then re-review. Fix the Should-fix items, or explain in the log why not.
3. Check the milestone's box in ROADMAP.md. Write the log's **Summary** (it becomes the release notes). Update STATUS.md: next milestone, and any owner hardware checks pending.
4. Open the PR using `.github/pull_request_template.md`:
   - title `M<p>.<n>: <title>`
   - include `Reviewer: PASS` only if there are no open Critical findings
   - set `needs-owner: yes` if the roadmap marks the milestone that way or it touches a sensitive area
5. **Owner hardware checks** never block merging. Write clear step-by-step instructions in the log and add them to STATUS.md under "Waiting on owner".

### When something isn't specified
- **Small, local choice:** decide, then add a `D-0xx` entry to DECISIONS.md.
- **Architectural choice** (affects ABIs, on-disk formats, security, or other subsystems): ask the `architect` subagent. Record the decision, update ARCHITECTURE.md, and mark the PR `needs-owner: yes`.
- **Truly ambiguous, or contradicts the docs:** add it under "Questions for owner" in STATUS.md, then continue with the parts that aren't blocked.

### When stuck (budget hygiene)
- The same failure after 2 fix attempts: ask the `architect` subagent.
- After the architect's advice plus 2 more attempts: **stop.** Write your findings, hypotheses, and exact repro steps under Blockers in STATUS.md, then commit and push. Don't burn the session looping.

## Build and test
(These Make targets come into being in M1.1.)
- `make`, `make image` (produces `build/bongos.img`), `make RELEASE=1`
- `make host-tests`
- `make test` (quick boot matrix from `tests/harness/matrix.conf`)
- `make test-full`
- `make debug` (QEMU `-d int,cpu_reset` log), `make gdb`
- `make format`, `make format-check`
- Boot harness: `tests/harness/run-qemu.sh --help`
- Tool setup: `tools/ci/install-deps.sh` (the cloud environment and CI both use it)

## Conventions (full rules in ARCHITECTURE §4)
- **Naming:** camelCase functions and variables, PascalCase types, UPPER_SNAKE constants and macros.
- **Prefixes:** subsystem prefixes like `pmmAllocPages` or `vfsOpen`. **Never the OS name in code.** The name lives only in `branding/`.
- **Errors:** kernel functions return a `Status` (negative means error). No errno in the kernel.
- **Contracts:** every non-static kernel function gets a contract comment (locks, may-sleep, IRQ-safe).
- **Placement:** x86 specifics only in `kernel/arch/x86_64/`. Assembly only in `arch/` and `boot/`.

## Hard rules
- No third-party code in the base system. Ports live in `ports/`; data files are listed in ARCHITECTURE §0.
- Never weaken, skip, or delete a test to get a pass. If a test itself is wrong, fix it and explain why in the log.
- Never force-push. Never rewrite `main`.
- The real-disk write guard (ROADMAP safety rule) stays on by default, always.
- Crypto stays labeled **EXPERIMENTAL** in docs and user-facing text.
- Never commit secrets or private keys. Signing keys belong to the owner (and to CI secrets).

## Subagents
- `architect` (Opus, read-only): designs and root-cause diagnosis. Consult it **before** implementing anything in paging, interrupts, SMP, scheduling, the syscall ABI, on-disk formats, or crypto. Also consult it when stuck.
- `reviewer` (Opus, read-only): reviews the milestone diff before the PR.
- `qemu-tester` (Haiku): builds, runs tests, and summarizes the logs. Use it instead of reading long logs yourself.
- `Explore` (Haiku): fast read-only codebase search.
