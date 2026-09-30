# bongOS bug hunting protocol

A repeatable process for finding, triaging, fixing, and preventing bugs in bongOS. It's
written for both humans and Claude sessions, and it's binding for every debugging session and
every bug fix (see `CLAUDE.md`). Every command in it exists in this repo. If one doesn't work,
fix this document.

| § | Contents | § | Contents |
|---|---|---|---|
| 0 | Ground rules | 7 | Automated testing and static analysis |
| 1 | Severity levels | 8 | Triage workflow, triple-fault playbook, patterns |
| 2 | Build profiles | 9 | Bug record template |
| 3 | Kernel instrumentation | 10 | Real hardware phase |
| 4 | QEMU hunting configurations | 11 | Who hunts: agents and skills |
| 5 | GDB workflow | 12 | Release gate checklist |
| 6 | Subsystem checklists | 13 | Milestone sweep |

---

## 0. Ground rules

1. **No repro, no fix.** A bug isn't fixed until something triggers it reliably, and that
   trigger stops working after the fix.
2. **One bug, one commit.** Never bundle fixes. Bundled fixes make later bisecting impossible.
   The message is `fix(<subsystem>): <summary>`.
3. **Every fix ships with a regression test**: a `KTEST`, a host `TEST`, or a boot-matrix
   check. Show the test failing before the fix and passing after it.
4. **Record every bug**, even ones fixed in five minutes: a full §9 record, or a row in
   bug-sweeper's findings table (the row must carry the same essentials: severity, file:line,
   root cause, repro, fix/test). S1 and S2 bugs always get a full §9 record. A bug found
   during a milestone goes in `docs/sweeps/M<p>.<n>.md` or the milestone log; one found in a
   milestone-sweep goes in the sweep report.
5. **Don't "fix" a bug by removing the symptom.** Adding a `sti`, a delay, a bigger stack, or
   a retry until the crash goes away is not finding the root cause.
6. **Never weaken, skip, or delete a test** to get a pass (CLAUDE.md Hard rules).
7. **Before a release**, run the §12 gate. No open S1 or S2 bug is allowed at release time.

---

## 1. Severity levels

| Level | Name | Meaning | Examples |
|---|---|---|---|
| **S1** | Kaboom | Triple fault, hang, panic, reboot loop | #DF with no IST, deadlock in the scheduler |
| **S2** | Silent Rot | Memory or state corruption, may not crash right away | Heap overwrite, pmm hands out the same frame twice, lost IRQ, a test that passes against broken code |
| **S3** | Wrong | Incorrect behavior, but the system keeps running | Wrong `Status` returned, wrong keyboard mapping |
| **S4** | Papercut | Cosmetic or minor | Misaligned text, a log typo |

S2 bugs are the most dangerous, because they tend to surface later as a "random" S1 somewhere
unrelated. Treat any unexplained S1 as a possible S2 until proven otherwise.

---

## 2. Build profiles

There are two profiles, and every hunt uses **both**. Bugs that appear only at `-O2` are
usually undefined behavior, a missing `volatile`, or bad inline-asm constraints.

| Profile | Command | Flags (`mk/kernel.mk`) |
|---|---|---|
| Debug (default) | `make` | `-O1`, UBSan with an explicit check list (D-076), `-DKERNEL_DEBUG=1` (pmm write-after-free poisoning, slab redzones, and list-unlink hardening, D-082), `-fstack-protector-strong` |
| Release | `make RELEASE=1` | `-O2`, no UBSan, no `KERNEL_DEBUG`, stack protector still on |

Both profiles also get `-ffreestanding -mno-red-zone -mgeneral-regs-only -mcmodel=kernel
-fno-omit-frame-pointer -Wall -Wextra -Werror`. **Kernel C code has no DWARF** (no `-g` in
`KERNEL_CFLAGS`; only the NASM objects carry line info). Symbol names always resolve (ELF
symtab plus KSYM), but source lines and variable types don't.

**Always `make clean` before switching profiles.** The objects all live in `build/` and don't
track flags, so without a clean you get a mix of debug and release objects. The full release
check is:
```sh
make clean && make RELEASE=1 && make RELEASE=1 test
make clean && make            # leave a debug build behind
```

