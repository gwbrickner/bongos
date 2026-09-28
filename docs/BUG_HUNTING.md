# bongOS Bug Hunting Protocol

A repeatable process for finding, triaging, fixing, and preventing bugs in bongOS. Written for both humans and Claude Code sessions. Drop it at `docs/BUG_HUNTING.md` and reference it from `CLAUDE.md`.

---

## 0. Ground Rules

1. **No repro, no fix.** A bug isn't "fixed" until there's a reliable way to trigger it and that trigger stops working after the fix.
2. **One bug, one branch, one commit.** Never bundle fixes. It makes bisecting later impossible.
3. **Every fix ships with a regression test** (in-kernel `ktest`, a headless QEMU boot check, or at minimum a documented manual repro).
4. **Log everything** in GitHub Issues using the template in §9, even bugs fixed in five minutes.
5. **Don't "fix" by removing the symptom.** Adding a `sti`, a delay, or a bigger stack until the crash goes away is not a root cause.
6. **Before tagging a release codename**, run the full sweep in §6 and the CI suite in §7. No open S1/S2 bugs allowed at tag time.

---

## 1. Severity Levels

| Level | Name | Meaning | Examples |
|---|---|---|---|
| **S1** | Kaboom | Triple fault, hang, panic, reboot loop | Double fault with no IST, deadlock in scheduler |
| **S2** | Silent Rot | Memory or state corruption, may not crash immediately | Heap overwrite, PMM double-allocates a frame, lost IRQ |
| **S3** | Wrong | Incorrect behavior, system keeps running | Syscall returns wrong value, wrong keyboard mapping |
| **S4** | Papercut | Cosmetic or minor | Framebuffer text misaligned, log typo |

S2 bugs are the most dangerous. They often show up later as a "random" S1 somewhere unrelated. Treat any unexplained S1 as a possible S2 until proven otherwise.

---

## 2. Debug Build Setup

Maintain two build profiles and hunt in **both**. Bugs that only appear at `-O2` are usually undefined behavior or missing `volatile`.

### Required compiler flags (all builds)

```make
CFLAGS += -ffreestanding -fno-builtin -nostdlib \
          -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
          -fno-omit-frame-pointer \
          -Wall -Wextra -Werror -Wshadow -Wpointer-arith \
          -Wcast-align -Wstrict-prototypes -Wmissing-prototypes
```

### Debug profile extras

```make
CFLAGS_DEBUG += -g3 -O0 \
                -fstack-protector-strong \
                -fsanitize=undefined \
                -DBONG_DEBUG=1
```

- `-fstack-protector-strong` requires you to define `__stack_chk_guard` and `__stack_chk_fail()` in the kernel. Have `__stack_chk_fail` panic with "stack smashed" and a backtrace.
- `-fsanitize=undefined` requires implementing the `__ubsan_handle_*` functions. Start with the common ones (type mismatch/misalignment, overflow, shift out of bounds, out of bounds, pointer overflow) and have each print file:line over serial, then panic.

### Release-ish profile

```make
CFLAGS_RELEASE += -g -O2
```

Keep `-g` even here. Debug symbols cost nothing at runtime and save hours.

---

## 3. Kernel Instrumentation (build these early)

These are the tools you hunt *with*. Build them before the kernel gets big.

### 3.1 Serial logger
- COM1 (`0x3F8`) initialized as the **very first** thing in `kmain`, before memory management.
- Log levels: `logTrace`, `logDebug`, `logInfo`, `logWarn`, `logError`.
- Every log line prefixed with subsystem tag and tick count: `[  1042][mm] mapped 0x... -> 0x...`
- Logger must be usable from interrupt context (no allocation, spinlock with IRQs disabled).

### 3.2 Panic handler
`kernelPanic(const char *fmt, ...)` must:
1. Disable interrupts (`cli`).
2. Dump all GPRs, `RIP`, `RFLAGS`, `CR0`, `CR2`, `CR3`, `CR4`, and the CS/SS selectors.
3. Walk the stack via `RBP` chain and print return addresses (capped at ~32 frames, and bail if `RBP` leaves the known stack range).
4. Print to **both** serial and framebuffer (framebuffer matters on real hardware).
5. Halt forever: `for (;;) asm volatile("cli; hlt");`

