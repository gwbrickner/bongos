# bongOS Roadmap

## How this file works
- **One milestone = one cloud session = one PR.** Branch name: `m<phase>-<n>-<slug>`
  (e.g. `m2-3-kernel-paging`).
- **What to work on:** the lowest-numbered unchecked milestone whose **Needs** are all done,
  unless the owner names a different one. Milestones marked **`[parallel-ok]`** are host-only
  libraries or tools. A separate session can run them at the same time as the main line,
  once their **Needs** are done.
- **`needs-owner`:** the PR waits for the owner's review instead of auto-merging (D-045).
- **Owner hardware checks:** these are for the owner to run on the reference PC. Hardware
  claims in such a milestone are not "done" until the owner reports the result, which gets
  logged in the milestone log. Everything else in the milestone can still merge.
- **Done when** lists objective checks. All of them must pass in `make test` (or
  `make test-full`/`make host-tests` where stated), in a cloud session, before the PR.
- **Finishing a milestone:**
  1. Check its box here.
  2. Update `docs/STATUS.md`.
  3. Merge the PR.
  4. Tag `v0.<phase>.<n>` (from phase 16 on: `v1.<phase-15>.<n>`).
- **Releases:** CI publishes the image for each tag. The GitHub Release title uses the
  phase's codename.

## Codenames
| Phase | Codename | Theme |
|---|---|---|
| 1 | Acapulco Gold | Toolchain, UEFI loader, first boot |
| 2 | Blue Dream | CPU and memory core, BIOS loader, KASLR |
| 3 | Cherry Pie | ACPI tables, interrupts, time, locks, SMP |
| 4 | Durban Poison | Threads and scheduler |
| 5 | Early Girl | Objects, handles, syscalls, IPC, first userspace |
| 6 | Forbidden Fruit | Modules, PCI, input, storage drivers |
| 7 | Girl Scout Cookies | VFS, page cache, FAT32, bongfs, swap |
| 8 | Headband | Userland foundation: svcd, POSIX, shell, users |
| 9 | Ice Cream Cake | Full ACPI, IOMMU, userspace drivers, USB |
| 10 | Jack Herer | Networking |
| 11 | Kosher Kush | Crypto, TLS, SSH, packages |
| 12 | Lemon Haze | Graphics and desktop |
| 13 | Maui Wowie | Apps and sandboxing |
| 14 | Northern Lights | Audio, dynamic linking, ports, DOOM |
| 15 | **OG Kush** | More filesystems, installer (**v1.0**) |
| 16 | Purple Punch | Browser and full text shaping |
| 17 | Quantum Kush | Linux binary compatibility |
| 18 | Runtz | Secure Boot and disk encryption |
| 19 | Sour Diesel | Self-hosting and performance |
| 20 | Trainwreck | Stretch goals |
| — | *later list* | Wi-Fi, Bluetooth (unscheduled) |

## Real-hardware safety rule (applies from M6.4 on)
The block layer **refuses every write to a disk that isn't the boot disk**, identified by
`BootInfo.bootDiskGuid`. The only override is an explicit `diskwrite=<disk-guid>` on the
kernel command line. The owner's NVMe holds other data, and bongOS must never touch it by
accident. Every storage milestone keeps a ktest proving the guard works.

---

## Phase 1: Acapulco Gold (bring-up)

### [ ] M1.1 Repo skeleton, build system, CI
**Needs:** none
1. Create the directory tree from ARCHITECTURE §2. Add a top-level `Makefile` with per-component `*.mk` fragments, and `branding/` (`name`, `version`, `codename`) exposed to C as `branding.h`.
2. Add `.clang-format` and the `make format` / `make format-check` targets.
3. Add the `make host-tests` framework: a tiny assert-based runner in `tests/host/`, with one sample test.
4. Add the GitHub Actions workflow: format-check, host-tests, and `make test` (it can be a stub at this point).
5. Add `tests/harness/`: `run-qemu.sh` (boot matrix runner, isa-debug-exit result mapping, timeouts, serial log capture into `build/logs/`).

**Done when:** `make host-tests` and `make format-check` pass locally and in CI, and the
workflow is green on the PR.

### [ ] M1.2 UEFI headers + hello loader + disk image `needs-owner`
**Needs:** M1.1
1. Add `boot/uefi/include/efi/`: our own minimal headers from the UEFI spec (base types, `EFI_SYSTEM_TABLE`, Boot Services, Simple Text Output, Loaded Image, Simple File System + File Protocol, GOP, RNG, the config-table GUIDs).
2. Add a UEFI loader entry `efiMain` that prints `bongOS loader` to ConOut **and** to COM1 (port I/O is fine in the loader), then halts.
3. Add `tools/mkimage` (host C): builds a GPT image with a protective MBR, a 1 MiB BIOS boot partition, a FAT32 ESP (via `mkfs.fat` + `mtools` for now), and an empty root partition. It places `BOOTX64.EFI` on the ESP.
4. Harness: boot OVMF with the image and assert the serial output contains the banner.

**Done when:** `make test` boots the loader under OVMF and sees the banner.

### [ ] M1.3 Kernel skeleton + real handoff `needs-owner`
**Needs:** M1.2
1. Kernel side:
   - `kernel.ld` (higher half at `0xFFFFFFFF80000000`, sections aligned for W^X)
   - the entry stub (switches to the kernel stack)
   - the 16550 serial driver and `klog`
   - `panic()`
   - the ktest framework (`KTEST()` section registration, `ktest=` cmdline, isa-debug-exit reporting)
2. `boot/common/`:
   - the ELF64 loader
   - the page-table builder (HHDM with 1 GiB/2 MiB pages, kernel mapping, trampoline identity page)
   - the BootInfo builder (`bootinfo.h` exactly as ARCHITECTURE §5.3)
   - the memory-map conversion
3. The UEFI loader loads `kernel.elf` + `boot.cfg` (a minimal `kernel=` + `cmdline=`) and runs `ExitBootServices` with retry, then jumps.
4. The kernel validates BootInfo (magic/version), prints the memory map summary, and runs its ktests.

**Done when:** `make test` shows `KTEST PASS bootinfo_valid` and exits 33 under OVMF at 1 CPU.

### [ ] M1.4 Framebuffer console + boot menu
**Needs:** M1.3
1. Loader: GOP mode selection (the rule in §5.5, plus a `resolution=` override), fill `BootInfo.fb`.
2. Kernel `fbcon`:
   - an 8x16 bitmap font (original or public domain), stored in `data/fonts/console.psf`
   - scrolling and colors
   - mirrors klog
3. Full `boot.cfg` parser with `[entry]` sections, `timeout`, and `default`. Menu on screen and serial (arrow keys + Enter; on serial, number keys).
4. Harness: QEMU `screendump` at the boot menu and after the kernel banner, compared with `tools/imgdiff` (introduced here) against reference PNGs.

**Done when:** the screenshot tests pass, and choosing each menu entry over serial boots with the right cmdline.
**Owner hardware check:**
1. `dd` the image to a USB stick.
2. Boot the PC from it through the UEFI boot menu (you may need to disable Secure Boot for now).
3. Confirm the boot menu appears, then the kernel text at native resolution.
4. Report the resolution shown in the log.

---

## Phase 2: Blue Dream (CPU and memory core)

### [ ] M2.1 GDT, IDT, exceptions, hardening runtime
**Needs:** M1.3
1. Kernel GDT (layout §7.1) + TSS with IST1-3. IDT with all 32 exception stubs. The handler prints the vector, error code, registers, CR2, and a symbolized backtrace.
2. Embed a compressed kernel symbol table in the image (post-link step) and add `symbolize()`.
3. Stack protector runtime (`__stack_chk_guard` seeded from `BootInfo.randomSeed`), plus the UBSan runtime with our own handlers (debug builds).
4. ktests: `int3` resumes; a deliberate #PF is reported with the correct CR2; a deliberate #UD is caught; a stack smash is detected.