---

## 3. Kernel instrumentation (what exists and how to use it)

| Tool | Where | Use |
|---|---|---|
| **klog** | `kernel/core/klog.c`, `kernel/include/klog.h` | Leveled, tagged logging to serial and fbcon. Serial is COM1 and is up first. |
| **panic()** | `kernel/core/panic.c` | Turns interrupts off, prints `PANIC: ...` plus a **symbolized backtrace** (KSYM v1, D-075, `docs/specs/ksyms.md`), then halts. Under `ktest=` it reports `KTEST FAIL` and exits QEMU instead. |
| **panicBug()** | `kernel/include/panic.h` | For kernel-internal invariant violations (a double free, a foreign pointer). A ktest can catch it with `TRAP_CATCH_KERNEL_BUG`. **Never call it with a lock held** (D-082). |
| **Trap reports** | `kernel/arch/x86_64/trap.c` | Any fault prints the vector, error code, CR2, the registers, and a symbolized backtrace. |
| **archTrapCatch()** | `kernel/include/arch/trap.h`, D-078 | ktest-only. Runs a function and catches a matching fault or software trip (`STACK_SMASH`, `UBSAN`, `KERNEL_BUG`), so a test can prove that detection works. |
| **Stack protector / UBSan** | `kernel/core/stack-protector.c`, `ubsan.c` | Print `file:line`, then panic (or get caught by archTrapCatch). |
| **Poisoning** (debug) | `kernel/mm/pmm.c`, `kernel/mm/slab-internal.h` | Freed pages and slab objects are filled with `0x6B`. Slab redzones are `0xBB` (`SLAB_REDZONE_SIZE` = 16 bytes). |
| **Misuse checks** (always on) | pmm and slab (D-082, D-096) | Double free, freeing a foreign pointer, and freeing a pointer that isn't the object's start all go to `panicBug()`. |
| **W^X verifier** | `kernel/mm/vmm.c` | Prints `vmm: W^X verified: ...` at boot, and `make test` requires that line. |

**Poison values to recognize in a register dump:**

| Value | Means |
|---|---|
| `0x6B6B6B6B6B6B6B6B` | Use after free (a pmm page or a slab object) |
| `0xBBBBBBBB...` | Overran a slab object into its redzone |

**Symbolizing an address by hand:** `llvm-addr2line -f -e build/kernel/kernel.elf <addr>` prints
the function name. The file:line shows as `??:0` without DWARF, so to find the instruction,
disassemble around it with `llvm-objdump -d --start-address=<addr-0x40> --stop-address=<addr+0x10>
build/kernel/kernel.elf`. The kernel is KASLR-slid on every boot (M2.6): subtract the slide first,
i.e. use `address - slide`. The slide is on the loader's serial line `loader: kaslr: slide=0x...`, on
the kernel's `[info] kaslr: virtBase=... slide=...` line, and in the first line of every backtrace
(`kaslr slide 0x... (link address = address - slide)`). Backtrace symbol names already account for it.

#DF, NMI, and #MC already run on their own IST stacks (M2.1, `cpu-init.c`). Still to come:
guard pages under each kernel stack (arriving with threads), and lock debugging (owner CPU
plus acquire RIP, recursion and non-owner release detection, and a spinning-too-long warning),
which arrives with SMP and preemption (M3.x).

---

## 4. QEMU hunting configurations

Everything goes through the harness. Don't hand-roll QEMU command lines.

### 4.1 Standard runs
```sh
tests/harness/run-qemu.sh --help                       # every option
tests/harness/run-qemu.sh --image build/bongos-ktest.img --fw uefi --cpus 1   # the ktest image, headless
tests/harness/run-qemu.sh --image build/bongos-ktest.img --fw bios --cpus 1   # the same, through SeaBIOS + stage1/stage2
tests/harness/run-qemu.sh --image build/bongos-ktest.img --debug              # + -d int,cpu_reset -> build/logs/<name>.qemu.log
tests/harness/run-qemu.sh --image build/bongos-ktest.img --mem 3072 --name big # a >4 GiB split (the NORMAL zone)
make run / make debug / make gdb                       # humans only: interactive, never exit
```
- `build/bongos-ktest.img` boots with `ktest=all` (`tests/harness/ktest-boot.cfg`), runs every
  ktest, and exits QEMU through `isa-debug-exit`. `build/bongos.img` is the normal image; it
  never runs ktests.
