# Side-task backlog for AI agents
_Low-priority work that can run beside the main line. Everything here is optional: none of it
blocks a milestone. Claude sessions read `CLAUDE.md` first; other agents read `AGENTS.md` first (its "Your tasks"
table is the owner's chosen subset of Tier A). This file only adds to them. Where they
disagree, the agent's own file wins. Written 2026-09-30, when the main line was about to start M2.6._

## 1. Ground rules for every task here
1. **Check nobody has the task.** Run `git fetch origin` and `git ls-remote --heads origin`. A
   branch named after the task or milestone (for example `m12-3-font-engine`) means someone
   started it: pick another task, or resume that branch only if the owner told you to.
2. **Stay off the main line's files.** The main line is on M2.6 (KASLR + RNG). Until it merges,
   don't edit `boot/`, `kernel/arch/`, `kernel/mm/`, `mk/test.mk`, `mk/image.mk`,
   `tests/harness/`, or `docs/STATUS.md`. Merge conflicts there cost the main line time.
   `docs/STATUS.md` is never edited by a side task (it is the parallel-lane rule).
3. **One task = one branch = one PR.** Use the branch the session gave you. If you weren't given
   one, name it `chore-<slug>` (or `m<p>-<n>-<slug>` for a roadmap milestone). Never push to
   `main`, never force-push.
4. **Keep a log.** Copy `docs/logs/TEMPLATE.md` to `docs/logs/<task-id>.md` (a roadmap milestone
   uses its normal `M<p>.<n>.md`). Add an entry after every step, then commit and push, as in
   CLAUDE.md "Progress protocol". The log is your status file. Put the "Next step" in it so a
   fresh session can resume.
5. **No test weakening.** Don't skip, delete, loosen or regenerate a test to get green. New
   behavior gets a test that fails when the behavior breaks.
6. **Decisions.** A small local choice gets a `D-0xx` row in `docs/DECISIONS.md`. Use the next
   free number at the time you commit, and expect to renumber on merge (D-120..D-139 are
   reserved for the main line; D-140..D-147 are taken by M12.2). An architectural choice
   (ABI, on-disk format, security, boot handoff) is not a side task: stop, write it under
   "Questions for the owner" in your log, and pick a different task.
7. **Never claim what you didn't run.** Quote the command and its result in the log.
8. **Before opening a PR**, run at least `make format-check` and `make host-tests`. If you touched
   kernel or boot code, also run `make test`. Only one QEMU job at a time. Open the PR only if
   the owner asked for one; otherwise leave the branch pushed and say so in the log.
9. **Crypto stays labeled EXPERIMENTAL.** Never add the OS name to code (only `branding/`).

## 2. Tier A: small, safe, start any time
These need no owner decision and touch no risky area.

### A1. Triage the `make analyze` warnings
- **Where:** STATUS.md "Open leads". `make analyze` on main reports 5 warnings:
  `kernel/include/list.h:50` (a possible NULL `prev` dereference in `listRemove`),
  `kernel/test/kmalloc_test.c:68,143,365`, `kernel/test/pmm_test.c:405`.
- **Do:** run `make analyze`, read `build/analyze/report.txt`, and decide for each warning:
  real bug (fix it and add a regression test that failed before), or false positive / deliberate
  misuse in a test. For the latter, prefer a narrow annotation or a small restructure that
  keeps the test's intent. Never delete or weaken the test.
- **Done when:** every warning is fixed or has a written reason in your log, and
  `make analyze` reports fewer warnings (or the remainder are each documented). `make test`
  still passes. `list.h` is used everywhere, so run the full quick matrix.
- **Watch out:** `list.h` is shared with the main line. Keep the change tiny.

### A2. Unit tests for `tools/mkfont`
- **Why:** `tools/mkfont/main.c` parses the console-font grid and writes PSF2 files, and it has no
  test in `tests/host/` (imgdiff, ksyms and mkimage all do). `make font-check` catches drift
  but not bad-input handling.
- **Do:** add `tests/host/mkfont_test.c` in the style of `tests/host/ksyms_test.c`. Cover:
  a valid grid; ragged rows; wrong glyph count; bad characters; the PSF2 header validation in
  `mkfont c`; truncated PSF2 input. If a function can't be linked into the test as written,
  do the smallest refactor (split parsing from `main`) and keep the CLI's behavior the same.
- **Done when:** `make host-tests` passes with the new tests, and a deliberate one-line break in
  the parser makes at least one of them fail (say which, in the log).

### A3. Share the duplicated compression tables and CRC-32
- **Why:** the M12.2 reviewer left three follow-ups (`docs/logs/M12.2.md`, Verification):
  the LENGTH/DIST tables exist in both `libs/compress/inflate.c` and `deflate.c`; imgdiff's PNG
  reader duplicates part of `gfxPngDecode`; CRC-32 exists three times.
- **Do only the safe two:** move the LENGTH/DIST tables into one internal header inside
  `libs/compress/`, and (if it stays small) make `tools/imgdiff` use the shared PNG chunk code.
  **Do not touch** the loader's or `tools/mkimage`'s CRC-32: sharing it needs `mk/image.mk`
  and boot-size decisions (owner/main line).
- **Done when:** `make host-tests` passes unchanged and the golden PNG results are byte-identical.
- **Depends on:** the M12.2 branch being merged (or branch from it). Check first.

### A4. Documentation drift audit (docs only)
- **Do:** read `docs/ARCHITECTURE.md` against `docs/DECISIONS.md`, the code and the Makefile,
  and list statements that are now false or stale (for example, sections that a later `D-xxx`
  supersedes, commands that changed, file paths that moved). Fix the small ones. For each
  disagreement you can't settle, add it to your log under "Questions for owner"; don't guess.
  ARCHITECTURE wins over code, so don't "fix" the doc to match a code bug.
- **Check specifically:** `README.md`, `SETUP.md`, the per-directory `README.md` files
  (`libs/*`, `data/*`, `tests/host/README.md`), and the ARCHITECTURE §2 repository layout
  against the real tree (`libs/`, `tools/`, `data/`).
- **Done when:** a list of corrections in the log, and the small ones committed. This is a good
  first task for a new session: it teaches the layout at no risk.

### A5. Shell and CI tooling hygiene
- **Do:** run `shellcheck` (install it if missing) over `tools/`, `tests/harness/` and
  `.claude/hooks/`, and fix warnings that don't change behavior. Confirm `tools/ci/install-deps.sh`
  is idempotent (a second run doesn't fail). Don't touch `.github/` or `.claude/`; they are
  `needs-owner` areas. Report problems there in your log instead.
- **Done when:** shellcheck is clean or each remaining warning is explained, and `make test` still
  passes if you edited `tests/harness/` (see rule 2 about `tests/harness/`; only do that part
  after M2.6 merges).

## 3. Tier B: host-only libraries (`[parallel-ok]` roadmap milestones)
These are real roadmap milestones that need only the host toolchain. ROADMAP says the owner names
which one runs, so **start one only if your prompt names it**, and follow CLAUDE.md "Finishing a
milestone" in full (both `bug-sweeper` and `reviewer` gates, a screenshot, the ROADMAP box).
Parallel lanes never edit STATUS.md.

| Milestone | Needs | Start now? | Notes |
|---|---|---|---|
| M12.3 Font engine | M1.1 | Branch `m12-3-font-engine` already exists on origin: resume it, don't restart. | Needs OFL fonts in `data/fonts/` with license files. |
| M12.7 JPEG + GIF decoders | M12.2 | Yes, once M12.2 is merged (branch from it if not). | Extend `libs/gfx/gfx-image.h`; follow D-146 (limits, allocation accounting, clean failure on malformed input) and D-147 (goldens). |
| M14.3 audio decoders (WAV, FLAC, MP3, Vorbis) | M1.1 for the decoders | Yes, decoders only (the player app needs M14.2). | `libs/audio`; compare PCM against reference decodes made by an independent encoder script (as M12.2 did with Python). |
| M16.1 HTML parser | M11.2 for the HTTP client | The parser part only. | WHATWG-lite tokenizer and tree builder; html5lib subset in host tests. |
| M11.1 Crypto primitives | M1.1 | Yes, but it is `needs-owner`. | Crypto: consult `architect` first, keep it labeled EXPERIMENTAL, official test vectors (ARCHITECTURE §17), a constant-time check for `cryptoEqual`. Its last step touches the kernel CSPRNG that M2.6 is writing: **leave that step for after M2.6 merges.** |
| M5.7, M7.6, M9.1 | M5.3, M7.5, M3.7 | No: their Needs aren't done. | Revisit when the main line gets there. |

Habits that worked on M12.2 (copy them):
- Ask `architect` for the design before coding, and record decisions as you go.
- Golden images are exact matches; never auto-create a reference (`tests/host/README.md`).
- Test decoders against fixtures made by an **independent** encoder, plus mutation fuzzing and
  allocation-failure sweeps, so a bug in your code can't hide behind a matching bug in the test.
- Floating point is fine in userland/host libs but never in the kernel (`-mgeneral-regs-only`).

## 4. Tier C: leave for after M2.6 merges
These are good work but conflict with the main line or need a risky-step process. Don't start
them in a side session unless the owner says so.

- **M2.4's Done-when ktests** (slab, kmalloc, vmalloc) aren't in `mk/test.mk`'s
  `_check-ktest-pass` list. The main line adds them in M2.6's first commit. Don't duplicate it.
- **D-114 deferrals in the BIOS loader** (`boot/bios/`): a protected-mode diagnostic IDT in
  stage2, and dual teletype+serial logging before VBE is up. Both are boot assembly/C on a
  path M2.6 modifies; both need `architect` first.
- **CRC-32 shared between loader, mkimage and libs/compress** (see A3): needs `mk/image.mk`.
- **`BootInfo.bootDiskGuid` / `bootPartGuid`** (D-056): an owner question is open in STATUS.md.

## 5. How to report back
When you stop, the state must be recoverable by someone with no memory of your session:
1. Your log has a final entry: what's done, what isn't, the exact **Next step** (file, function,
   failing test, command), and any questions for the owner.
2. Everything is committed (`<task-id>: <what>`, or `<task-id>: WIP ...` if a test still fails)
   and pushed to your branch.
3. If the task is finished, mark it here in your PR: add `— done in <branch>` to the task
   heading. That one-line edit to this file is the only edit to it you make.