**Done when:** those ktests pass, and panic output includes a symbolized backtrace.

### [ ] M2.2 Physical memory manager `needs-owner`
**Needs:** M2.1
1. Early bump allocator over the BootInfo map.
2. The `Page` array at the metadata region (§6.1). Buddy allocator orders 0-10 with `DMA32` and `NORMAL` zones.
3. A per-CPU page cache (BSP-only for now, with the per-CPU API already in place).
4. Reclaim `LOADER_RECLAIM` memory after switching stacks.
5. ktests: an alloc/free stress test (100k ops, random orders); a no-leak check (free count restored); zone correctness; a double-free is detected in debug builds.

**Done when:** the ktests pass, and `/proc/meminfo`-style totals printed at boot match the BootInfo map.

### [ ] M2.3 Kernel paging `needs-owner`
**Needs:** M2.2
1. Build the kernel's own PML4:
   - HHDM
   - the kernel image with per-section permissions (text R-X, rodata R, data NX)
   - pre-allocated kernel-half PML4 entries 256-511
2. Switch to it and remove the loader identity mapping.
3. PAT setup (WB/WC/UC). Remap the framebuffer as WC.
4. Enable SMEP/SMAP/UMIP when present. Add the `vmmMapKernel`/`vmmUnmapKernel` APIs and the kernel virtual area allocator.
5. ktests: a write to the text segment faults; executing from a data page faults; the framebuffer mapping is WC (read the PAT bits back).

**Done when:** the ktests pass, and the boot log shows W^X verified.

### [ ] M2.4 Slab, kmalloc, vmalloc `needs-owner`
**Needs:** M2.3
1. Slab caches with constructors, per-CPU magazines, and `kmalloc` size classes from 16 to 8192.
2. `vmalloc` for large allocations, page-granular with guard pages.
3. Debug builds: poison on free, redzones, and double-free / use-after-free detection (poison check on alloc).
4. ktests: stress, alignment guarantees, overflow into a redzone is detected, and a guard page write faults.

**Done when:** the ktests pass.

### [ ] M2.5 BIOS loader `needs-owner`
**Needs:** M1.4, M2.1
1. stage1 (440 B NASM): relocate, INT 13h AH=42h reads of stage2 using the LBA/length that mkimage patched in. On error, print the error code.
2. stage2:
   - A20
   - E820
   - VBE mode pick (same rule as UEFI)
   - RSDP scan
   - 32-bit C with real-mode thunks for INT 13h/INT 10h
   - GPT + FAT32 readers reused from `boot/common`
   - boot menu
3. Load the kernel and initrd, build the page tables + BootInfo (`bootMethod = BIOS`), enter long mode, jump.
4. Update mkimage to install stage1 and stage2.
5. Harness: the boot matrix now runs **UEFI and BIOS** (SeaBIOS).

**Done when:** every existing test passes under both firmwares, including the screenshot tests
(with a separate reference image for the BIOS menu).
**Owner hardware check (optional):** if the board's CSM and your GPU allow legacy boot,
enable CSM and try the USB. If it won't come up, report what you see; the BIOS path stays
QEMU-verified only.

### [ ] M2.6 KASLR + kernel RNG `needs-owner`
**Needs:** M2.4, M2.5
1. Loaders: pick a 2 MiB-aligned slide inside the kernel window from the random seed, and apply the relocations from `--emit-relocs` (`R_X86_64_64`, `R_X86_64_32S`). Honor `kaslr=off`.
2. Kernel: the entropy pool (RDSEED/RDRAND, boot seed, interrupt timing later), a ChaCha20 CSPRNG, and `randomGetBytes`.
3. The symbolizer and panic output account for the slide.
4. ktests: two boots produce different `kernelVirtBase` (harness check); the RNG passes a basic statistical sanity test; ChaCha20 matches the RFC 8439 vectors.

**Done when:** the tests pass under both firmwares, and `kaslr=off` gives the fixed base.

---

## Phase 3: Cherry Pie (interrupts, time, SMP)

### [ ] M3.1 ACPI tables
**Needs:** M2.4
1. RSDP, then XSDT (with RSDT fallback), with checksum validation. Parse the FADT, MADT, MCFG, HPET, and IVRS into kernel structs. Load and keep the DSDT and SSDTs.
2. Map ACPI regions correctly (reclaim `ACPI_RECLAIM` only after parsing).
3. Add a `acpidump=1` cmdline option that dumps raw tables over serial as hex. Add `tools/acpiextract` to turn that log into files. Store QEMU's tables in `tests/data/acpi/qemu-q35/`.
4. Host tests: the table parsers run against the stored tables.

**Done when:** the host tests pass, and the boot log lists the CPUs from the MADT and the MCFG base.

### [ ] M3.2 Interrupt controllers `needs-owner`
**Needs:** M3.1
1. Remap and fully mask the 8259.
2. Local APIC: x2APIC when supported, otherwise xAPIC through MMIO mapped UC.
3. IOAPIC(s) from the MADT, honoring interrupt source overrides, polarity, and trigger.
4. Vector allocator (48-239), `irqRegister`, EOI handling, the spurious vector, and the vector plan from §7.2.
5. ktests: self-IPI delivery, and IOAPIC routing of the PIT/RTC IRQ in QEMU.

**Done when:** the ktests pass under both firmwares.

### [ ] M3.3 Timekeeping `needs-owner`
**Needs:** M3.2
1. ACPI PM timer driver, plus an optional HPET driver.
2. TSC: invariance check and calibration against the PM timer, with the frequency logged.
3. LAPIC timer calibration; one-shot mode, or TSC-deadline mode when CPUID says it's supported.
4. A per-CPU timer min-heap, `timeMonotonicNs`, and the CMOS RTC for `timeWallNs`.
5. ktests: `timeMonotonicNs` is monotonic across 1M reads; a 100 ms one-shot timer fires within ±5 ms (QEMU); the wall clock is within 2 s of the host's time (harness passes the host time on the cmdline).

**Done when:** the ktests pass.
**Owner hardware check:** boot on the PC and report the logged TSC frequency (expect about
4.5 GHz), whether the TSC is invariant, and whether an HPET was found.

### [ ] M3.4 Locking + lock validator `needs-owner`
**Needs:** M3.2
1. Ticket spinlocks, with irqsave/irqrestore, `preemptCount` plumbing (not preempting yet), and C11 atomics wrappers.
2. The lock validator (debug builds):
   - lock classes by init site
   - an acquisition-order graph, reporting inversions with both stacks
   - recursive-lock and IRQ-safety checks
3. ktests: a deliberate A→B / B→A inversion is reported; an IRQ-unsafe lock taken from an IRQ is reported.

**Done when:** the ktests pass, and the validator adds no false positives across the existing tests.

### [ ] M3.5 SMP bring-up `needs-owner`
**Needs:** M3.3, M3.4
1. `CpuLocal` through the GS base, and per-CPU GDT/TSS/IST stacks.
2. The trampoline page below 1 MiB. INIT-SIPI-SIPI per the SDM, with timeouts.
3. The AP path: real → protected → long mode → per-CPU init → idle `hlt` loop.
4. IPIs: call-function with completion, TLB shootdown (batched), and stop. The `cpus=N` cmdline option.
5. The per-CPU page caches from M2.2 become truly per-CPU.
6. Harness: the matrix is now **UEFI/BIOS × 1/4 CPUs**.
7. ktests: every CPU runs a call-function and increments a counter; a TLB shootdown stress test (map/unmap on one CPU while the others read); concurrent pmm stress on 4 CPUs.

**Done when:** the full matrix passes.
**Owner hardware check:** report the CPU count in the boot log. Expect 16 (8 cores × 2 threads).