- The harness always passes `-no-reboot`, so a triple fault ends the run (it shows as CRASH)
  instead of looping.
- `-d int` logs every exception with the full register state. **It only works under TCG**, so
  `--debug` forces TCG. Otherwise the harness uses KVM when `/dev/kvm` is writable (CI) and TCG
  when it isn't (the cloud container).
- Agents: builds and boots need a 600000 ms Bash timeout (`make test` takes about 2–3 minutes).
  `make clean` deletes `build/logs/`, so read the logs first.
- The HMP monitor socket is `build/run/<name>.monitor`. Serial goes to
  `build/logs/<name>.serial.log`.
- To select only some ktests, put `ktest=<pattern>` in a boot.cfg cmdline (see
  `kernel/test/ktest.c` for the pattern syntax).

### 4.2 Config matrix
Bugs hide in specific configurations. `tests/harness/matrix.conf` (`make test`) and
`matrix-full.conf` (`make test-full`) hold the rows the current milestone supports, in the
form `<fw> <cpus> [memMiB]`. Add rows as features land. Before a release, cover:

| Variable | Values |
|---|---|
| Firmware | UEFI (OVMF) and BIOS (SeaBIOS); both are in `matrix.conf` since M2.5 |
| RAM | 64 MiB (once the pmm handles it), 512 MiB, 3072 MiB (splits across the 4 GiB hole), 8 GiB |
| CPUs | 1; 2 and 4 (once M3.5 SMP lands) |
| Profile | debug and release (§2) |

### 4.3 QEMU monitor cheat sheet
Connect to it with `nc -U build/run/<name>.monitor`.

| Command | Use |
|---|---|
| `info registers` | Full CPU state right now |
| `info mem` / `info tlb` | Active mappings / virtual→physical translations (a paging sanity check) |
| `info lapic` / `info pic` | Interrupt controller state (a missing EOI shows up here) |
| `x /16gx ADDR` / `xp /16gx ADDR` | Dump virtual / physical memory |
| `x /10i $pc` | Disassemble at the current instruction |

---

## 5. GDB workflow

For humans, `make gdb` starts QEMU paused with the gdbstub on `:1234`, then attaches an
interactive gdb. **Agents must use batch mode** (an interactive gdb never returns):
```sh
tests/harness/run-qemu.sh --image build/bongos-kaslroff.img --gdb --name dbg   # returns at once
timeout 120 gdb -batch build/kernel/kernel.elf -ex "target remote :1234" \
    -ex "hbreak kernelMain" -ex continue -ex "info registers rip rsp cr3" -ex "bt" -ex "x/8gx \$rsp"
kill "$(cat build/run/dbg.pid)"                                               # always clean up
```
**KASLR (M2.6):** every other image (`build/bongos.img`, `build/bongos-ktest.img`) slides the kernel
by a random multiple of 2 MiB on each boot, so `hbreak kernelMain` with `kernel.elf` at its link
addresses would never hit. Use `build/bongos-kaslroff.img` (the ktest image with `kaslr = off`,
slide 0, `ktest=all`) as above. To debug a slid boot instead, the slide must be known before symbols
can be loaded: it is on the loader's serial line `loader: kaslr: slide=0x<hex>`
(`build/logs/<name>.serial.log`). So attach to a VM that is already past the loader (a hang, or a
run paused after that line appeared), then run
`-ex "symbol-file build/kernel/kernel.elf -o 0x<slide>"` before any `bt`/`hbreak` on kernel symbols.
Every boot picks a new slide, so a slide read from an earlier run never applies to the next one.
The entry points are `kernelEntry` (asm) and `kernelMain` (C). Check other symbols with
`nm build/kernel/kernel.elf | grep <name>` (add the slide to those addresses on a slid boot).

