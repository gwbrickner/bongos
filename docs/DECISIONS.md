# Decision log

This log is append-only. Each entry records a decision, the alternatives that were rejected,
and why. D-001 through D-046 were made by the owner during the planning session on
2026-09-24.

**Rules for new entries:**
- Changing an earlier decision needs a new entry that says "Supersedes D-xxx", plus an update
  to ARCHITECTURE.md in the same PR.
- Decisions that affect architecture need owner review: label the PR `needs-owner`.

| ID | Decision | Rejected | Why |
|---|---|---|---|
| D-001 | Goal: learning + daily driver + portfolio | single focus | Owner wants all three; drives "custom everything, but pragmatic ordering" |
| D-002 | Targets: QEMU first, then the owner's real PC | QEMU only; real HW first | Fast iteration + real-world payoff |
| D-003 | Kernel language: C17 + assembly (NASM) | Rust, C++, Zig | Owner choice |
| D-004 | Firmware: UEFI **and** legacy BIOS | UEFI only | Owner choice; both paths share a BootInfo ABI so the kernel doesn't care |
| D-005 | Own bootloaders from the start (no Limine/GRUB) | Limine | Owner choice: learn every piece |
| D-006 | UEFI loader first, BIOS loader a few milestones later | both first; BIOS first | Real PC uses UEFI; avoids weeks before first kernel output |
| D-007 | UEFI loader built on our own minimal headers written from the UEFI spec | gnu-efi, POSIX-UEFI | Fully custom; only ~12 protocols needed |
| D-008 | Boot media: one GPT disk image, USB-flashable, boots both ways (protective-MBR stage1 + BIOS boot partition + ESP) | hybrid ISO; both | Simplest path to real hardware |
| D-009 | Full SMP from day one; the kernel starts APs itself (INIT-SIPI-SIPI) | SMP later; single core | Owner choice; locking correct from the start |
| D-010 | Kernel architecture: hybrid (core subsystems in-kernel, IPC for optional userspace drivers) | monolithic; microkernel | Performance + crash isolation where it matters |
| D-011 | x86_64 only, with all CPU-specific code isolated in `arch/x86_64` | no abstraction; multi-arch now | Keeps an ARM64 port possible at near-zero cost |
| D-012 | In-kernel drivers loadable at runtime (`*.kmod`, exact ABI version match) | all built-in; userspace only | Flexibility; smaller kernel |
| D-013 | Scheduler: per-CPU run queues; classes realtime / normal (fair, vruntime) / background; idle stealing | round-robin; MLFQ | Desktop responsiveness |
| D-014 | Preemptible kernel | non-preemptible | Smooth input/audio; SMP locking already required |
| D-015 | VM features: demand paging + COW, memory-mapped files, swap, ASLR + KASLR | subsets | Owner wants all; ordered by dependency |
| D-016 | Compatibility: native API + POSIX layer in libc + Linux binary personality later | POSIX-native; no POSIX | Custom core, still ports software; Linux apps later |
| D-017 | Resources held as capability handles with rights; no ambient authority | Unix fds | Security; handles pass cleanly over IPC; POSIX fds map onto handles |
| D-018 | Native process creation is spawn-style; `fork()` emulated via the kernel `processClone` + COW (also used by Linux `clone`) | classic fork/exec; no fork | Clean native API; fork still available for ports |
| D-019 | Native FS: custom journaling "bongfs" (ordered mode, CRC32C); snapshots as a stretch goal | CoW FS; ext4 as native | Best reliability per effort |
| D-020 | Extra filesystems: FAT32 (required for ESP), ext4 (write gated), NTFS read-only, exFAT, ISO9660 | fewer | Owner choice |
| D-021 | One file tree + per-process namespaces | plain Unix tree; drive letters | Enables capability sandboxing |
| D-022 | Graphics: GOP/VBE framebuffer + virtio-gpu; no NVIDIA display driver; AMD iGPU display = stretch | real GPU modesetting/3D | Owner has an RTX 4060 Ti (infeasible); firmware framebuffer gives native resolution |
| D-023 | Drivers: USB (xHCI, hub, HID, storage), audio (HDA), Ethernet (e1000, virtio-net, RTL8125); no laptop support; Wi-Fi/Bluetooth on the later list | Wi-Fi now | Owner: "just Ethernet for now" |
| D-024 | Fully custom ACPI/AML interpreter (minimal early, full later) | port uACPI/ACPICA | Owner choice: fully custom |
| D-025 | Timers: invariant TSC + LAPIC one-shot/TSC-deadline, calibrated against the ACPI PM timer; HPET optional | require HPET | Some AM5 boards disable HPET |
| D-026 | Mandatory progress logging (STATUS.md + milestone logs, commit + push every working step) | — | Sessions can die on usage limits; the next session must resume cleanly |
| D-027 | Network stack in-kernel; NIC drivers as modules | userspace network server | Performance |
| D-028 | Network scope: IPv4 + IPv6, TCP, UDP, ICMP, DHCP, DNS, firewall, SSH server | smaller | Owner choice |
| D-029 | Custom crypto (TLS 1.3 client, SSH server, disk encryption), validated against official vectors + real-client interop, labeled EXPERIMENTAL | port a vetted library | Owner choice; honest labeling |
| D-030 | Shells: custom native `bsh` early, bash port later | one or the other | Shell needed before POSIX layer exists |
| D-031 | `svcd` service manager as PID 1, restarts crashed services and userspace drivers | simple init | Hybrid-kernel payoff |
| D-032 | Classic Unix directory layout | custom `/System`-style | Owner choice; ports expect it |
| D-033 | Custom package manager `pkg`, signed `.bpkg` packages, GitHub-hosted repo | none | Owner choice |
| D-034 | Self-hosting (port LLVM) as a late milestone | no | Owner choice |
| D-035 | Desktop: floating by default + tiling mode toggle | one mode | Owner choice |
| D-036 | Text: basic Unicode first; complex scripts + color emoji later | full Unicode first | Avoid an early rabbit hole |
| D-037 | Built-in apps: terminal, files, editor, settings, sysmon, imageview, player, calc, browser (no JavaScript) | fewer | Owner wants all |
| D-038 | SDL2 port + chocolate-doom with Freedoom | none | Owner choice |
| D-039 | Theme: dark, KDE Plasma-inspired layout with original assets | light; copied assets | Owner taste; keep assets legally clean |
| D-040 | Multi-user with login + `elevate` admin tool, Argon2id password hashes | single user | Owner choice; SSH needs it |
| D-041 | Sandbox GUI apps (namespace + `permd` portal); terminal tools unsandboxed | all or none | Owner choice |
| D-042 | Secure Boot with the owner's own keys; late milestone | none | Owner choice |
| D-043 | Full-disk encryption (AES-XTS + Argon2id), late, optional at install | none | Owner choice |
| D-044 | Public repo, MIT license | GPLv3; private | Portfolio; free unlimited CI minutes |
| D-045 | Merge policy: auto-merge on green CI + no Critical review findings, except `needs-owner` areas (memory, interrupts/SMP, scheduler, security/crypto, on-disk formats, boot ABI) | review all; merge all | Momentum with oversight of risky areas |
| D-046 | Name "bongOS"; per-phase cannabis-strain codenames; OS name only in `branding/`; identifiers use subsystem prefixes, camelCase functions, PascalCase types, UPPER_SNAKE constants | OS-name prefix in code | Owner choice; renaming stays a one-directory change |
| D-047 | Kernel built with `--emit-relocs`; loader applies the KASLR slide | PIE kernel | Keeps `-mcmodel=kernel` codegen; standard approach |
| D-048 | Initrd format: cpio newc | custom archive | Trivial to parse, host tools exist |
| D-049 | IPC protocols are hand-written C structs in `libs/proto`, no IDL compiler | IDL | Less tooling to build and maintain |
| D-050 | AMD-Vi IOMMU used to isolate userspace drivers; QEMU tests run with `amd-iommu` | trust drivers | Userspace drivers aren't isolated without it |
| D-051 | Every currently-empty ARCHITECTURE §2 directory gets a `README.md` naming what lands there and which milestone adds it | leave the directories out of git until populated | Keeps the repo tree matching ARCHITECTURE from day one; a reader doesn't need to open ARCHITECTURE.md to see the plan |
| D-052 | `branding.h` (generated by `tools/gen-branding.sh` from `branding/{name,version,codenames.tsv}`) exposes `BRANDING_NAME`/`BRANDING_VERSION`/`BRANDING_CODENAME`, not OS-prefixed; the codename is picked from the phase encoded in `branding/version`'s major.minor (mirroring the tag math in `.github/workflows/release.yml`), falling back to `"unreleased"` | prefixing the macros with the OS name | D-046 already rejects the OS name in code identifiers; renaming stays a one-directory change |
| D-053 | `make image`/`test`/`test-full` no-op cleanly (exit 0, clear message) while `boot/uefi/` and `boot/common/` have no `.c` sources; once they do, a missing build rule is a hard `$(error)` instead of a silent skip | make them fail now; or let them silently skip forever | ROADMAP M1.1 explicitly allows a no-op `test` until M1.2; the `$(error)` stops that no-op from surviving past the milestone that's supposed to replace it |
| D-054 | Host tests (`tests/host/`) use a constructor-registered `TEST()` macro, built with the host's own `clang` (not the cross toolchain) plus ASan/UBSan, one binary per `make host-tests` run | a TAP-style external runner; per-file binaries | Zero-config test registration; matches ARCHITECTURE §23 ("runs natively on Linux, fast"); sanitizers are cheap here and this is exactly the code (allocators, bongfs, crypto vectors, parsers) where they pay off most |
| D-055 | `.github/workflows/release.yml` skips compressing/publishing when `make RELEASE=1 image` produces no `build/bongos.img`, instead of failing the workflow | fail the release workflow on every milestone until M1.2 | Milestones before M1.2 have nothing to release; once boot sources exist, `mk/image.mk`'s own `$(error)` (D-053) makes a missing image a hard build failure, so this skip is self-limiting to the pre-M1.2 window |
| D-056 | GPT partition type GUIDs, minted with `cat /proc/sys/kernel/random/uuid` (the kernel's own RFC 4122 v4 generator), one call per constant, hardcoded in `tools/mkimage/gpt.c`/`gpt.h`: root = `D873F840-6583-4B38-8C8B-FA27F8332D8D` (`GPT_TYPE_GUID_ROOT`), swap = `1FC85B55-4413-4D12-8C5F-AD7BFAA6CB9D` (`GPT_TYPE_GUID_SWAP`, not yet used -- minted now so both format constants get owner review together). Each GUID names a partition **role**, not a filesystem: the root partition's actual contents are identified by their own on-disk magic (a bongfs superblock today; once full-disk encryption exists, ARCHITECTURE §15.2's crypt header wrapping one) -- a root partition with no recognized magic is unformatted, never corrupt, and must never be auto-formatted. The kernel finds root by scanning for `GPT_TYPE_GUID_ROOT` only on the disk whose GPT DiskGUID equals `BootInfo.bootDiskGuid`, never by scanning every disk | deriving the GUID from a name (UUIDv5); reusing an existing filesystem's type GUID (e.g. Linux's `0FC63DAF-8483-4772-8E79-3D69D8477DE4`, which would make Linux hosts probe and auto-mount it); an all-zero/reserved type until bongfs exists (UEFI §5.3.3 defines an all-zero type as "unused entry", so the space would look free and a tool could allocate over it); a GUID meaning "bongfs" specifically (breaks under FDE, where the partition holds a crypt header instead) | ARCHITECTURE §5.1 calls for "a custom type GUID recorded in DECISIONS.md" for root (and, optionally, swap); `tools/mkimage` (M1.2) already needs to label the empty partition 3 with its eventual role so real GPT tools (and later, the kernel's own GPT scanner, M6.4) can identify it, even though bongfs itself doesn't exist until M7.6. Reviewed by the `architect` subagent (docs/logs/M1.2.md) |
| D-057 | `tests/harness/run-qemu.sh` gained a `--expect-serial PATTERN` banner-match mode: PASS once PATTERN appears on serial (polling, killing QEMU on match/timeout), instead of the isa-debug-exit/KTEST exit-code protocol | wait for M1.3's kernel before wiring `make test` to the UEFI loader at all; give the M1.2 loader a fake KTEST report over serial | ARCHITECTURE §23's isa-debug-exit/KTEST protocol needs a kernel, which doesn't exist until M1.3, but ROADMAP M1.2's Done-when ("`make test` boots the loader under OVMF and sees the banner") needs `make test` to actually assert something now; extending run-qemu.sh (CLAUDE.md says extend, not rewrite) keeps one harness for both modes, and `mk/test.mk` drops `--expect-serial` the moment M1.3 lands |