### 3.3 Symbolizing backtraces
Add `scripts/symbolize.sh` that pipes a serial log through:

```sh
addr2line -e build/kernel.elf -f -i -C <address>
```

Later upgrade: embed a sorted symbol table in the kernel so panics print function names directly.

### 3.4 Assertions
```c
#define kAssert(cond) \
    do { if (!(cond)) kernelPanic("assert failed: %s at %s:%d", #cond, __FILE__, __LINE__); } while (0)
```
Assert liberally: alignment of addresses passed to paging functions, IRQ state on lock entry, pointer ranges, list integrity.

### 3.5 Memory poisoning
| Event | Fill pattern | Why |
|---|---|---|
| Heap allocation | `0xCD` | Catches use of uninitialized memory |
| Heap free | `0xDD` | Catches use-after-free |
| Freed physical frame | `0xFE` | Catches stale mappings to freed frames |
| Red zones around heap blocks | `0xAB` guard bytes | Check on free; catches overflows |

If you ever see `0xCDCDCDCD`, `0xDDDDDDDD`, or `0xFEFEFEFE` in a register dump, you instantly know the bug class.

### 3.6 Guard pages
- Every kernel stack gets an **unmapped guard page** below it. A stack overflow then page-faults instead of silently corrupting neighbors.
- The double fault handler **must** run on its own IST stack (TSS IST1), otherwise a stack overflow becomes a triple fault with no info.

### 3.7 Lock debugging (once SMP or preemption exists)
- Each spinlock records its owner CPU and the `RIP` that acquired it.
- Detect: recursive acquire, release by non-owner, spinning longer than N iterations (print "possible deadlock" + owner `RIP`).

---

## 4. QEMU Hunting Configurations

### 4.1 Standard debug run

```sh
qemu-system-x86_64 \
  -machine q35 -m 512M -smp 1 \
  -cdrom build/bongos.iso \
  -serial file:logs/serial.log \
  -monitor stdio \
  -d int,cpu_reset,guest_errors -D logs/qemu.log \
  -no-reboot -no-shutdown
```

- `-no-reboot -no-shutdown` freezes the VM on a triple fault instead of rebooting, so you can inspect state.
- `-d int` logs every interrupt and exception with full register state. **Only works under TCG**, not KVM.
- `-monitor stdio` gives the QEMU monitor in your terminal.

### 4.2 Config matrix

Bugs hide in specific configurations. Before a release, boot through all of these:

| Variable | Values to test |
|---|---|
| RAM | `64M`, `512M`, `4G`, `8G` (tests memory map parsing and >4 GiB addresses) |
| CPUs | `-smp 1`, `-smp 2`, `-smp 4` |
| CPU model | default, `-cpu qemu64`, `-cpu max` |
| Accel | TCG (default), KVM if available locally |
| Machine | `q35`, `pc` |
| Build | debug `-O0`, release `-O2` |

### 4.3 QEMU monitor cheat sheet

| Command | Use |
|---|---|
| `info registers` | Full CPU state right now |
| `info mem` | Active virtual memory mappings (sanity check paging) |
| `info tlb` | Virtual to physical translations |
| `info lapic` / `info pic` | Interrupt controller state (missing EOI shows up here) |
| `x /16gx 0xADDR` | Dump virtual memory |
| `xp /16gx 0xADDR` | Dump physical memory |
| `x /10i $pc` | Disassemble at current instruction |

---

## 5. GDB Workflow

```sh
# terminal 1
qemu-system-x86_64 <flags from 4.1> -s -S

# terminal 2
gdb build/kernel.elf \
  -ex "set architecture i386:x86-64" \
  -ex "target remote :1234" \
  -ex "hbreak kmain" \
  -ex "continue"
```

Tips:
- Use **`hbreak`** (hardware breakpoints) early in boot and around paging changes. Software breakpoints write `int3` into memory and break when mappings change.
- `layout split` shows source and assembly together.
- `info registers rip rsp rbp cr2 cr3`
- `x/20gx $rsp` to inspect the stack.
- `watch -l someVar` for a hardware watchpoint on corruption. This is the #1 tool for S2 bugs: find what's being corrupted, watch it, catch the writer red-handed.
- `bt` works if frame pointers are on.

---

## 6. The Hunt: Subsystem Sweeps

Run a sweep on one subsystem at a time. For each, go through the checklist, write a test for anything not already covered, then try to break it on purpose.