Tips:
- Use **`hbreak`** early in boot and around paging changes. Software breakpoints write `int3`
  into memory, and they break when the mappings change.
- A hardware watchpoint catches the writer of corrupted memory. It's the number one tool for
  S2 bugs. Without DWARF, gdb doesn't know C types, so watch an address:
  `watch -l *(unsigned long *)0xffff...` (take the address from `nm` or a log line).
- `info registers rip rsp rbp cr2 cr3`, `x/20gx $rsp`, `x/10i $pc`, and `bt` (frame pointers
  are on; there are symbol names, but no source lines).

---

## 6. Subsystem checklists

Sweep one subsystem at a time. Go through its checklist, write a test for anything not already
covered, then try to break it on purpose. The "Paths" line is the scope a sweep covers.

### 6.1 Boot, loader, and early init
Paths: `boot/` (`uefi/`, `bios/` stage1+stage2, `common/`), `kernel/arch/x86_64/entry.asm`, `early-map.c`, `kernel/core/main.c`,
`bootinfo.c`, `tools/mkimage/`
- [ ] BootInfo is validated (magic, version, sizes) before use. Both loaders fill it the same
  way (ARCHITECTURE §5.3).
- [ ] `.bss` is zeroed (verify it; don't assume). Linker-script sections are page-aligned.
  Section symbols are correct.
- [ ] Memory-map parsing handles overlapping, unaligned, and >4 GiB entries, and respects
  reserved regions.
- [ ] GDT loaded and segment registers reloaded (including CS, via `lretq`). TSS loaded, with
  IST pointers at the **top** of their stacks.
- [ ] The loader's boot.cfg parser (D-067) rejects malformed input without hanging (host-test
  it).
- [ ] Boots at the smallest and largest RAM sizes in the matrix.

### 6.2 Interrupts and exceptions
Paths: `kernel/arch/x86_64/trap*.{c,asm}`, `cpu-init.c`, `load-gdt.asm`
- [ ] All 32 exception vectors have handlers. Unexpected vectors panic and name the vector.
- [ ] Error-code handling is correct: vectors 8, 10–14, 17, 21, 29, and 30 push one. Getting
  this wrong misaligns the whole frame.
- [ ] Stubs save and restore **all** GPRs, keep the stack 16-byte aligned before `call`, and
  end in `iretq`.
- [ ] #DF, NMI, and #MC run on their own IST stacks.
- [ ] Every hardware IRQ gets an EOI, and spurious IRQs are handled correctly (once the LAPIC
  and IOAPIC land).
- [ ] Deliberate `ud2`, #PF, #GP, divide by zero, and `int3` each produce the correct report,
  shown by ktests using `archTrapCatch`.

### 6.3 Physical memory manager
Paths: `kernel/mm/pmm*.c`, `buddy.c`, `early.c`, `kernel/include/pmm.h`, `page.h`
- [ ] Never hands out frame 0, kernel frames, in-use loader frames, or reserved regions.
- [ ] Double free, freeing the upper half of a buddy pair, and freeing a foreign page are all
  detected (`pmm_double_free`, D-082).
- [ ] Allocating until exhaustion returns an error. Nothing wraps and nothing crashes.
- [ ] Stress: allocate N frames, write a unique pattern into each, verify every one, free them
  in random order, and repeat. The meminfo self-check line stays `OK`.

### 6.4 Virtual memory and paging
Paths: `kernel/arch/x86_64/paging.c`, `kernel/mm/vmm.c`, `kva.c`
- [ ] Every address is canonical. Page-table pages are zeroed on allocation.
- [ ] `invlpg` runs after every unmap and remap, and CR3 is reloaded where needed (plus a
  shootdown once SMP exists).
- [ ] Flags are correct: NX on data, no write on text and rodata, the U bit only where
  intended, PAT/WC on the framebuffer. There are no W^X aliases through the HHDM or the KVA
  window (the M2.3 lesson).
- [ ] The higher-half mapping survives removing the loader's mappings (`loader_reclaimed`).

