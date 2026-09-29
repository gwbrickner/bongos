# bongOS status
_The main line's dashboard. Parallel-lane sessions never edit this file; they track progress in
their own milestone log. Keep it under ~80 lines. Finished milestones get one line here, and
the details belong in `docs/logs/M<p>.<n>.md`._

**Last updated:** 2026-09-29 (the AI-facing docs were reworked; M2.5 is next)

## Next step
Start **M2.5 BIOS loader** (`needs-owner`). Its Needs, M1.4 and M2.1, are done.
1. `git fetch origin main && git switch -c m2-5-bios-loader origin/main`
2. Copy `docs/logs/TEMPLATE.md` to `docs/logs/M2.5.md`, then write its Plan from ROADMAP.md's
   M2.5 steps and ARCHITECTURE §5.6 (the BIOS loader flow). Every step is **risky** (it's the
   boot handoff), so consult `architect` before writing stage1.
3. The scope, in short: a 440-byte NASM stage1 (INT 13h AH=42h reads of stage2, at an LBA and
   length that mkimage patches in); stage2 (A20, E820, VBE mode pick, RSDP scan, 32-bit C with
   real-mode INT 13h/10h thunks, reusing `boot/common`'s GPT/FAT32 readers and boot menu); load
   the kernel and initrd, build the page tables and BootInfo (`bootMethod = BIOS`), and enter
   long mode. `mkimage` installs stage1 and stage2. Add `bios 1` rows to
   `tests/harness/matrix.conf` and `matrix-full.conf`, and add BIOS runs of the GUI tests.

## Current milestone
None in progress.

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

Current tests: 40 ktests and 172 host tests. As of 2026-09-29 on main plus this docs work,
`make test` passes in both the debug and release profiles, and `make test-full` passes in
debug. The final boot screen is at `docs/screenshots/M2.4.png`.

## Blockers
_(none)_

## Questions for owner
- `BootInfo.bootDiskGuid` and `bootPartGuid` (D-056) have no milestone that fills them yet,
  so both stay zero. Suggestion: use the UEFI PartitionInfo protocol plus a BlockIo
  GPT-header read, in M6.4 (which adds the kernel's own GPT scanner, per D-056).

## Waiting on owner (hardware checks and other owner-only steps)
- **Default the main session to Sonnet** (D-101). Adding `"model": "sonnet"` to
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

## Open leads (for the next `bug-sweeper` or `/milestone-sweep` to triage)
- `make analyze` on main reports 5 warnings: `kernel/include/list.h:50` (a possible NULL
  `prev` dereference), `kernel/test/kmalloc_test.c:68,143,365`, and
  `kernel/test/pmm_test.c:405`. The test-file hits are probably deliberate misuse, but none has
  been triaged yet.

## Parallel lanes (informational; the main line updates this when lanes merge)
| Milestone | Branch | State |
|---|---|---|
