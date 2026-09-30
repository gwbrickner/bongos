# bongOS status
_The main line's dashboard. Parallel-lane sessions never edit this file; they track progress in
their own milestone log. Keep it under ~80 lines. Finished milestones get one line here, and
the details belong in `docs/logs/M<p>.<n>.md`._

**Last updated:** 2026-09-30 (M2.6: steps 1-6 done and swept; only the finish gate remains)

## Next step
**M2.6 finish gate** (`needs-owner`; branch `claude/bold-hawking-dzmwlg`, which carries the M2.6 work
merged with `origin/main`), CLAUDE.md "Finishing a milestone". Gate step 1 is done: `qemu-tester`
reported `make format-check`, `host-tests` (503/503), `test` and `test-full` passing on the merge
commit. Step 2 is in progress: `bug-sweeper` finish mode was cut off by a rate limit and resumed; it
had committed `044b502` (random_get_bytes_long_request) and `a66c5b7` (RFC 8439 vectors
cross-checked). Remaining: (2) wait for `SWEEP: PASS` in `docs/sweeps/M2.6.md` (rerun until it
does); (3) `reviewer` on `git diff origin/main...HEAD`; (4) `make screenshot SHOT=docs/screenshots/M2.6.png`
and the `FW=bios` `M2.6-bios.png`, open both; (5) tick M2.6 in ROADMAP.md, write the log's
Summary/Verification, update this file; (6) PR from the template (`needs-owner: yes`). Then M3.1.

## Current milestone
**M2.6 KASLR + kernel RNG**, log `docs/logs/M2.6.md`, decisions D-120..D-126. Steps 1-6 (loader
relocation, kernel slide awareness, loaders wired, `libs/crypto`, kernel RNG + seed lifecycle, docs)
are done and steps 1-5 swept. Remaining: the finish gate above.

## Phase
2: Blue Dream (CPU and memory core)

## Done (the full writeups are in each log's Summary)
| Milestone | PR | What it delivered |
|---|---|---|
| M1.1 | [#1](https://github.com/gwbrickner/bongos/pull/1) | Repo skeleton, Makefile + `mk/*.mk`, host-test framework, CI |
| M1.2 | [#2](https://github.com/gwbrickner/bongos/pull/2) | Our own UEFI headers, a hello loader, a GPT disk image (`tools/mkimage`) |
| M1.3 | [#3](https://github.com/gwbrickner/bongos/pull/3) | Higher-half kernel, klog, `panic()`, the ktest framework, the real BootInfo handoff |
| M1.4 | [#4](https://github.com/gwbrickner/bongos/pull/4) | GOP boot menu, fbcon, `tools/imgdiff`, GUI screenshot tests |
| M2.1 | [#5](https://github.com/gwbrickner/bongos/pull/5) | GDT/TSS/IST, full IDT, symbolized backtraces (KSYM), UBSan, stack protector, `archTrapCatch` |
| M2.2 | [#6](https://github.com/gwbrickner/bongos/pull/6) | pmm: Page array, buddy allocator (DMA32/NORMAL), page cache, double-free detection |
| M2.3 | [#7](https://github.com/gwbrickner/bongos/pull/7) | Kernel page tables, W^X verifier, framebuffer WC, SMEP/SMAP/UMIP, loader reclaim, KVA |
| M2.4 | [#8](https://github.com/gwbrickner/bongos/pull/8) | Slab caches, kmalloc (16–8192 bytes), vmalloc with guard pages, `PMM_BUG_OWNED_PAGE` |
| M2.5 | [#11](https://github.com/gwbrickner/bongos/pull/11) | BIOS loader: stage1 MBR, stage2 with a real-mode thunk, E820/VBE, a GPT+FAT32 reader, the shared menu and handoff. One image boots both ways |

The boot matrix covers `uefi 1` and `bios 1`, plus 3072 MiB rows in `make test-full`. The final
boot screens are in `docs/screenshots/`.

## Blockers
_(none)_

## Questions for owner
- `BootInfo.bootDiskGuid` and `bootPartGuid` (D-056) have no milestone that fills them yet,
  so both stay zero. Suggestion: use the UEFI PartitionInfo protocol plus a BlockIo
  GPT-header read (the BIOS loader already has a GPT reader, D-105), in M6.4 (which adds the
  kernel's own GPT scanner, per D-056).
- **M2.6:** accept 8 bits of KASLR entropy (512 MiB window, D-121)? Keep the canary on D-077's seed
  fold, or move it to `randomGetBytes` later (the serial-printed slide leaks ~8 bits of that seed)?
  Is falling back to an unslid boot on a relocation failure (D-120) acceptable, versus refusing?
- **M2.6 vector provenance (needs network):** `libs/crypto/test/crypto-vectors.h` could not be diffed
  against the RFC text (rfc-editor.org was denied by the proxy). Every field was cross-checked against
  independent transcriptions (Nettle, Mbed TLS, pyca, Linux testmgr, Crypto++, `a66c5b7`), but
  someone with network access should still diff it against RFC 8439 2.3.2/2.4.2/A.1/A.2 and FIPS 180-4.

## Waiting on owner (hardware checks and other owner-only steps)
- **Default the main session to Sonnet** (D-117). Adding `"model": "sonnet"` to
  `.claude/settings.json` is an owner-only change, because the agent isn't allowed to change
  its own settings. Until then, pick Sonnet when starting a session (`/model sonnet`).
- **Re-run the cloud environment's setup script** (Environment settings → re-run setup), so
  that fresh sessions get `libclang-rt-18-dev` and `gdb` from `tools/ci/install-deps.sh`.
  Until then, `make host-tests` needs `sudo apt-get install -y libclang-rt-18-dev` in each new
  container. If apt fails on the image's third-party PPAs (ondrej/php, deadsnakes; both 403),
  move those files out of `/etc/apt/sources.list.d/` first.
- **M1.4 hardware check:** `dd` the image to a USB stick and boot it. Confirm the boot menu
  appears and that the arrow keys and Enter work. Report the resolution it logs and whether
  scrolling is smooth. Full steps are in `docs/logs/M1.4.md`, "Owner hardware check".
- **M2.5 hardware check** (optional; never blocks): enable CSM, boot the same USB stick in
  legacy/BIOS mode, and report whether the menu and kernel screen appear, and at what
  resolution. If it doesn't boot, report the last thing visible. Full steps are in
  `docs/logs/M2.5.md`, "Owner hardware check".

## Open leads (for the next `bug-sweeper` or `/milestone-sweep` to triage)
- `make analyze` on main reports 5 warnings: `kernel/include/list.h:50` (a possible NULL
  `prev` dereference), `kernel/test/kmalloc_test.c:68,143,365`, and
  `kernel/test/pmm_test.c:405`. The test-file hits are probably deliberate misuse, but none has
  been triaged yet.
- M2.5 deferred some items on purpose (D-114), including a PM-side diagnostic IDT in stage2
  and dual teletype+serial logging before VBE is set up.

## Parallel lanes (informational; the main line updates this when lanes merge)
| Milestone | Branch | State |
|---|---|---|