### 6.5 Kernel heap (slab, kmalloc, vmalloc)
Paths: `kernel/mm/slab*.c`, `vmalloc.c`, `kernel/include/kmalloc.h`, `vmalloc.h`
- [ ] Alignment guarantees are honored (at least 16 bytes, and natural alignment for power-of-2
  sizes).
- [ ] Redzones are checked on free. Freeing a foreign pointer, an interior pointer, or the same
  pointer twice is detected (D-096).
- [ ] No lock is held across pmm calls, ctor/dtor calls, or `panicBug()` (the M2.4 lesson).
- [ ] Stress: thousands of random-size alloc/free cycles with pattern checks. `slabShrinkAll()`
  returns every page, so the heap doesn't grow forever under churn.
- [ ] vmalloc's OOM partial failure unwinds completely (the pmm page counts return to
  baseline).

### 6.6 Scheduler and context switching (once it exists)
- [ ] The callee-saved registers (`rbx`, `rbp`, `r12`–`r15`) are saved and restored. Each
  task has its own kernel stack with a guard page. TSS `RSP0` is updated on each switch.
- [ ] No context switch happens while a spinlock is held (check the preempt count).
- [ ] Stress: many tasks that each increment their own counter and yield. All the counters
  advance, and none corrupts another.

### 6.7 Syscalls and user mode (once they exist)
- [ ] Every user pointer is validated and copied through `copyFromUser`/`copyToUser`. SMEP,
  SMAP, and UMIP are on (M2.3 enables them). `stac`/`clac` appear only around deliberate
  copies.
- [ ] Invalid syscall numbers return an error and never index outside the table. Handle rights
  are checked and can never grow.
- [ ] Fuzz: random syscalls with random arguments for 10 minutes. User input must never panic
  the kernel.

### 6.8 Drivers
Paths: `kernel/drivers/`
- [ ] MMIO goes through `volatile` accessors, with barriers where the device needs them. Port
  I/O widths are correct.
- [ ] Framebuffer writes are bounds-checked (pitch ≠ width × bpp is the classic bug).
- [ ] Keyboard: key release, `0xE0` extended scancodes, and buffer overflow while typing fast.
- [ ] Timers are calibrated, not hardcoded.

### 6.9 SMP (once it exists)
- [ ] Every shared structure has a documented lock. Per-CPU data goes through the GS base and
  is correct on every CPU.
- [ ] TLB shootdowns happen when shared mappings change.
- [ ] Every stress test runs with 4 CPUs.

---

## 7. Automated testing and static analysis

### 7.1 ktests (in the kernel)
```c
#include "ktest.h"
KTEST(pmm_alloc_free_stress) {           /* the name becomes "KTEST PASS pmm_alloc_free_stress" */
    KTEST_ASSERT(p != NULL);
    KTEST_ASSERT_EQ(got, want);          /* prints both values in hex on failure */
}
```
Put them in `kernel/test/*_test.c` (arch-specific ones go in `kernel/arch/x86_64/test/`). The
build picks them up automatically, and they run when the cmdline has `ktest=`. If a ktest
proves a Done-when clause, add its name to `mk/test.mk`'s `_check-ktest-pass` required list.
Otherwise a dropped test would pass silently.

### 7.2 Host tests
Use `TEST(name)` with `ASSERT_TRUE`/`ASSERT_EQ`/`ASSERT_STREQ` (`tests/host/framework/test.h`)
in `tests/host/*_test.c`. They build with ASan and UBSan, and they're the place to test pure
logic (parsers, allocator algorithms) exhaustively. Kernel sources compiled for the host get
listed in `mk/host-tests.mk`.

### 7.3 The exit protocol
The kernel prints `KTEST START/PASS/FAIL <name>` and a final `KTEST DONE`, then writes to the
`isa-debug-exit` port 0xF4: `0x10` means all passed (QEMU exits 33), and `0x11` means a failure
(exits 35). The harness maps a timeout to HANG and a reset or poweroff to CRASH (ARCHITECTURE
§23).

