# Owner setup and session playbook

Do Part A once. After that, every session is just a prompt from Part C.

---

## Part A: one-time setup (about 20 minutes)

### A1. Claim the credit
Claim the Claude Code cloud credit **by October 7** (it expires November 4): go to
`claude.ai/code/claim-credit`, or run `/claim-credit` in the Claude Code CLI.

### A2. Create the repo
1. Create a **public** GitHub repo named `bongos` with no README, license, or `.gitignore` (this kit has all of them).
2. Unzip this kit and push it:
   ```sh
   cd bongos
   git init -b main
   git add -A
   git commit -m "Planning: architecture, roadmap, tooling"
   git remote add origin git@github.com:<you>/bongos.git
   git push -u origin main
   ```

### A3. GitHub settings (needed for the auto-merge policy)
1. **Settings → General → Pull Requests:** turn on **Allow auto-merge** and **Automatically delete head branches**.
2. **Settings → Actions → General → Workflow permissions:** choose **Read and write permissions**.
3. **Settings → Rules → Rulesets → New branch ruleset** for `main`:
   - require a pull request before merging (0 approvals, so auto-merge works)
   - require the status check **`ci`** to pass
   - block force pushes

   You're the admin, so you can still merge `needs-owner` PRs yourself.
4. **Recommended: add a bot token so merges trigger releases.** Merges done with GitHub's
   default token don't start other workflows, so without this, releases won't publish on
   their own.
   1. Create a **fine-grained personal access token** limited to this repo, with **Contents: read/write** and **Pull requests: read/write**.
   2. Save it as the repo secret **`BOT_TOKEN`** (Settings → Secrets and variables → Actions).

   Without the token, run the `release` workflow by hand (Actions → release → Run workflow → `M1.4`).

### A4. Cloud environment at claude.ai/code
1. Connect GitHub, and give Claude access to the `bongos` repo.
2. Create an environment called **bongos**:
   - **Setup script:** paste the whole of `tools/ci/install-deps.sh`. It gets cached after the first run.
   - **Network access:** the default or trusted level is enough, since it has to reach the Ubuntu package mirrors and GitHub. Later, M14.5 (ports) needs to download upstream source tarballs; switch to broader access then. The milestone will remind you.
3. **Model:** pick **Sonnet** for sessions. The `architect` and `reviewer` subagents automatically use Opus for the hard parts, and `qemu-tester`/`Explore` use Haiku. While a subagent is running, `/tasks` shows which model it's actually on.

---

## Part B: how the loop works
- Each session does **one milestone**: on its own branch, logging progress, committing and pushing after every working step, and finishing with a PR.
- **Normal PRs** auto-merge when CI passes. **`needs-owner` PRs** wait for you. Review those on GitHub, merge them, and start the next session.
- **When a session dies** (credit or usage limits, network, anything else), start a new one with the resume prompt. It reads `docs/STATUS.md` and the milestone log, and continues from "Next step".
- **Things only you can do** show up in `docs/STATUS.md` under **Waiting on owner** (hardware checks, key generation, ACPI dumps), with step-by-step instructions.

---

## Part C: prompts to paste

### C1. The very first session
```
Read CLAUDE.md, then docs/STATUS.md, docs/ROADMAP.md, and the ARCHITECTURE.md sections that
M1.1 touches. Start milestone M1.1, following the session protocol in CLAUDE.md exactly
(branch, milestone log, STATUS.md, commit and push after every working step). Work until
every "Done when" check passes, run the reviewer subagent, then open the PR with the
template.
```

### C2. Every session after that
```
Continue bongOS. Follow the session protocol in CLAUDE.md: if docs/STATUS.md shows a
milestone in progress, resume it from "Next step"; otherwise start the next eligible
milestone in docs/ROADMAP.md. Finish it through the PR.
```

### C3. A parallel lane (optional; runs alongside the main line)
```
Work on parallel-lane milestone M<p>.<n> from docs/ROADMAP.md (it's marked [parallel-ok]).
Follow CLAUDE.md's parallel-lane rules: your own branch and milestone log, and do NOT edit
docs/STATUS.md. Finish it through the PR.
```
Good parallel candidates once their Needs are done: M11.1 (crypto), M12.2 (gfx), M12.3
(fonts), M7.6 (bongfs tools, after the spec is approved), M14.3 (audio decoders), and the
host part of M9.1 (AML). Keep it to one or two lanes at a time; every session draws from the
same credit.

### C4. Resuming after a crash, a limit, or a closed session
```
Resume bongOS. Read docs/STATUS.md (for a parallel lane, read docs/logs/M<p>.<n>.md) and
continue exactly from "Next step". Don't redo finished steps.
```

### C5. Reporting a hardware check result
```
Owner hardware check for M<p>.<n>: <what you saw, e.g. "boots to menu at 2560x1440, kernel
banner shows">. Record it in the milestone log, remove it from "Waiting on owner" in
STATUS.md, and fix anything that failed.
```

---

## Part D: flashing a USB stick for hardware checks
- **Windows:** use Rufus and choose **DD image mode** when it asks, or balenaEtcher. Pick `bongos.img` (decompress the release's `.img.xz` first with 7-Zip).
- **Linux:** `sudo dd if=bongos.img of=/dev/sdX bs=4M status=progress conv=fsync`. Triple-check that `sdX` is the USB stick.
- **Boot it:** during POST, press **F8** on ASUS boards to get the boot menu, and pick the UEFI entry for the stick. For early milestones, you may need to turn off Secure Boot (in the BIOS under Boot → Secure Boot → Other OS). Turn it back on for Windows afterward if you like; Phase 18 makes bongOS work *with* Secure Boot.