### [ ] M3.6 CPU features + FPU state
**Needs:** M3.5
1. A CPUID feature bitset, the required-feature check (panicking with the missing feature's name), and `/proc/cpuinfo` data (exposed later).
2. XSAVE/XSAVEOPT area sizing (FXSAVE fallback). Eager save/restore hooks (used from M4.1).
3. `fpuBegin()`/`fpuEnd()` for kernel SIMD sections, with preemption disabled inside.
4. ktests: an FPU section keeps its state across an IPI storm.

**Done when:** the ktests pass on 1 and 4 CPUs.

### [ ] M3.7 Minimal AML + power off/reboot
**Needs:** M3.1
1. A minimal AML evaluator: enough namespace and package parsing to read `\_S5_`.
2. Shutdown through the PM1a/PM1b control registers. Reboot through the FADT reset register, then the 8042, then a triple fault.
3. The SCI handler and the power-button fixed event. For now it calls a kernel shutdown hook (M8.1 routes it to `svcd`).
4. Harness: `system_powerdown` sent through the QEMU monitor shuts the guest down cleanly (QEMU exits with 0 and a "shutdown" status).

**Done when:** the power-button test passes under both firmwares.
**Owner hardware check:** pressing the case power button while the kernel is idle powers the
PC off cleanly.

---

## Phase 4: Durban Poison (threads and scheduler)

### [ ] M4.1 Kernel threads + sleeping primitives `needs-owner`
**Needs:** M3.6
1. `Thread` struct, kernel stacks (16 KiB + a guard page), the `contextSwitch` asm (callee-saved registers + FPU through M3.6), and per-CPU idle threads.
2. `WaitQueue`, `Mutex`, `Semaphore`, `RwLock`, and `Completion`, integrated with the lock validator.
3. A temporary round-robin scheduler, just to run threads.
4. ktests: a producer/consumer through a semaphore; mutex contention with 8 threads on 4 CPUs; a stack overflow hits the guard page and panics with a clear message.

**Done when:** the ktests pass across the full matrix.

### [ ] M4.2 Scheduler classes `needs-owner`
**Needs:** M4.1
1. Per-CPU run queues. The normal class (vruntime, red-black tree, nice weights, target latency 6 ms, minimum granularity 1 ms). The background class. The realtime class (100 priorities, FIFO/RR, bitmap).
2. Wakeup preemption rules and `threadSetPriority`.
3. ktests:
   - two CPU-bound threads at nice 0 and nice 5 get shares within 10% of the expected ratio
   - a realtime thread always runs ahead of normal threads
   - a background thread gets 0% while a normal thread is busy

**Done when:** the ktests pass.

### [ ] M4.3 Kernel preemption `needs-owner`
**Needs:** M4.2
1. `needResched`, preemption on IRQ return when `preemptCount == 0`, and the check in `preemptEnable()`.
2. The lock validator catches "sleeping while atomic" and "might_sleep in IRQ".
3. ktests: a CPU hog in kernel mode gets preempted by a realtime thread within 2 ms; the sleeping-while-atomic check fires on a deliberate bug.

**Done when:** the ktests pass across the matrix, with no validator reports.

### [ ] M4.4 Load balancing + accounting `needs-owner`
**Needs:** M4.3
1. Idle CPUs steal from the busiest queue. Periodic balancing. Affinity masks.
2. Per-thread user/system time and per-CPU busy/idle time.
3. Work queues (kernel worker threads) and a deferred-work mechanism for drivers (bottom halves).
4. ktests: 64 CPU-bound threads on 4 CPUs finish within 15% of the ideal time; an affinity-pinned thread never migrates.

**Done when:** the ktests pass.

---

## Phase 5: Early Girl (objects, syscalls, IPC, first userspace)

### [ ] M5.1 Object + handle framework `needs-owner`
**Needs:** M4.4
1. Object header (type, refcount, koid, signals, observers). Per-process handle tables (index + generation encoding). Rights masks.
2. Internal APIs: `handleClose`, `handleDuplicate` (rights can only shrink), `handleReplace`, `objectWaitOne`, `objectSignal`.
3. ktests: rights can't grow; a stale handle (wrong generation) is rejected; the refcount reaches zero and the object is destroyed; a waiter wakes on a signal.

**Done when:** the ktests pass.

### [ ] M5.2 Address spaces + user memory `needs-owner`
**Needs:** M5.1
1. `AddressSpace` (red-black region tree), `VmRegion`, and `VmObject` `ANON` and `PHYS`.
2. The page fault handler (demand-zero), plus internal `vmMap`/`vmUnmap`/`vmProtect`.
3. `copyFromUser`/`copyToUser`/`copyStringFromUser` using STAC/CLAC and the fixup table.
4. ktests (run from kernel test threads with a user address space attached): demand-zero pages; protection faults; a bad user pointer in a copy returns `STATUS_BAD_ADDRESS` instead of panicking.

**Done when:** the ktests pass across the matrix.

### [ ] M5.3 Syscall entry + ring 3 `needs-owner`
**Needs:** M5.2
1. STAR/LSTAR/SFMASK, and the entry stub with `swapgs` and a per-CPU kernel stack.
2. `kernel/include/uapi/syscalls.def` (X-macro), `status.h`, and the dispatch table, plus the per-process personality field (NATIVE only).
3. Return through `sysret` (with a canonical-address check; fall back to `iretq`).
4. The first user program: hand-written asm embedded in the kernel. It calls `logWrite("hello from ring 3")` and `processExit(0)`.

**Done when:** a ktest sees the ring-3 message and the clean exit, and invalid syscall
numbers return `STATUS_NOT_SUPPORTED`.

### [ ] M5.4 IPC objects `needs-owner`
**Needs:** M5.3
1. Channels (bytes + handles, queue limits, `channelCall` with txid), Ports (observers and packets), Events/EventPairs, Timers, and futexes.
2. Expose them as syscalls in `syscalls.def`.
3. ktests with two user programs:
   - ping-pong 10k messages
   - handle transfer through a channel (sender loses it, receiver gets it with the same rights)
   - `PEER_CLOSED` delivered to a port
   - futex wait/wake

**Done when:** the ktests pass.

### [ ] M5.5 Processes, ELF, initrd, ASLR `needs-owner`
**Needs:** M5.4
1. `Process`, `Thread` (user threads), and `Job` objects, with `processCreate`/`processStart`/`threadCreate`/`threadStart`/`processExit`/`jobCreate`.
2. The ELF64 PIE loader (static binaries). User ASLR: PIE base, stack, mmap base, with at least 28 bits.
3. Bootstrap channel protocol (§10): args, env, and startup handles.
4. The loader loads the initrd (cpio newc). The kernel mounts a read-only view of it (a simple in-memory reader until the VFS exists) and starts `/sbin/init` from it.
5. `tools/mkinitrd`.

**Done when:** a C "hello" program built with the new toolchain (M5.7 may be developed in
parallel; a minimal crt0 is fine here) prints from the initrd, and two runs show different
load addresses.

### [ ] M5.6 User exceptions + processClone (COW) `needs-owner`
**Needs:** M5.5
1. User faults go to the process's exception channel if one is bound; otherwise the process is terminated with a crash report in klog (registers + backtrace using frame pointers).
2. `processClone`: duplicates the address space with copy-on-write (private writable pages marked read-only, refcounts bumped) and the handle table (copying only handles with `DUPLICATE`).
3. ktests: after a clone, parent and child each see their own writes; COW page counts return to baseline after exit; a crash report contains the faulting RIP.

**Done when:** the ktests pass across the matrix.

### [ ] M5.7 libc stage 1 + cross toolchain `[parallel-ok]`
**Needs:** M5.3 (for syscall numbers; stubs can be written against `syscalls.def` early)
1. `tools/bong-cc` wrapper and `build/sysroot` layout. crt0 (unpacks the bootstrap message).
2. Syscall stubs generated from `syscalls.def`.
3. `string.h`, `stdlib.h` (malloc on top of `vmMap`: a size-class allocator with free lists), `stdio.h` (FILE on handles, `printf` family), `ctype.h`, `stdint`-family, `errno` (per thread), and `assert`.
4. Host tests for the pure parts (printf formatting, string functions, the malloc algorithm).
5. `tests/user/` runner: test binaries in a test initrd that report through the same KTEST protocol.

**Done when:** the host tests pass, and a user test binary using `printf`, `malloc`, and `qsort` passes in QEMU.

---

## Phase 6: Forbidden Fruit (modules, PCI, input, storage drivers)

### [ ] M6.1 Loadable kernel modules
**Needs:** M5.5
1. `.kmod` build rules, `EXPORT_SYMBOL` / `.ksymtab`, `.modinfo`, and the `KERNEL_MODULE_ABI` check.
2. The loader (§12): relocations, symbol resolution, dependencies, section protections, `moduleInit`/`moduleExit`, refcounted unload.
3. `modules.idx` generation at build time, and loading from the initrd `/lib/modules`.
4. ktests: a test module loads, calls an exported function, and unloads; an ABI mismatch is refused; an unresolved symbol is refused with its name.

**Done when:** the ktests pass.

### [ ] M6.2 PCI/PCIe + driver registry
**Needs:** M6.1
1. ECAM through MCFG, bus enumeration, BAR sizing/mapping, the capabilities list, and MSI/MSI-X setup helpers.
2. The kernel device model: `Device`, `Driver`, match tables, binding, and automatic module loading through `modules.idx`.
3. The boot log prints the PCI device list (`bb:dd.f vendor:device class`).
4. ktests: the QEMU q35 device list matches the expected IDs; MSI-X vector allocation works on a virtio device.

**Done when:** the ktests pass.
**Owner hardware check:** send the logged PCI list from the PC. It identifies the exact NIC,
audio, and USB controllers for later milestones.

### [ ] M6.3 PS/2 input + InputDevice + keymaps
**Needs:** M6.2
1. i8042 module: keyboard (scancode set 2 → set 1 translation off, our own decoder) and mouse.
2. Kernel `InputDevice` objects and an event queue. Keymaps in `data/keymaps/` (US default; layouts are loadable).
3. fbcon echoes typed keys in a debug mode (`inputdebug=1`).
4. Harness: send keys through the QEMU monitor (`sendkey`) and assert the serial echo.

**Done when:** the harness test passes.

### [ ] M6.4 Block layer + partitions + virtio-blk + write guard
**Needs:** M6.2
1. `BlockDevice`, async `submitIo` with scatter-gather, flush, and a simple per-device queue.
2. GPT and MBR partition scanning. Partition devices are named `diskN`/`diskNpM`.
3. A shared virtio-pci modern transport library, plus the virtio-blk module.
4. **The write guard** (see the safety rule at the top of this file), with a ktest.
5. ktests: read and write a scratch virtio disk and verify checksums; writes to a non-boot disk without `diskwrite=` fail with `STATUS_ACCESS_DENIED`.

**Done when:** the ktests pass.

### [ ] M6.5 NVMe driver
**Needs:** M6.4
1. Controller reset/enable, the admin queue, Identify Controller and Namespace, and one I/O queue pair per CPU with MSI-X.
2. Read, write, and flush, including PRP lists for large transfers.
3. ktests on QEMU NVMe: a concurrent 4-CPU read/write stress with checksums; a flush; a large transfer (1 MiB).

**Done when:** the ktests pass.
**Owner hardware check:** boot from USB and report the NVMe model string and the partition
list detected on your SSD (read-only; the write guard blocks writes).

### [ ] M6.6 AHCI driver
**Needs:** M6.4
1. HBA init, port detection, and the command list and FIS. Read/write DMA with NCQ, and IDENTIFY.
2. ktests on the QEMU AHCI disk: the same stress test as NVMe.

**Done when:** the ktests pass.

---

## Phase 7: Girl Scout Cookies (VFS, page cache, filesystems, swap)

### [ ] M7.1 VFS core + namespaces `needs-owner`
**Needs:** M5.6, M6.4
1. `Vnode`, the dentry cache (with negative entries), `Mount`, `FileSystemType` ops, and the `File`/`Directory` objects.
2. Path lookup relative to a directory handle, with a `..` containment check at the namespace root and a symlink depth limit. Unix permission checks followed by rights checks.
3. `Namespace` objects with bind mounts, and the `fs*` + `namespace*` syscalls.
4. tmpfs. The initrd is unpacked into a tmpfs root (this replaces the M5.5 temporary reader).
5. ktests: a sandbox-escape attempt with `..`, a symlink pointing outside, and an absolute symlink all fail; a permission denial for another uid; bind-mount visibility is confined to one namespace.

**Done when:** the ktests pass.

### [ ] M7.2 devfs + procfs
**Needs:** M7.1
1. devfs: `/dev/null`, `zero`, `random`, `console`, `ttyS0`, and the block devices.
2. procfs: `meminfo`, `cpuinfo`, `interrupts`, `modules`, `uptime`, and `/proc/<pid>/{status,cmdline,maps}`.

**Done when:** a user test reads each file and validates its format.

### [ ] M7.3 Page cache + file mmap `needs-owner`
**Needs:** M7.1
1. A per-vnode page cache (radix tree), readahead, a writeback thread, and `fsSync`/fsync.
2. `VmObject FILE`: `vmMap` of files with PRIVATE (COW) and SHARED semantics; dirty tracking and writeback.
3. ktests / user tests: mmap-read equals read; writes through a shared mapping are visible to read after sync; a private mapping doesn't modify the file.

**Done when:** the tests pass across the matrix.

### [ ] M7.4 FAT32
**Needs:** M7.3
1. FAT32 read/write with LFN, directory create/delete, rename, and truncate. Mount the ESP read-write at `/boot`.
2. Interop tests:
   - the harness builds an image with host `mkfs.fat` and pre-populated files
   - a user test in bongOS verifies the files, then creates, modifies, and deletes files
   - the host runs `fsck.fat -n` and compares the file tree to the expected result

**Done when:** the interop tests pass, with fsck clean.

### [ ] M7.5 bongfs specification `needs-owner`
**Needs:** M7.1
1. The `architect` subagent drafts `docs/specs/bongfs.md` from ARCHITECTURE §15.1, covering:
   - exact byte layouts of the superblock, group descriptors, inodes, extents, directory blocks, and journal records
   - checksum coverage
   - feature flags
   - allocation policies
   - the journal commit and replay algorithm
   - crash-consistency invariants
   - the checks fsck performs
2. **No code in this milestone.** The PR is spec-only and waits for owner review.

**Done when:** the owner approves the spec PR.

### [ ] M7.6 bongfs core + host tools `[parallel-ok]` `needs-owner`
**Needs:** M7.5
1. `libs/bongfs` (portable C, with `HOSTED` shims): format, allocation, inodes, extents, directories (inline plus a hashed B+tree), and the journal.
2. Host tools: `mkfs.bongfs`, `fsck.bongfs` (checks and repairs), and `bongfs-cp` (copies a directory tree into an image).
3. Host tests:
   - a random operation fuzzer compared against an in-memory model
   - **crash-consistency tests**: a simulated power cut at random write boundaries, then journal replay, then fsck must report the image clean and consistent with the last commit

**Done when:** `make host-tests` passes, including 10k fuzz iterations and 1k crash points.

### [ ] M7.7 bongfs kernel driver + root filesystem `needs-owner`
**Needs:** M7.4, M7.6
1. Kernel integration of `libs/bongfs` with the page cache. Journal replay on mount.
2. mkimage builds the root partition as bongfs, populated with `bongfs-cp`. The early userspace in the initrd mounts root and pivots into it.
3. Tests: boot with root on bongfs; heavy file I/O from 4 threads; kill QEMU mid-write, then reboot, then journal replay, then host `fsck.bongfs` reports clean.

**Done when:** the tests pass across the matrix.

### [ ] M7.8 Swap + reclaim `needs-owner`
**Needs:** M7.7
1. A swap partition (type GUID recorded in DECISIONS) or a swapfile, the swap map, active/inactive lists, clock-style scanning, watermarks, and the reclaim thread.
2. Anonymous pages go to swap; clean file pages are dropped; dirty pages are written back first.
3. `test-full`: 128 MiB RAM, and a user program touches 400 MiB and checksums it all, which must pass. The page cache shrinks under pressure.

**Done when:** the `test-full` swap tests pass on 1 and 4 CPUs.

---

## Phase 8: Headband (userland foundation)

### [ ] M8.1 svcd + logd + svc tool
**Needs:** M7.7, M5.7
1. `svcd` as PID 1: parses `/etc/svc/*.svc`, orders services by dependencies, gives each service its own Job, applies restart policies with backoff, and handles orderly shutdown (with the M3.7 power-button hook routed here).
2. `logd`: the kernel log reader, a service log channel, `/var/log/*`, rotation, and `dmesg`.
3. `svc status|start|stop|restart|logs`.
4. Tests: a service configured to crash gets restarted with backoff; dependency order is respected; the power button leads to a clean shutdown sequence in the log.

**Done when:** the tests pass.

### [ ] M8.2 POSIX layer, part 1
**Needs:** M8.1
1. A file descriptor table over handles: `open`, `read`, `write`, `close`, `lseek`, `stat`/`fstat`, `dup`/`dup2`, `pipe`, `opendir`/`readdir`, `mkdir`, `unlink`, `rename`, `chdir`/`getcwd`, `access`.
2. Status → errno mapping, `environ`, `getenv`/`setenv`, `time`/`clock_gettime`/`nanosleep`, `posix_spawn`, and `waitpid` (on process-terminated signals plus exit codes).
3. User tests: a POSIX conformance mini-suite (`tests/user/posix/`).

**Done when:** the suite passes.

### [ ] M8.3 PTYs + terminals
**Needs:** M8.2
1. Kernel `Pty` objects with a line discipline (canonical and raw, echo, control characters). termios in libc.
2. The console device: fbcon and serial become ttys. `getty` on tty1 and ttyS0.
3. Tests: canonical-mode line editing, and raw-mode byte passthrough through a pty pair.

**Done when:** the tests pass.

### [ ] M8.4 bsh + coreutils, batch 1
**Needs:** M8.3
1. bsh:
   - a lexer and parser
   - pipelines and redirections (`> >> < 2>&1`)
   - variables and `$?`
   - `if`/`for`/`while`/functions
   - builtins (`cd`, `export`, `exit`, `source`, `alias`)
   - line editing with history and tab completion
   - a colored prompt
2. coreutils batch 1: `ls cat cp mv rm mkdir rmdir ln touch echo pwd head tail wc sort uniq grep find hexdump clear`.
3. Tests: script-based tests in bsh with expected output (`tests/user/bsh/*.bsh` plus golden files).

**Done when:** all golden tests pass.

### [ ] M8.5 Signals + fork + job control `needs-owner`
**Needs:** M8.4
1. Kernel async delivery to user threads (signal frames, and a `sigreturn` equivalent), process groups, and sessions.
2. POSIX: `sigaction`, `kill`, `raise`, masks, `SIGCHLD`, `SIGINT`/`SIGTSTP` from the pty, and `fork()` through `processClone`.
3. bsh job control: Ctrl-C, Ctrl-Z, `fg`, `bg`, `jobs`.
4. Tests: fork+exec+wait; a signal handler runs and resumes; Ctrl-C kills a foreground `sleep` in a pty test.

**Done when:** the tests pass across the matrix.

### [ ] M8.6 pthreads + coreutils, batch 2
**Needs:** M8.5
1. pthreads: create/join/detach, mutexes, condition variables, rwlocks, TLS through the FS base, and `pthread_once`. A thread-safe malloc.
2. coreutils batch 2: `ps kill top df du mount umount chmod chown date uname whoami id env sleep reboot poweroff free`.

**Done when:** a pthread stress test (8 threads × 100k mutex ops) and the golden tests pass.

### [ ] M8.7 Users + login + elevate `needs-owner`
**Needs:** M8.6
1. `libs/crypto`: BLAKE2b and Argon2id (RFC 9106 vectors in host tests).
2. Kernel credentials (uid, gid, groups), `credGet`/`credSet` (privileged), and permission enforcement across the VFS.
3. `/etc/passwd`, `group`, and `shadow` (Argon2id). `login`, `useradd`, `passwd`, and `elevate` (with `/etc/elevate.conf`, the admin group, and password re-prompt with a timeout).
4. Tests: a wrong password is rejected; a non-admin can't elevate; an admin can; a file made by user A is unreadable by user B under mode 600.

**Done when:** the tests pass.

---

## Phase 9: Ice Cream Cake (full ACPI, IOMMU, userspace drivers, USB)

### [ ] M9.1 Full AML interpreter `[parallel-ok for the host part]`
**Needs:** M3.7
1. The complete ACPI 6.x AML opcode set, namespace, methods, operation regions (SystemMemory, SystemIO, PCI_Config), Mutex/Event, Notify, `_OSI` (answers as recent Windows), `_STA`/`_INI`, `_PRT`, and GPEs.
2. A host-side test harness that loads the stored table sets and evaluates `_PRT`, `_S5_`, and `_STA` for every device.
3. The kernel uses `_PRT` for INTx routing.
4. Owner step: boot a Linux live USB on the PC, run `sudo acpidump > b650.dat`, and commit it to `tests/data/acpi/b650/` (the milestone's instructions explain how).

**Done when:** the host tests pass on the QEMU tables and on the B650 tables (once provided), and every existing test still passes.

### [ ] M9.2 AMD-Vi IOMMU `needs-owner`
**Needs:** M9.1, M6.2
1. IVRS parsing, device and command tables, and per-domain I/O page tables.
2. Kernel drivers get a kernel DMA domain.
3. APIs: domain creation, map/unmap, and invalidation.
4. ktests with QEMU `-device amd-iommu`: DMA from a device outside its domain is blocked and logged, and in-domain DMA works (use QEMU's `edu` test device).

**Done when:** the ktests pass in `test-full`.

### [ ] M9.3 Userspace driver framework `needs-owner`
**Needs:** M9.2, M8.1
1. `Resource`, `Interrupt`, and `BusTransaction` objects, and `ioPortGrant`.
2. A userspace `devmgr` service that receives the root resource, binds devices marked "userspace" to driver processes, and hands them exactly their resources.
3. When a driver crashes, its IOMMU domain is torn down and its IRQs are masked, then `svcd` restarts it.
4. Demo driver for QEMU's `edu` device (DMA + IRQ). Test: kill the driver mid-DMA, it restarts, and the device works again.

**Done when:** the tests pass in `test-full`.

### [ ] M9.4 xHCI + USB core + hubs
**Needs:** M6.2, M4.4
1. xHCI:
   - controller init
   - the command, event, and transfer rings
   - port status and reset
   - slot and endpoint contexts
   - control, interrupt, and bulk transfers
   - MSI-X
2. USB core: enumeration, descriptors, config selection, and interface-driver matching. The hub class driver.
3. Tests with QEMU `qemu-xhci` plus `usb-hub`, `usb-kbd`, and `usb-storage`: enumeration logs match the expected descriptors.

**Done when:** the tests pass.

### [ ] M9.5 USB HID as a userspace driver
**Needs:** M9.3, M9.4
1. A HID driver process: boot protocol keyboard and mouse, plus a report-descriptor parser for generic mice (wheel, extra buttons). It feeds `InputDevice` objects.
2. Tests: `sendkey` / mouse events through `usb-kbd`/`usb-tablet`; kill the HID driver and verify that input resumes within 2 s.

**Done when:** the tests pass.

### [ ] M9.6 USB mass storage
**Needs:** M9.4, M7.4
1. BOT transport with SCSI transparent commands (INQUIRY, READ CAPACITY, READ/WRITE 10/16) as a block device.
2. Test: a FAT32 image on `usb-storage` mounts, then read/write/verify.

**Done when:** the tests pass.
**Owner hardware check:** a USB keyboard and mouse work on the PC; a FAT32 flash drive mounts
and its files list correctly.

---

## Phase 10: Jack Herer (networking)

### [ ] M10.1 Net core + e1000 + virtio-net
**Needs:** M6.2, M4.4
1. `NetDevice`, `NetBuf`, the per-device receive worker, and loopback.
2. e1000 and virtio-net modules.
3. Harness: QEMU `-netdev user` with `filter-dump` to a pcap for debugging failed tests.

**Done when:** ktests send and receive raw frames through loopback, e1000, and virtio-net.

### [ ] M10.2 ARP + IPv4 + ICMP
**Needs:** M10.1
1. ARP cache, IPv4 (checksums, fragment reassembly, routing table), and ICMP echo.
2. Test: the guest pings the slirp gateway 10.0.2.2 and gets replies.

**Done when:** the test passes.

### [ ] M10.3 UDP + sockets
**Needs:** M10.2, M8.2
1. UDP, `Socket` objects, the native `socket*` syscalls, and port integration.
2. POSIX BSD sockets in libc: `socket`, `bind`, `sendto`, `recvfrom`, `setsockopt`, and `poll`/`select`.
3. Test: a UDP echo exchange with a host-side Python server over slirp.

**Done when:** the test passes.

### [ ] M10.4 TCP
**Needs:** M10.3
1. RFC 9293 state machine, the RFC 6298 retransmit timer, sliding windows with window scaling, timestamps, fast retransmit and recovery, and NewReno. `listen`, `accept`, `connect`, and `shutdown`.
2. A loss-injection test mode on loopback (`loopback.drop=N%`).
3. Tests:
   - the host `curl`s a tiny HTTP server in the guest through `hostfwd`
   - the guest downloads 50 MiB from a host HTTP server and the checksum matches
   - a loopback transfer with 5% drop still completes correctly

**Done when:** the tests pass.

### [ ] M10.5 netd: DHCP, DNS, SNTP
**Needs:** M10.4, M8.1
1. `netd`: a DHCPv4 client, a DNS stub resolver (over a channel, and `getaddrinfo` in libc), SNTP time sync, and interface configuration.
2. Tools: `net` (show and configure interfaces and routes) and `ping`.
3. Tests: slirp DHCP assigns 10.0.2.15; `getaddrinfo("example.com")` resolves through slirp DNS (the test uses a host-provided name when there is no internet).

**Done when:** the tests pass.

### [ ] M10.6 IPv6
**Needs:** M10.5
1. IPv6, NDP, ICMPv6, SLAAC, and dual-stack sockets.
2. Test: SLAAC address on slirp IPv6, `ping6` to the gateway, and a TCP connection over IPv6 to the host.

**Done when:** the tests pass.

### [ ] M10.7 Firewall `needs-owner`
**Needs:** M10.4
1. Connection tracking and a rule engine (5-tuple, state, interface).
2. Defaults: allow outbound, established, and related traffic; allow inbound ICMP echo; drop other inbound.
3. The `fw` tool, with rules persisted in `/etc/fw.rules`.
4. Tests: a host connection to an unlisted port is dropped; after `fw allow tcp 8080`, it works.

**Done when:** the tests pass.

### [ ] M10.8 RTL8125 (real hardware)
**Needs:** M10.5, owner confirmation of the exact chip (M6.2 PCI list)
1. The Realtek 2.5GbE driver: reset, the MAC address, descriptor rings, MSI-X, and link-state detection.
2. QEMU can't test this chip, so this milestone is **verified on hardware**. The code must still build, and it must pass a mock-register unit test for descriptor ring handling (host test).

**Owner hardware check:** boot the PC with Ethernet plugged in, check for a DHCP lease on
your network, `ping` your router, and resolve and ping a public hostname.

---

## Phase 11: Kosher Kush (crypto, TLS, SSH, packages)

### [ ] M11.1 Crypto primitives `[parallel-ok]` `needs-owner`
**Needs:** M1.1
1. `libs/crypto` (also built for the kernel):
   - SHA-256/512, HMAC, HKDF
   - ChaCha20-Poly1305
   - AES-128/256: AES-NI plus a constant-time bitsliced fallback
   - GCM, XTS
   - X25519, Ed25519
   - a bignum library, ECDSA P-256 verify, and RSA-PSS / PKCS#1 v1.5 verify
   - `cryptoEqual`, `cryptoWipe`
2. Host tests with the official vectors (§17), plus a check that `cryptoEqual` runs in constant time.
3. The kernel CSPRNG switches to the shared ChaCha20 implementation.

**Done when:** every vector test passes.

### [ ] M11.2 X.509 + TLS 1.3 client `needs-owner`
**Needs:** M11.1, M10.5
1. DER/ASN.1 parsing, X.509 parsing, and chain validation against the Mozilla CA bundle (`data/certs/`), with hostname verification.
2. A TLS 1.3 client: X25519, AES-GCM and ChaCha20-Poly1305, SNI, HelloRetryRequest, and alerts.
3. A `fetch` command (curl-like).
4. Interop tests:
   - the host runs `openssl s_server` with a test CA and each cipher suite, and the guest connects and verifies data
   - a bad certificate or wrong hostname is rejected

**Done when:** the tests pass.

### [ ] M11.3 SSH server `needs-owner`
**Needs:** M11.1, M10.7, M8.7
1. `sshd`:
   - the transport (curve25519-sha256, ssh-ed25519, chacha20-poly1305@openssh.com, aes256-gcm)
   - authentication with public keys (`~/.ssh/authorized_keys`) and passwords (shadow)
   - session channels with a PTY and shell, and exec
2. A host key is generated on first boot. `svc enable sshd` opens port 22 in the firewall.
3. Interop tests from the host with real OpenSSH (`hostfwd` 2222 → 22): key login and running a command; an interactive PTY session with a scripted `expect`-style check; a wrong key or password is rejected.

**Done when:** the tests pass.

### [ ] M11.4 Package manager `needs-owner`
**Needs:** M11.2
1. `libs/compress` deflate/inflate (reuse it if M12.2 already created it) and a tar library, with host tests against zlib-produced data.
2. `.bpkg` format, the `tools/pkgsign` host tool, a signed repo index, and `pkg install|remove|update|search|info|verify` with an installed-package database in `/var/lib/pkg`.
3. CI publishes the repo (index + packages) to GitHub Pages on release. The repo signing key is kept as a CI secret; the owner generates it (instructions in the milestone log).
4. Tests: install from a local test repo served by the host; a tampered package is rejected; remove leaves no files behind.

**Done when:** the tests pass.

---

## Phase 12: Lemon Haze (graphics and desktop)

### [ ] M12.1 Display + input objects + virtio-gpu
**Needs:** M9.5, M6.3
1. Kernel `Display` objects (mode info, a WC framebuffer mappable to user space, `setMode`, cursor plane, a 60 Hz `VSYNC` signal) and `InputDevice` handles for user space.
2. virtio-gpu module (2D: resources, scanout, transfer, flush, cursor).
3. A test client draws a gradient; `screendump` compared against the reference.

**Done when:** the screenshot test passes on the GOP framebuffer and on virtio-gpu.

### [ ] M12.2 libs/gfx `[parallel-ok]`
**Needs:** M1.1
1. Rasterizer: antialiased paths, lines, rects, rounded rects, box blur for shadows, alpha compositing, clipping, and damage-region utilities.
2. Decoders: PNG and BMP. PNG needs inflate: create `libs/compress` (deflate/inflate) here if it doesn't exist yet. M11.4 then reuses it instead of writing its own.
3. Host golden-image tests with `tools/imgdiff`.

**Done when:** the host tests pass.

### [ ] M12.3 Font engine `[parallel-ok]`
**Needs:** M1.1
1. TrueType/OpenType: `cmap`, `glyf`, `loca`, `hmtx`, `kern`, and basic GPOS kerning. An antialiased rasterizer, a glyph cache, UTF-8 decoding, line breaking, and fallback fonts.
2. Add the OFL fonts in `data/fonts/` (a UI sans + a monospace), with license files.
3. Host golden tests: rendered paragraphs compared to references.

**Done when:** the host tests pass.

### [ ] M12.4 Compositor v1
**Needs:** M12.1, M12.2, M8.6
1. The compositor protocol in `libs/proto/compositor.h` (surfaces, buffers as VmObjects, commits with damage, input events, focus, cursor).
2. Damage-tracked compositing, 60 Hz pacing, and a realtime input thread. A floating window manager with move, resize, raise, and focus.
3. Test clients and screenshot tests (two overlapping windows, a moved window, cursor rendering).

**Done when:** the screenshot tests pass.

### [ ] M12.5 UI toolkit v1
**Needs:** M12.3, M12.4
1. The widgets and layouts from §19, theme tokens (`data/themes/dark.theme`), HiDPI scaling (1x/2x), keyboard navigation, and focus rings.
2. A `uigallery` demo app showing every widget.
3. Golden screenshots of the gallery at 1x and 2x.

**Done when:** the screenshot tests pass.

### [ ] M12.6 Desktop shell + greeter
**Needs:** M12.5, M8.7
1. `desktop`:
   - the bottom panel: launcher menu, task manager, tray (clock; network and volume placeholders)
   - notifications
   - wallpaper (an original design)
   - the lock screen
2. The tiling mode toggle (Super+T, plus a setting stub), with the tiling layout inside the compositor.
3. `greeter`: graphical login that talks to the login backend. svcd starts the compositor, then the greeter, then the desktop session.
4. Screenshot tests: the greeter, the empty desktop, the launcher open, and two tiled windows.

**Done when:** the screenshot tests pass.
**Owner hardware check:** boot the PC to the greeter at native resolution, log in, open the
launcher, and move windows around with a USB mouse. Report whether anything is sluggish.

### [ ] M12.7 JPEG + GIF decoders `[parallel-ok]`
**Needs:** M12.2
1. Baseline and progressive JPEG (including chroma subsampling), and GIF (including animation frames).
2. Host golden tests.

**Done when:** the host tests pass.

---

## Phase 13: Maui Wowie (apps and sandboxing)

### [ ] M13.1 permd + GUI sandbox `needs-owner`
**Needs:** M12.6
1. The sandbox profile applied by the desktop launcher: a Job, and a namespace containing the app directory, `~/.local/share/<app>`, `/tmp/<app>`, and read-only fonts and themes.
2. `permd`: the file picker dialog, "Allow access to X?" prompts, and per-app grants persisted in `~/.config/permd/`. Grants hand directory or file handles to the app.
3. Tests: a sandboxed test app can't list `~/Documents` without a grant, and can after a scripted grant; revoking a grant takes effect on the next launch.

**Done when:** the tests pass.

### [ ] M13.2 Terminal
**Needs:** M12.5, M8.5
1. An xterm-256color emulator on a PTY: tabs, scrollback, selection and copy/paste (with a compositor clipboard protocol added here), font zoom.
2. Tests: `vttest`-style escape sequence golden screenshots, and bsh running inside it.

**Done when:** the tests pass.

### [ ] M13.3 Files + editor
**Needs:** M13.1
1. `files`: browse, grid and list views, copy, move, delete, rename, properties, and open-with.
2. `editor`: multi-tab, open and save through `permd`, find and replace, and undo/redo.
3. Scripted UI tests (input injection plus screenshot and file-system assertions).

**Done when:** the tests pass.

### [ ] M13.4 Settings, sysmon, calc, imageview
**Needs:** M13.1, M12.7
1. `settings`:
   - display (resolution and scale)
   - keyboard layout
   - users
   - network (from netd)
   - sound (placeholder until M14.2)
   - theme and accent color
   - window mode (floating/tiling)
   - app permissions (from permd)
2. `sysmon` (CPU per core, memory, processes with kill, network), `calc`, and `imageview` (PNG, JPEG, GIF, BMP; zoom and slideshow).
3. Screenshot and behavior tests for each.

**Done when:** the tests pass.

---

## Phase 14: Northern Lights (audio, dynamic linking, ports, DOOM)

### [ ] M14.1 HD Audio driver
**Needs:** M6.2, M4.4
1. HDA controller: CORB/RIRB, stream descriptors, BDL DMA, MSI.
2. Codec enumeration, a widget graph walk to find output paths, jack detection, and a volume and mute path. `AudioStream` objects.
3. Test: QEMU `intel-hda` + `hda-duplex` with `-audiodev wav`; play a 1 kHz tone, and the harness checks the captured WAV frequency and level.

**Done when:** the test passes.
**Owner hardware check:** a test tone plays from the board's rear line-out or headphones, and
from the monitor over HDMI or DisplayPort through the 4060 Ti.

### [ ] M14.2 sndd sound server
**Needs:** M14.1, M8.6
1. Client streams through shared ring buffers, mixing, resampling to 48 kHz, per-app and master volume, and a realtime thread.
2. The tray volume control and the Settings sound page.
3. Test: two clients' tones mix correctly in the WAV capture.

**Done when:** the test passes.

### [ ] M14.3 Audio decoders + player `[parallel-ok for decoders]`
**Needs:** M14.2 (for the player app); decoders need only M1.1
1. `libs/audio`: WAV, FLAC, MP3, and Ogg Vorbis decoders (host tests compare PCM output against reference decodes).
2. `player` app: a playlist, seeking, and a volume control.

**Done when:** the host decoder tests and the player playback test pass.

### [ ] M14.4 Dynamic linker
**Needs:** M8.6
1. `ld.so`: ELF dynamic loading, relocations, symbol lookup, TLS in shared objects, `dlopen`/`dlsym`, and ASLR for libraries.
2. `libc.so`. System binaries switch to dynamic linking (static stays available).
3. Tests: dynamic hello; `dlopen` of a plugin; the POSIX suite passes against the dynamic libc.

**Done when:** the tests pass.

### [ ] M14.5 Ports infrastructure + bash
**Needs:** M14.4, M11.4
1. `ports/` recipe format: source URL, checksum, patches, and build steps using `bong-cc`. CI builds ports into signed packages.
2. Port **bash**, and make it installable with `pkg install bash`.
3. Test: bash runs the bsh golden scripts translated to bash, plus a bash self-test subset.

**Done when:** the tests pass.

### [ ] M14.6 SDL2 + DOOM
**Needs:** M14.5, M14.2, M12.4
1. Port SDL2 with a bongOS backend: video through compositor surfaces, audio through sndd, and input.
2. Port chocolate-doom, with the **Freedoom** WAD packaged (`data/` or a pkg).
3. Test: the chocolate-doom demo playback renders its title screen (screenshot test), and audio is present in the WAV capture.

**Done when:** the tests pass. 🎮
**Owner hardware check:** play DOOM on the PC.

---

## Phase 15: OG Kush (more filesystems, installer, **v1.0**)

### [ ] M15.1 ISO9660
**Needs:** M7.3
1. ISO9660 read support with Joliet and Rock Ridge.
2. Tests: images made with host `xorriso` mount, and their files verify.

**Done when:** the tests pass.

### [ ] M15.2 exFAT
**Needs:** M7.3
1. exFAT read/write.
2. Interop with host `mkfs.exfat` / `fsck.exfat`, following the FAT32 test pattern.

**Done when:** the tests pass with fsck clean.

### [ ] M15.3 ext4 read
**Needs:** M7.3
1. Superblock, group descriptors, inodes, extents, htree directories, symlinks, and checksums (metadata_csum).
2. Tests: a host `mkfs.ext4` image with a populated tree is verified byte-for-byte from the guest.

**Done when:** the tests pass.

### [ ] M15.4 ext4 write (gated) `needs-owner`
**Needs:** M15.3
1. Writes work only when `ext4.write=1` is set: allocation, extents, directories, journaling (jbd2-compatible), and orphan handling.
2. Tests: a random operation fuzzer in the guest, then host `e2fsck -fn` must report clean. Kill QEMU mid-write, and after reboot plus journal replay, `e2fsck` must report clean.

**Done when:** 1k fuzz rounds and 100 crash points are all clean.

### [ ] M15.5 NTFS read-only
**Needs:** M7.3
1. MFT, attributes (resident and non-resident), runlists, directory indexes (B+ trees), and compressed files (stretch within the milestone).
2. Tests: an image built on the host with `mkntfs` + `ntfs-3g`, with its files verified from the guest.

**Done when:** the tests pass.

### [ ] M15.6 Installer `needs-owner`
**Needs:** M13.4, M7.7
1. A graphical `installer` app (plus a text fallback). Target selection: **whole disks only, and never the disk containing an existing OS unless the owner types the disk's model name to confirm.**
2. The installer partitions the disk (GPT, ESP, root, optional swap), formats bongfs, copies the system, installs both loaders, and creates the first user.
3. Tests: install to a blank QEMU disk, then reboot from that disk only, then log in.

**Done when:** the tests pass.
**Owner hardware check:** install to a spare or external drive (not the main NVMe), then boot
it through the UEFI boot menu.

### 🎉 v1.0.0 "OG Kush" release criteria
- Every Phase 1-15 milestone is done, and every owner hardware check has passed.
- `make test-full` is green. The image boots on the reference PC to the desktop, with
  networking, audio, packages, and installation working.
- The owner approves the release PR (`needs-owner`).

---

## Phase 16: Purple Punch (browser and text)

### [ ] M16.1 HTTP client + HTML parser `[parallel-ok for the parser]`
**Needs:** M11.2
1. HTTP/1.1 (keep-alive, chunked, redirects, gzip through our inflate). A WHATWG-lite HTML tokenizer and tree builder. A DOM.
2. Host tests: parser conformance against a subset of the html5lib tests.

**Done when:** the tests pass.

### [ ] M16.2 CSS + layout + browser app
**Needs:** M16.1, M12.5, M12.7
1. CSS parsing and cascade (selectors, specificity, inheritance). The box model, block and inline flow, fonts, colors, backgrounds, and images. Flexbox if time allows (otherwise a follow-up milestone).
2. `browser` app: tabs, an address bar, history, links, and page scrolling. **No JavaScript.**
3. Screenshot tests of local test pages. A manual check against a simple real site.

**Done when:** the tests pass.

### [ ] M16.3 Complex text: shaping, bidi, color emoji
**Needs:** M12.3
1. Full GSUB/GPOS shaping (Arabic, Devanagari, and other complex scripts), the Unicode bidi algorithm, and COLR/CPAL color emoji. Add the OFL fonts that cover them.
2. Host golden tests.

**Done when:** the tests pass.

---

## Phase 17: Quantum Kush (Linux binary compatibility)

### [ ] M17.1 Linux personality, part 1
**Needs:** M14.4, M8.5
1. Detect Linux binaries, and add the Linux x86_64 syscall table: the ~100 syscalls busybox needs, errno mapping, Linux signal frames, and `/proc` basics.
2. Test: an unmodified static **busybox** runs its applet test subset.

**Done when:** the tests pass.

### [ ] M17.2 Linux personality, part 2
**Needs:** M17.1
1. `clone` flags, futex, epoll (on top of ports), `ioctl`s for terminals, and `/compat/linux` sysroot handling for glibc's `ld-linux`.
2. Test: unmodified dynamic glibc command-line programs (coreutils, python3 from a Debian sysroot) run basic scripts.

**Done when:** the tests pass.

---

## Phase 18: Runtz (Secure Boot and disk encryption)

### [ ] M18.1 Secure Boot `needs-owner`
**Needs:** M15.6, M11.1
1. Host tooling: generate the owner's PK/KEK/db keys (instructions), and `sbsign` `BOOTX64.EFI`.
2. The loader verifies Ed25519 detached signatures on `kernel.elf` and `initrd.img`. The kernel verifies a signature on every `.kmod` (unsigned modules are refused when `secureboot=on`, and this becomes the default).
3. Tests in QEMU using OVMF with our keys enrolled (the secure-boot OVMF variant): a signed image boots; a tampered kernel or module is refused with a clear message.

**Done when:** the tests pass.
**Owner hardware check:** enroll your keys in the ASUS firmware (the step-by-step guide lives
in the milestone log), turn Secure Boot on, and boot. Tamper test: boot a deliberately
modified image and confirm it's refused.

### [ ] M18.2 Full-disk encryption `needs-owner`
**Needs:** M15.6, M11.1
1. The encrypting block layer (AES-256-XTS, using AES-NI) and a header format (Argon2id-wrapped master key, with 2 key slots).
2. An early-userspace passphrase prompt in the initrd, and an optional encryption step in the installer.
3. Tests: an encrypted install boots with the right passphrase and is refused with the wrong one; the raw disk shows no plaintext markers.

**Done when:** the tests pass.

---

## Phase 19: Sour Diesel (self-hosting and performance)

### [ ] M19.1 Performance pass
**Needs:** M12.6
1. PCID, tickless idle, the vDSO time page, direct-switch fast path for `channelCall`, priority inheritance on `Mutex`, TCP CUBIC + SACK, and USB UAS.
2. Add a benchmark suite (`tests/bench/`) whose results are recorded in the milestone log before and after.

**Done when:** no regressions, and the log shows improvements with numbers.

### [ ] M19.2 Self-hosting
**Needs:** M14.5, M17.2 optional
1. Port LLVM (clang, lld), make, nasm, and git (optional) through `ports/`.
2. Build bongOS on bongOS. The resulting image boots in QEMU nested (TCG) or is copied out and booted.

**Done when:** a bongOS-built `bongos.img` passes `make test` on the host.

---

## Phase 20: Trainwreck (stretch goals)

### [ ] M20.1 bongfs snapshots `needs-owner`
- Refcounted extents, copy-on-write for snapshotted inodes, and a `snapshot` tool (`create|list|rollback|delete`), behind an `incompat` feature flag.
- The spec is updated first.

### [ ] M20.2 AMD iGPU display
- Display-mode setting on the 7700X's RDNA2 iGPU (DCN): mode enumeration from the EDID, and setting a mode on the motherboard HDMI port.
- This milestone is hardware-verified only, and is a research-heavy stretch.

## Later list (unscheduled)
- Wi-Fi: needs a chip decision, firmware handling, 802.11, and WPA2/WPA3.
- Bluetooth: HCI over USB, L2CAP, HID over Bluetooth, and audio.