### 7.4 CI
`.github/workflows/ci.yml` runs on every PR and every push to main: `make format-check`,
`make host-tests`, and `make test`. On failure it uploads `build/logs`.
`.github/workflows/milestone-sweep.yml` (§13) adds `make test-full`, the release profile, and
`make analyze`.

### 7.5 Static analysis
`make analyze` runs the Clang static analyzer over every kernel C source, with the kernel's own
flags. The report goes to `build/analyze/report.txt`. It exits 1 if clang failed on any file (that
file went unanalyzed, so fix the build or the analyzer flags). Warnings fail it only with
`ANALYZE_STRICT=1`.
Triage every finding in the code you're changing as a real bug, a false positive (say why), or
needs investigation. Findings in test code that deliberately misuses the API are usually false
positives, but check each one. Treat any untriaged finding as S3 until it's triaged.

---

## 8. Triage workflow

For every bug:
1. **Capture.** Save the serial log, the `--debug` qemu log, the exact harness command, the
   commit, and the profile.
2. **Reproduce.** Make it trigger reliably. If it's intermittent, use 1 CPU, TCG, and
   `--extra "-icount shift=auto"` to cut timing nondeterminism. Try both profiles.
3. **Minimize.** Strip it down to the smallest trigger, ideally a single ktest selected with
   `ktest=<name>`.
4. **Classify.** Assign a severity (§1) and a subsystem.
5. **Bisect if it's a regression.** Use `git bisect run <script>`, where the script builds,
   runs the minimal repro, and exits 0 for good and 1 for bad.
6. **Root-cause it.** Write one sentence saying **why** it happened, not just where. If you
   can't write that sentence, you're not done.
7. **Fix it** (§0 rules 2 and 5). Change the design only through `architect` and a `D-0xx`
   entry.
8. **Add the regression test.** Show it failing before the fix.
9. **Verify** in both profiles (§2), on the configuration that first triggered it, plus
   `make test`.
10. **Record it** (§9).