### 6.1 Boot and early init
- [ ] `.bss` zeroed (or guaranteed zeroed by bootloader; verify, don't assume).
- [ ] Linker script sections page-aligned; kernel symbols like `kernelEnd` correct.
- [ ] Bootloader memory map parsed correctly: overlapping entries, unaligned entries, entries above 4 GiB, reserved regions respected.
- [ ] GDT loaded and segment registers reloaded (including a far return or `lretq` to reload CS).
- [ ] TSS loaded, IST entries point to the **top** of their stacks.
- [ ] Boots with 64M RAM and with 8G RAM.

### 6.2 Interrupts and exceptions
- [ ] All 32 exception vectors have handlers. Unexpected vectors panic with the vector number.
- [ ] Error-code vs no-error-code exceptions handled correctly (8, 10–14, 17, 21, 29, 30 push error codes). Getting this wrong misaligns the whole frame.
- [ ] Stubs save/restore **all** GPRs and end with `iretq` (not `iret`).
- [ ] Stack is 16-byte aligned before calling C handlers.
- [ ] EOI sent for every hardware IRQ (PIC or LAPIC), and spurious IRQs (7/15 on PIC, LAPIC spurious vector) handled without EOI where required.
- [ ] Double fault uses IST1.
- [ ] Try: divide by zero, `ud2`, null dereference, `int3`, write to a read-only page. Each must produce a clean, correct panic.

### 6.3 Physical memory manager
- [ ] Never hands out frame 0, kernel frames, bootloader-reclaimable frames still in use, or reserved regions.
- [ ] Double free detected (kAssert on bitmap/stack state).
- [ ] Allocating until exhaustion returns failure instead of wrapping or crashing.
- [ ] Stress test: allocate N frames, write a unique pattern into each, verify all, free in random order, repeat.

### 6.4 Virtual memory and paging
- [ ] All addresses canonical (bits 63:48 sign-extend bit 47).
- [ ] Page table pages zeroed on allocation.
- [ ] `invlpg` after every unmap/remap; full CR3 reload where needed.
- [ ] Flags correct: NX on data, no write on `.text` and `.rodata`, User bit only where intended.
- [ ] Higher-half kernel mapping survives removal of any identity mapping.
- [ ] `info mem` output matches what you expect after each major init step.

### 6.5 Kernel heap
- [ ] Alignment guarantees honored (at least 16 bytes).
- [ ] Red zones checked on free.
- [ ] Freeing a pointer not from the heap is detected.
- [ ] Stress: thousands of random-size alloc/free cycles with pattern verification.
- [ ] Coalescing of free blocks actually works (heap doesn't grow forever under churn).

### 6.6 Scheduler and context switching
- [ ] Callee-saved registers (`rbx`, `rbp`, `r12`–`r15`) saved/restored.
- [ ] Each task's kernel stack is separate and has a guard page.
- [ ] TSS `RSP0` updated on switch when user mode exists.
- [ ] No context switch while holding a spinlock.
- [ ] Stress: spawn many tasks that each increment their own counter and yield; verify all counters advance and none corrupt each other.

### 6.7 Syscalls and user mode (when it exists)
- [ ] Every user pointer validated (range, canonical, mapped, User bit) before the kernel touches it.
- [ ] Enable SMEP/SMAP if the CPU supports them; use `stac`/`clac` only around deliberate user copies.
- [ ] Invalid syscall numbers return an error, never index out of a table.
- [ ] Fuzz: a userland program that calls random syscalls with random arguments for 10 minutes. The kernel must never panic from user input.

### 6.8 Drivers
- [ ] MMIO accessed through `volatile` pointers.
- [ ] Port I/O widths correct (`inb`/`inw`/`inl`).
- [ ] Framebuffer writes bounds-checked (pitch vs width is a classic bug).
- [ ] Keyboard driver handles key release, extended scancodes (`0xE0`), and a buffer overflow when typing fast.
- [ ] Timer frequency calibrated, not hardcoded (matters on real hardware).

### 6.9 SMP (when it exists)
- [ ] Every shared structure has a documented lock.
- [ ] Per-CPU data accessed via `GS` base, correct on every CPU.
- [ ] TLB shootdowns implemented when unmapping shared memory.
- [ ] Run every stress test with `-smp 4`.

---

## 7. Automated Testing

### 7.1 In-kernel test framework (`ktest`)
- Tests registered with a macro into a dedicated linker section, e.g. `KTEST(pmmAllocFree) { ... }`.
- Built only when `BONG_KTEST=1`. The kernel boots, runs all tests, prints `KTEST PASS name` / `KTEST FAIL name` over serial, then exits QEMU.

### 7.2 Exiting QEMU with a status code
Add to QEMU flags:
```sh
-device isa-debug-exit,iobase=0xf4,iosize=0x04
```
Writing value `v` to port `0xf4` exits QEMU with status `(v << 1) | 1`. Pick a convention, e.g. write `0x10` for pass (exit 33) and `0x11` for fail (exit 35).

### 7.3 CI (GitHub Actions)
On every push and PR:
1. Build debug and release.
2. Run `ktest` build headless in QEMU with a 60-second `timeout`.
3. Fail the job if: exit code isn't the pass code, timeout hits, or `serial.log` contains `PANIC`, `UBSAN`, or `stack smashed`.
4. Upload `serial.log` and `qemu.log` as artifacts on failure.
5. Run the boot matrix from §4.2 (at least RAM sizes and `-smp`) on a nightly schedule.

### 7.4 Static analysis
Run weekly and before every release:
- `gcc -fanalyzer` (catches leaks, null derefs, use-after-free in C code paths)
- `cppcheck --enable=all --inconclusive src/`
- `scan-build make` (Clang static analyzer)

Treat new findings as S3 by default until triaged.

---

## 8. Triage Workflow

For every bug:

1. **Capture.** Save `serial.log`, `qemu.log`, the exact QEMU command, the commit hash, and the build profile.
2. **Reproduce.** Get it to trigger reliably. If it's intermittent, try `-smp 1`, TCG, and `-icount shift=auto` to reduce timing nondeterminism.
3. **Minimize.** Strip it down to the smallest trigger (a single `ktest` if possible).
4. **Classify.** Assign severity (§1) and subsystem label.
5. **Bisect if it's a regression.** `git bisect run scripts/reproBug.sh` where the script returns 0 on good, 1 on bad.
6. **Root-cause.** Write one sentence explaining *why* it happened, not just where. If you can't write that sentence, you're not done.
7. **Fix** on a branch named `fix/<subsystem>-<short-name>`.
8. **Add regression test.**
9. **Verify** in debug and release builds, and on the config that originally triggered it.
10. **Close the issue** with the root-cause sentence and the fix commit.

### Triple fault playbook
The VM froze with `-no-reboot`? Open `logs/qemu.log` and search upward from the end:

1. Find the last `check_exception` lines. The sequence tells the story: e.g. `v=0e` (page fault) → `v=08` (double fault) → triple fault.
2. The **first** exception in the chain is the real bug. Everything after is fallout.
3. Common vectors: `v=00` divide error, `v=06` invalid opcode, `v=0d` general protection, `v=0e` page fault.
4. For a page fault, read `CR2` (faulting address) and the error code bits: bit 0 = page present, bit 1 = write, bit 2 = user mode, bit 3 = reserved bit set, bit 4 = instruction fetch.
5. Symbolize `RIP` with `addr2line`.

### Pattern recognition
| Symptom | Usual suspect |
|---|---|
| `RIP` or `RSP` full of `0xCD`/`0xDD` | Uninitialized or use-after-free |
| Crash only at `-O2` | Missing `volatile`, UB, or bad inline asm constraints/clobbers |
| Random crash after interrupts enabled | Stub not saving a register, misaligned stack, wrong error-code handling |
| Works once, then no more IRQs | Missing EOI |
| Crash right after CR3 load | Kernel or current stack not mapped in new tables |
| Garbage in local variables near interrupts | Red zone (forgot `-mno-red-zone`) |
| `#UD` on a normal-looking instruction | Compiler emitted SSE (missing `-mgeneral-regs-only`) |
| Double fault with no useful info | Stack overflow, check guard page and IST |
| Works in QEMU, dies on real hardware | See §10 |

---

## 9. Bug Report Template

Save as `.github/ISSUE_TEMPLATE/bug.md`:

```markdown
---
name: Bug
about: Report a bongOS bug
labels: bug
---

**Severity:** S1 / S2 / S3 / S4
**Subsystem:** boot / int / pmm / vmm / heap / sched / syscall / driver / fs / smp / other
**Commit:**
**Build profile:** debug / release
**Environment:** QEMU (paste full command) / real hardware

**What happened:**

**Expected:**

**Repro steps:**
1.

**Logs:** (serial.log / qemu.log excerpt, symbolized backtrace)

**Root cause (fill when known):**

**Fix commit:**
**Regression test:**
```

Labels: `sev:1` … `sev:4`, `area:<subsystem>`, `regression`, `real-hw-only`, `heisenbug`.

---

## 10. Real Hardware Phase

Things QEMU hides from you:

- **RAM is not zeroed.** Anything you forgot to initialize now contains garbage.
- **Memory maps are messier:** more reserved holes, ACPI regions, and MMIO ranges.
- **Timing is real.** PIT/APIC/TSC calibration matters, and races that never showed in TCG show up.
- **Firmware quirks.** UEFI implementations vary; ACPI tables can be weird.

Debug output on real hardware:
1. **Serial** if the motherboard has a COM header (check the manual) plus a cheap header-to-DB9 cable and a USB serial adapter on another machine.
2. **Framebuffer panic screen** as the fallback; that's why §3.2 prints there too. Take a phone photo and symbolize the addresses.
3. **Log ring buffer** kept at a fixed physical address; after a warm reboot, the next boot can check for and print the previous boot's log (not guaranteed to survive, but often does).

Always boot real hardware from a USB stick, never let bongOS write to your real drives until the storage driver has passed its full sweep. Use a spare drive for filesystem testing.

---

## 11. Running a Hunt in Claude Code

Paste this at the start of a cloud session:

> Read `docs/BUG_HUNTING.md`. Run a bug hunt on the **[subsystem]** subsystem following §6.[n].
> For each checklist item: verify it by reading code and/or writing a `ktest`. Build and run headless in QEMU (TCG, no KVM) using the flags in §4.1 plus `isa-debug-exit`.
> For each bug found: follow §8 steps 1–9, one branch and one commit per bug, commit message format `fix(<subsystem>): <summary>`, and open an issue body using §9.
> Do not refactor unrelated code. Do not fix a bug without a repro. Stop and summarize when the checklist is complete or after 3 bugs, whichever comes first.

Tips for keeping sessions efficient:
- Scope each session to **one subsystem**. Broad "find all the bugs" sessions burn credit and produce shallow results.
- Have the session end with a summary: items verified, bugs found, tests added, items it couldn't verify.
- The cloud container likely has no KVM, which is fine (and actually good, since `-d int` needs TCG).

---

## 12. Release Gate Checklist

Before tagging a new codename:

- [ ] CI green on debug and release
- [ ] Full boot matrix (§4.2) passes
- [ ] Static analysis run, all findings triaged
- [ ] Syscall fuzzer ran 10+ minutes with no panic (once syscalls exist)
- [ ] Zero open S1 or S2 issues
- [ ] Booted on real hardware at least once
- [ ] `CHANGELOG.md` lists fixed bugs with issue numbers

---

## 13. Milestone Sweep

A full sweep of every implemented subsystem, run after each milestone. Two halves:

**Automated (free, runs on tag push):**
```sh
git tag milestone/<name> && git push origin milestone/<name>
```
This triggers `.github/workflows/milestone-sweep.yml`: static analysis plus the full §4.2 boot matrix (16 QEMU runs). Wait for it to finish before starting the Claude half.

**Claude Code (manual kickoff in a cloud session):**
```
/milestone-sweep <name>
```
This runs `.claude/skills/milestone-sweep/SKILL.md`, which acts as a coordinator: baseline build and tests, scopes which subsystems exist and which changed, then hands each subsystem to the `subsystem-hunter` subagent (`.claude/agents/subsystem-hunter.md`) one at a time, so each gets a fresh context. Reports land in `docs/sweeps/<name>/`, with `SUMMARY.md` as the entry point. All work happens on a `sweep/<name>` branch for review before merging.

**Sweep policy:**
- S1/S2 bugs with a solid repro get fixed during the sweep (max 3 per subsystem).
- S3/S4 bugs get logged only and fixed in normal sessions later.
- Unverified checklist items in `SUMMARY.md` become the next session's to-do list.