Same failure after 2 fix attempts? Stop patching and consult `architect` (CLAUDE.md "When
stuck").

### Triple-fault playbook
The run ended in CRASH? Rerun it with `--debug` and search `build/logs/<name>.qemu.log` upward
from the end:
1. Find the last `check_exception` lines. The sequence tells the story, for example `v=0e`
   (#PF) → `v=08` (#DF) → a reset.
2. The **first** exception in the chain is the real bug. Everything after it is fallout.
3. The common vectors: `v=00` #DE, `v=06` #UD, `v=0d` #GP, `v=0e` #PF.
4. For a #PF, CR2 is the faulting address. The error code bits are: 0 = present, 1 = write,
   2 = user, 3 = reserved bit set, 4 = instruction fetch.
5. Symbolize RIP (§3). The qemu log's RIP is exact even without DWARF, so disassemble around
   it.

### Pattern recognition
| Symptom | Usual suspect |
|---|---|
| Registers or memory full of `0x6B` | Use after free |
| `0xBB` where data should be | An overrun into a slab redzone |
| Crash only in release (`-O2`) | UB, a missing `volatile`, or bad inline-asm constraints or clobbers |
| Random crash after interrupts are enabled | A stub not saving a register, a misaligned stack, or wrong error-code handling |
| Works once, then no more IRQs | A missing EOI |
| Crash right after a CR3 load | The kernel, the current stack, or the IST stacks aren't mapped in the new tables |
| Garbage in locals near interrupts | The red zone (check `-mno-red-zone`) |
| `#UD` on a normal-looking instruction | SSE emitted (check `-mgeneral-regs-only`) |
| #DF with no useful info | A stack overflow: check the guard page and IST |
| A test passes but the feature is broken | A weak test. Mutation-check it (bug-sweeper §4). |
| Works in QEMU, dies on real hardware | See §10 |

---

## 9. Bug record template

Use one per bug, in the sweep report or the milestone log:
```markdown
### BUG-<milestone>-<n>: <one-line summary>
- **Severity:** S1 / S2 / S3 / S4 · **Subsystem:** boot / int / pmm / vmm / heap / sched / syscall / driver / smp / other
- **Commit found at:** <sha> · **Profile:** debug / release · **Config:** <harness command>
- **What happened / expected:**
- **Repro:** exact commands or ktest name
- **Evidence:** serial or qemu log excerpt, symbolized backtrace
- **Root cause:** one sentence: why
- **Fix:** <sha> or "not fixed: <reason>" · **Regression test:** <name>
```
GitHub Issues are optional. Open one only when a bug is deferred past the current milestone,
and use the same fields.

---

## 10. Real hardware phase

Things QEMU hides:
- **RAM is not zeroed.** Anything you forgot to initialize now contains garbage.
- **Memory maps are messier:** more reserved holes, ACPI regions, and MMIO ranges.
- **Timing is real.** PIT, APIC, and TSC calibration matters, and races that never showed up
  under TCG appear.
- **Firmware quirks.** UEFI implementations vary, and ACPI tables can be odd.

Getting debug output from real hardware:
1. **Serial**, if the board has a COM header, using a header-to-DB9 cable and a USB serial
   adapter on another machine.
2. **The framebuffer panic screen** (fbcon mirrors klog). Take a phone photo and symbolize the
   addresses.
3. **A log ring buffer** at a fixed physical address, printed by the next boot after a warm
   reboot (planned).

Always boot real hardware from USB. The real-disk write guard stays on (ROADMAP safety rule).
Owner hardware checks go in the milestone log and in STATUS.md's "Waiting on owner".

---

## 11. Who hunts: agents and skills

| When | Who | Scope |
|---|---|---|
| After each risky milestone step, and at the milestone finish | `bug-sweeper` (Opus) | That diff: adversarial tests, mutation checks, both profiles, the analyzer, fixes with a repro |
| Before the PR | `reviewer` (Opus) | Design and contract review of the whole milestone diff |
| After a milestone merges (the owner runs it) | `/milestone-sweep` → `subsystem-hunter` (Sonnet), one per subsystem | Everything that exists, checklist by checklist (§6) |
| A bug survives 2 fix attempts | `architect` (Opus) | Root-cause diagnosis |

For an ad-hoc hunt in a session, tell it: "Follow `docs/BUG_HUNTING.md` §8 for <symptom>" or
"Hunt §6.<n> for <subsystem>". Keep each session scoped to **one** subsystem. Broad "find all
the bugs" sessions produce shallow results.

---

## 12. Release gate checklist

Before a release:
- [ ] CI is green. `make test` and `make test-full` pass in **both** profiles (§2).
- [ ] The full config matrix (§4.2) passes for every row the implemented features support.
- [ ] `make analyze` ran, and every finding is triaged.
- [ ] The syscall fuzzer ran 10+ minutes with no panic (once syscalls exist).
- [ ] No open S1 or S2 bugs.
- [ ] Real hardware has booted at least once (the owner check is recorded).
- [ ] The release notes (the milestone log Summaries) list the bugs that were fixed.

---

## 13. Milestone sweep

A full sweep of every implemented subsystem, run after a milestone merges. It has two halves:

**Automated (CI):** `.github/workflows/milestone-sweep.yml` runs from the Actions tab
("Run workflow"), or it runs itself when a `sweep/*` branch is pushed. It builds both
profiles, runs `make test-full` and the release `make test`, and runs `make analyze`, then
uploads the logs and the analyzer report.

**Claude (a cloud session the owner starts):**
```
/milestone-sweep M<p>.<n>
```
This runs `.claude/skills/milestone-sweep/SKILL.md`. It acts as a coordinator: a baseline in
both profiles, then scoping which subsystems exist and which changed, then handing each
subsystem to `subsystem-hunter` one at a time, so each gets a fresh context. The reports land
in `docs/sweeps/M<p>.<n>-full/`, with `SUMMARY.md` as the entry point. All the work happens on
a `sweep/M<p>.<n>` branch, for the owner to review before merging.

**Sweep policy:**
- S1 and S2 bugs with a solid repro get fixed during the sweep (at most 3 per subsystem).
- S3 and S4 bugs get logged only, and are fixed later in normal sessions.
- Unverified checklist items in `SUMMARY.md` become the next session's to-do list.
