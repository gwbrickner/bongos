# bongOS Architecture

This is the source of truth for every design decision. If code and this document disagree,
this document wins, unless a newer entry in `docs/DECISIONS.md` changes it. When that
happens, update this document in the same PR.

---

## 0. Vision and scope

bongOS is a from-scratch x86_64 desktop operating system written in C and assembly. It is a
learning project, a daily-driver target, and a portfolio piece all at once.

**Everything in the base system is custom:** the bootloaders, kernel, drivers, libc,
crypto, GUI toolkit, apps, and tools. The only third-party material allowed:
- **Data** with permissive licenses: fonts (OFL), the Mozilla CA bundle, the Freedoom WAD,
  and keymap data.
- **Ported applications** under `ports/`: bash, SDL, LLVM, and so on. These are built from
  upstream source plus our patches and are never copied into the base tree.
- **Host-side tools** that run on the Linux build machine: clang, nasm, qemu, mkfs.*, fsck.*,
  sbsign, OpenSSH, and curl. They are used for building and testing only.

**Non-goals:**
- 3D GPU acceleration.
- NVIDIA display drivers.
- Laptop features (battery, backlight, lid, touchpad).
- 32-bit x86.

**Later list (unscheduled):** Wi-Fi and Bluetooth.

**Stretch goals:**
- Display-mode setting on the AMD iGPU.
- bongfs snapshots.
- Full complex-script text shaping.

---

## 1. Targets

### 1.1 QEMU (primary development and CI target)
- Machine `q35`, `-cpu max` (TCG in cloud/CI, KVM locally if available).
- Firmware: **OVMF** (UEFI) and **SeaBIOS** (legacy BIOS). Every `make test` boots both.
- CPUs: tested at **1 and 4 vCPUs**. RAM: 512 MiB default; swap tests use 128 MiB.
- Devices exercised over the roadmap: NVMe, virtio-blk, AHCI, e1000, virtio-net, virtio-gpu,
  intel-hda + hda-duplex, qemu-xhci + usb-kbd/usb-mouse/usb-storage, PS/2, `amd-iommu`.

### 1.2 Reference real machine
| Part | Hardware | bongOS support |
|---|---|---|
| CPU | AMD Ryzen 7 7700X (Zen 4, AM5) | x2APIC, invariant TSC, AES-NI, PCID, SMEP/SMAP |
| Board | ASUS Prime B650 (exact model confirmed with `lspci`/board label at the NIC milestone) | UEFI + CSM |
| GPU | NVIDIA RTX 4060 Ti | UEFI GOP / VBE framebuffer only, at native resolution |
| iGPU | Radeon (RDNA2) in the 7700X | framebuffer now; display-mode setting is a stretch goal |
| Storage | NVMe SSD | NVMe driver |
| Network | Onboard Realtek Ethernet (expected RTL8125 2.5GbE; confirm) | RTL8125 driver |
| Audio | Onboard HD Audio (Realtek codec) + NVIDIA HDMI audio | HDA driver + generic codec parser |
| USB | xHCI controllers (CPU + chipset) | xHCI stack |

### 1.3 Minimum CPU
x86_64 with NX, SSE2, APIC, and CMPXCHG16B. An invariant TSC is required on real hardware
(a warning is printed under QEMU without it). XSAVE is used when present.

---

## 2. Repository layout

```
boot/
  common/          shared loader code: boot.cfg parser, ELF loader, page-table builder,
                   BootInfo builder, GPT + FAT32 readers (compiled for both loaders)
  uefi/            UEFI loader (PE32+); own UEFI headers in boot/uefi/include/efi/
  bios/            stage1 (MBR, NASM), stage2 (NASM real-mode thunks + 32-bit C)
kernel/
  arch/x86_64/     the ONLY place with x86 specifics: cpu, gdt, idt, apic, paging, smp,
                   context switch, syscall entry, port I/O, iommu, fpu
  include/         kernel headers; include/arch/ is the arch interface; include/uapi/ is
                   shared with userspace (syscalls.def, status codes, object/rights ids)
  core/            init, panic, klog, objects & handles, process, thread, scheduler,
                   syscall dispatch, exceptions/signal delivery, time, random
  mm/              early allocator, buddy pmm, slab/kmalloc, vmalloc, vmm, page cache, swap
  sync/            spinlock, mutex, rwlock, semaphore, waitqueue, completion, lock validator
  ipc/             channel, port, event, timer, futex objects
  fs/              block layer, partitions, vfs, namespaces, devfs, procfs, tmpfs, initrd,
                   fat32, bongfs, iso9660, exfat, ext4, ntfs, crypt (disk encryption)
  net/             netdev, netbuf, arp, ipv4, ipv6, icmp, udp, tcp, sockets, firewall
  drivers/         built-in drivers: serial, fbcon, pci, acpi (+aml), timers, rtc
  modules/         loadable driver modules (*.kmod): nvme, ahci, virtio-*, e1000,
                   rtl8125, ps2, xhci, usb-hub, usb-storage, hda
  compat/linux/    Linux personality (late)
  test/            in-kernel test framework (ktest) and tests
libs/
  libc/            custom libc: native API wrappers, C17 standard library, POSIX layer
  crypto/          crypto primitives (also compiled into the kernel)
  bongfs/          bongfs core logic shared by kernel, mkfs, and fsck
  gfx/             2D rasterizer, image decoders (PNG/JPEG/BMP/GIF), font engine
  ui/              GUI toolkit
  net/             DHCP, DNS, TLS 1.3, HTTP/1.1, SSH protocol libraries
  proto/           IPC message definitions (C structs) for system services
  audio/           audio decoders (WAV, FLAC, MP3, Ogg Vorbis)
system/            svcd (init/service manager), logd, devmgr, netd, sndd, permd,
                   compositor, desktop (panel/launcher/tray), greeter, getty, login, sshd
apps/              terminal, files, editor, settings, sysmon, imageview, player, calc, browser
cmds/              bsh (native shell), coreutils, pkg, elevate, mount, fw, useradd, ...
ports/             recipes + patches: bash, SDL2, chocolate-doom, LLVM (clang/lld), make, nasm, ...
tools/             host tools: mkimage, mkfs.bongfs, fsck.bongfs, bongfs-cp, mkinitrd,
                   symbolize, imgdiff (screenshot compare), pkgsign, bong-cc (cross wrapper)
data/              fonts, icons, wallpapers, themes, keymaps, CA bundle, Freedoom
tests/             test harness, boot matrix, interop tests, reference screenshots, data/
branding/          OS name, version, codename: the ONLY place the OS name is defined
docs/              ARCHITECTURE, DECISIONS, ROADMAP, STATUS, logs/, specs/
.claude/           Claude Code agents, settings, hooks
```

---

## 3. Build and toolchain

- **Host:** Ubuntu 24.04 (cloud sessions and CI) or any modern Linux. GNU make, clang/lld/llvm
  18+, nasm, python3 (test harness only).
- **One top-level Makefile** includes a `*.mk` fragment per component. There is no autotools
  or CMake.

| Target | How it's built |
|---|---|
| Kernel | `clang --target=x86_64-unknown-elf -std=c17 -ffreestanding -nostdlib -mno-red-zone -mgeneral-regs-only -mcmodel=kernel -fno-pic -fno-omit-frame-pointer -fstack-protector-strong -Wall -Wextra -Werror`, linked by `ld.lld` with `kernel/arch/x86_64/kernel.ld` and `--emit-relocs` (needed for KASLR) |
| Modules | same flags; `ld.lld -r` produces relocatable `*.kmod` |
| UEFI loader | `clang --target=x86_64-unknown-windows -ffreestanding -fshort-wchar -mno-red-zone -mgeneral-regs-only -fno-stack-protector`, `lld-link /subsystem:efi_application /entry:efiMain /nodefaultlib` |
| BIOS stage1 | NASM flat binary, 440 bytes max |
| BIOS stage2 | NASM (16-bit entry + real-mode thunks) + C via `clang --target=i386-unknown-elf -ffreestanding -m32`, linked to a flat binary |
| Userspace | `tools/bong-cc` wraps `clang --target=x86_64-unknown-elf --sysroot=build/sysroot` with our crt0/libc; PIE by default |
| Host tools | normal host clang, sharing `libs/bongfs` etc. via `#ifdef HOSTED` shims |

**Build types:**
- `make` is the debug build: `-O1`, UBSan with custom kernel handlers, the lock validator,
  poisoned freed memory, and extra asserts.
- `make RELEASE=1` uses `-O2` and keeps stack canaries and NX. Heavy checks are off.

**Kernel UBSan (debug builds, D-076):** an explicit check list, never the bare
`-fsanitize=undefined` group -- `alignment,bool,builtin,bounds,enum,integer-divide-by-zero,
nonnull-attribute,null,object-size,pointer-overflow,returns-nonnull-attribute,shift,
signed-integer-overflow,unreachable,vla-bound`. The bare group pulls in `function` for C under
clang 18, which prefixes every function with an 8-byte type-hash checked on indirect calls --
hand-written assembly entry points carry no such prefix, so an indirect call into one reads
unmapped memory just before it. `local-bounds` is also excluded (emits a bare `ud2`, bypassing
the handler entirely). Every check always panics; there is no log-and-continue mode.

**`KERNEL_DEBUG` (debug builds, D-082):** a second, coarser debug-build define alongside
`KERNEL_UBSAN`, gating extra-cost invariant checks that aren't sanitizer instrumentation -- today,
the pmm's write-after-free poisoning (§6.2), `list.h`'s NULL-the-links-on-remove hardening, and
the slab allocator's redzones (checked on free) and payload poisoning (checked on next reuse,
D-096, §6.2). Misuse that's cheap enough to detect unconditionally (the pmm's double-free/misuse
state machine; the slab allocator's own bufctl-based double-free/bad-pointer checks) stays on in
release builds too; `KERNEL_DEBUG` is only for checks with a real per-operation cost.

**Make targets:** `all`, `image` (-> `build/bongos.img`), `test` (quick matrix), `test-full`,
`host-tests`, `debug`, `run`, `run-bios`, `gdb`, `format`, `lint`, `clean`.

---

## 4. Coding conventions

- **C17**, formatted by the repo's `.clang-format`: 4-space indent, 100 columns, braces on
  the same line. CI runs `make format-check`.
- **Names:**
  - functions and variables: `camelCase`
  - types, structs, and typedefs: `PascalCase`
  - macros, constants, and enum values: `UPPER_SNAKE`
- **Subsystem prefixes** on non-static functions: `pmmAllocPages`, `vmmMapPage`,
  `schedYield`, `vfsOpen`, `tcpSend`. **The OS name never appears in code identifiers.** It
  lives only in `branding/`, so a rename touches one directory.
- **Errors:** kernel functions return `Status` (an `int32_t` enum): `STATUS_OK = 0`, and
  errors are negative. The kernel has no errno. The full list is in
  `kernel/include/uapi/status.h`.
- Only freestanding headers are used in the kernel: `stdint.h`, `stddef.h`, `stdbool.h`,
  `stdarg.h`, `stdatomic.h`.
- The kernel has no VLAs and no unbounded recursion. The AML interpreter and path lookup use
  explicit depth limits.
- **Every non-static kernel function has a contract comment:** which locks must be held, and
  whether it may sleep, may be called from IRQ context, or can fail.
- **Assembly** lives only in `kernel/arch/` and `boot/`.

---

## 5. Boot

### 5.1 Disk image (`tools/mkimage` -> `build/bongos.img`, default 2 GiB, `dd`-able to USB)
GPT disk:
| # | Partition | Contents |
|---|---|---|
| LBA 0 | Protective MBR | boot-code area (440 B) = **BIOS stage1**; stage2 LBA/length patched in by mkimage |
| 1 | BIOS boot partition, 1 MiB, type `21686148-6449-6E6F-744E-656564454649` | **BIOS stage2**, raw |
| 2 | EFI System Partition, FAT32, 256 MiB | `/EFI/BOOT/BOOTX64.EFI`, `/bong/boot.cfg`, `/bong/kernel.elf`, `/bong/initrd.img` |
| 3 | Root (type GUID recorded in D-056: a partition *role*, not a filesystem -- identify actual contents by their own on-disk magic) | the system; until bongfs exists the root is the initrd |
| 4 | Swap (optional; type GUID also recorded in D-056) | swap space |

- The ESP is mounted at `/boot`.
- The initrd is a **cpio newc** archive containing early userspace and `/lib/modules/*.kmod`
  plus `modules.idx`.
- The image assumes 512-byte logical sectors throughout (mkimage's LBA math, GPT header fields).
  USB media is commonly 512e (512-byte logical, larger physical); a loader or driver that reads
  block size must take it from the device (`BlockIo->Media` under UEFI) rather than assuming 512.

### 5.2 `boot.cfg` (grammar: D-067)
A line-oriented `key = value` file, at most 64 KiB, UTF-8 (a leading BOM is skipped, a NUL byte
is an error). Lines end at LF; a trailing CR is stripped. Each line is trimmed, then: blank
lines are skipped; a line starting with `#` is a whole-line comment (no inline comments); a line
starting with `[` opens a section, whose trimmed inner text (1-63 bytes, printable ASCII, no
`[`/`]`, unique) is the entry's **display name** in the menu -- there is no separate `name =`
key; every other non-blank line must contain `=`, else it's a fatal error. A key is
`[a-z0-9_.]{1,31}`; an unrecognized key is ignored (forward-compatible) but counted. Within one
scope the first occurrence of a key wins.

**Keys**, valid both before the first `[section]` (the *global* scope) and inside a section
(each section is its own scope), with entry-then-global-then-built-in inheritance (an explicit
empty `initrd =` or `cmdline =` in a section overrides an inherited non-empty value):
- `kernel` (`/`-prefixed path; built-in default `/bong/kernel.elf`)
- `initrd` (path; built-in default: none; not yet loaded, D-067 -- M5.5)
- `cmdline` (any bytes but NUL; built-in default: empty)
- `resolution = auto | WIDTHxHEIGHT` (built-in default `auto`)
- `kaslr = on | off` (built-in default `on`; no effect until M2.6)

`timeout` (seconds, 0-3600, or `forever`; default 0 = no menu) and `default` (a 1-based index if
all digits, else an exact entry name; default 1) are **global-only**. At most 9 `[entry]`
sections (so serial digit keys 1-9 can select directly); a file with none is treated as one
implicit entry taking every value from the global scope, so an M1.3-style `kernel =`/`cmdline =`
file still boots unchanged. `tools/mkimage` runs the same parser (`boot/common/bootcfg.c`) at
image-build time and fails the build on a malformed `boot.cfg`.

**Default entries** (`boot/boot.cfg`, generated from `boot/boot.cfg.in`):
- **bongOS**
- **Safe mode** (`cmdline = cpus=1 nomodules fbcon=on`)
- **Serial debug** (`cmdline = loglevel=debug`)

The menu is rendered on the framebuffer and on serial (arrow keys + Enter on the framebuffer
side; number keys 1-9 on serial), per §5.5.

### 5.3 BootInfo: the handoff ABI (`boot/common/include/bootinfo.h`, shared with the kernel)

```c
#define BOOTINFO_MAGIC   0x544F4F42474E4F42ULL   /* "BONGBOOT" */
#define BOOTINFO_VERSION 1                         /* bump on any layout change */
#define BOOTINFO_CMDLINE_MAX 4096                  /* bytes, including the NUL */

typedef enum { BOOT_METHOD_UEFI = 1, BOOT_METHOD_BIOS = 2 } BootMethod;

typedef enum {
    BOOT_MEM_USABLE = 1, BOOT_MEM_RESERVED, BOOT_MEM_ACPI_RECLAIM, BOOT_MEM_ACPI_NVS,
    BOOT_MEM_BAD, BOOT_MEM_LOADER_RECLAIM, /* BootInfo, loader page tables, boot stack */
    BOOT_MEM_KERNEL, BOOT_MEM_INITRD, BOOT_MEM_FRAMEBUFFER
} BootMemType;

typedef struct { uint64_t base; uint64_t length; uint32_t type; uint32_t reserved; } BootMemRegion;

typedef struct {
    uint64_t phys; uint32_t width, height, pitch, bpp;   /* pitch is bytes per scanline */
    uint8_t redShift, redSize, greenShift, greenSize, blueShift, blueSize, reserved[2];
} BootFramebuffer;

typedef struct BootInfo {
    uint64_t magic; uint32_t version; uint32_t size;
    uint32_t bootMethod; uint32_t memMapCount;
    uint64_t memMapPhys;                 /* BootMemRegion[memMapCount], sorted, non-overlapping */
    BootFramebuffer fb;
    uint64_t rsdpPhys;                   /* ACPI RSDP */
    uint64_t kernelPhysBase, kernelVirtBase, kernelSize, kaslrSlide;
    uint64_t initrdPhys, initrdSize;
    uint64_t cmdlinePhys;                /* NUL-terminated */
    uint64_t hhdmBase;                   /* virtual base of the direct physical map */
    uint64_t loaderTsc;                  /* TSC when the loader started (boot-time stats) */
    uint64_t efiSystemTablePhys;         /* 0 on BIOS; kernel does not use runtime services in v1 */
    uint8_t  bootDiskGuid[16], bootPartGuid[16];  /* so the kernel can find its disk */
    uint8_t  randomSeed[64];             /* EFI_RNG / RDSEED / RDRAND / TSC jitter */
} BootInfo;
```

All addresses are physical. The kernel reads them through the HHDM. If the kernel sees a
different `version`, it refuses to boot and says so on serial and on the framebuffer. A zero
value means "not provided" for `fb.phys`, `initrdPhys`/`initrdSize`, `rsdpPhys`,
`efiSystemTablePhys`, and both GUID fields (D-064).

### 5.4 Machine state at kernel entry (both loaders)
- **Mode:** long mode with loader-built page tables:
  - the direct map of every RAM-backed EFI descriptor type the loader sees pre-ExitBootServices
    (Loader{Code,Data}, BootServices{Code,Data}, Runtime{Code,Data}, Conventional, ACPIReclaim,
    ACPINVS), clipped at the 64 TiB HHDM window, at `hhdmBase = 0xFFFF800000000000`, using 1 GiB
    pages (or 2 MiB pages without PDPE1GB); MMIO, reserved, persistent, and unaccepted memory are
    never HHDM-mapped (D-059), **except** the framebuffer (D-068, below). This loader-built HHDM
    is temporary: M2.3 (D-086) rebuilds a narrower one from the BootInfo memory map itself --
    USABLE/LOADER_RECLAIM/KERNEL/INITRD/ACPI_RECLAIM/ACPI_NVS only (RESERVED/BAD dropped
    entirely; ACPI's RSDP, for instance, is not HHDM-reachable after M2.3), with the kernel's own
    text+rodata physical range carved out read-only
  - the kernel image at `kernelVirtBase` (slid when KASLR is on)
  - an identity mapping of the loader's trampoline page only; the kernel removes it
  - if `fb.phys != 0`, the framebuffer (`[fb.phys, fb.phys + pitch*height)`, rounded to page
    boundaries) at `hhdmBase + fb.phys`, using 4 KiB pages only (never sharing a large page with
    real RAM), `PCD=1`/`PWT=0` (PAT index 2, UC- under the firmware's power-on `IA32_PAT`), NX,
    global -- the one HHDM mapping that isn't RAM (D-068). If the range overlaps an existing
    mapping or falls outside the 64 TiB window, the loader leaves `fb` all-zero instead. M2.3
    reprograms the PAT and remaps this range WC once the kernel owns its own page tables.
- **Control registers:** `EFER.NXE=1`, `CR0.WP=1`, `CR4.PAE|PGE`. Interrupts are disabled.
- **Registers:** `rdi` holds the BootInfo virtual address (in the HHDM). `rsp` points to a
  64 KiB boot stack that is marked `LOADER_RECLAIM`. The kernel switches to its own stack
  before reclaiming it.
- **GDT/IDT:** the firmware's own, left in place (now unmapped under the loader's page tables).
  The kernel installs its own first thing.

### 5.5 UEFI loader flow (D-068)
1. Get the LoadedImage and SimpleFileSystem protocols, then read and parse `/bong/boot.cfg`
   (§5.2).
2. Pick the GOP mode using the *global* `resolution` and set it (see "GOP mode selection"
   below) -- before the menu, since the menu needs a framebuffer. `EnableCursor(FALSE)` first.
   After this point the loader never calls `ConOut` again (GraphicsConsole doesn't know the mode
   changed and would blit at stale geometry); errors go to raw COM1 only (D-071: the
   framebuffer text renderer exists post-GOP, but no error path draws to it in M1.4 -- every
   failure after this point already has a serial diagnostic, and a boot that can't reach this far
   has no menu to show one on either way).
3. Show the menu if `timeout > 0` (arrow keys/Enter/1-9 via `ConIn` only -- never poll COM1
   receive before `ExitBootServices`, since OVMF's TerminalDxe owns it). Resolve the chosen (or
   default/timed-out) entry.
4. If the entry's resolved `resolution` differs from the global one, pick and set the GOP mode
   again.
5. Load `kernel.elf` (check ELF64, x86_64, `PT_LOAD` segments) into `EfiLoaderData` pages.
   Apply the KASLR slide using the `--emit-relocs` relocations (`R_X86_64_64`, `R_X86_64_32S`).
6. Load the initrd (once M5.5 wires it in; M1.4 parses `initrd =` but doesn't load it).
7. Find the RSDP in the config tables (ACPI 2.0 GUID first, then 1.0).
8. Gather the random seed: `EFI_RNG_PROTOCOL` if present, else RDSEED/RDRAND, plus TSC jitter.
9. Build the page tables (including the framebuffer mapping, §5.4) and BootInfo.
10. Call `GetMemoryMap` then `ExitBootServices`. Retry on a map-key mismatch, with no
    allocations between the two calls.
11. Convert the memory map, load CR3, and jump.

**GOP mode selection:** use the GOP on `ConsoleOutHandle`, falling back to the first
`LocateHandleBuffer(ByProtocol, GOP)` result with a linear framebuffer (`FrameBufferBase != 0`,
not `PixelBltOnly`). Query every mode; accept 32-bit-pixel formats only (RGBX, BGRX, or a
BitMask whose R/G/B fields are each non-zero, contiguous, and together span bits 24-31). `auto`
picks the largest `width*height` with width <=3840 and height <=2160 (ties: wider, then lower
mode number); `WIDTHxHEIGHT` picks an exact match or falls back to `auto` with a log line. Set
the mode only if it differs from the current one, then re-read `Mode->Info` and
`FrameBufferBase` (both can change on `SetMode`).

### 5.6 BIOS loader flow
- **stage1 (MBR):**
  1. Relocate to 0x0600.
  2. Read stage2 using INT 13h AH=42h (LBA extensions), from the location mkimage patched in.
  3. Jump to it.
- **stage2:**
  1. Enable A20: fast A20 port, INT 15h AX=2401, then the 8042 as a fallback.
  2. Get the E820 memory map.
  3. Pick a VBE mode (INT 10h 4F00/4F01/4F02) with a linear framebuffer, using the same rule
     as UEFI.
  4. Find the RSDP (EBDA first KiB, then 0xE0000 to 0xFFFFF).
  5. Enter 32-bit protected mode. Use real-mode thunks for INT 13h reads to parse GPT, find
     the ESP, and read FAT32 files (shared `boot/common` readers).
  6. Load the kernel and initrd above 1 MiB.
  7. Build the page tables and BootInfo (shared code).
  8. Enter long mode and jump.

### 5.7 Secure Boot (late milestone)
- `BOOTX64.EFI` is Authenticode-signed on the host with `sbsign`, using the user's own db key
  that they enrolled in the ASUS firmware.
- The loader contains an Ed25519 public key and verifies the detached signatures `kernel.elf.sig`
  and `initrd.img.sig`. The kernel verifies a signature on every `*.kmod`.
- Unsigned or tampered files cause the boot to be refused, with a clear message.

---

## 6. Memory management

### 6.1 Virtual address layout
| Range | Use |
|---|---|
| `0x0000000000000000`-`0x000000000000FFFF` | never mapped (null guard) |
| `0x0000000000010000`-`0x00007FFFFFFFFFFF` | user space (47-bit) |
| `0xFFFF800000000000`-`0xFFFFBFFFFFFFFFFF` | direct map of physical memory (HHDM) |
| `0xFFFFC00000000000`-`0xFFFFDFFFFFFFFFFF` | kernel virtual area: vmalloc, thread stacks (with guard pages), MMIO, fixmaps |
| `0xFFFFE00000000000`-`0xFFFFEFFFFFFFFFFF` | `Page` metadata array (one entry per physical frame) |
| `0xFFFFFFFF80000000`-`0xFFFFFFFF9FFFFFFF` | kernel image (KASLR slides it within this 512 MiB window, 2 MiB aligned) |
| `0xFFFFFFFFA0000000`-`0xFFFFFFFFEFFFFFFF` | loadable modules (within ±2 GiB of the kernel for `-mcmodel=kernel`) |

The PML4 entries 256-511 (the kernel half) are allocated at boot and shared by every address
space, so kernel mappings never need to be synced between them. M2.3 (D-086) allocates all 256 of
them eagerly, from the pmm, as part of building the kernel's own PML4, and never writes any of
them again afterward -- every later address space (M4+) and the SMP AP trampoline PML4 (M3.5)
copies these 256 entries by value rather than syncing individual mappings into them.

### 6.2 Physical memory
1. **`Page` array (M2.2, D-079):** one 64-byte `Page` per managed physical frame (state/order/
   flags/refcount/mapcount/an intrusive free-list `ListNode`/the owning `VmObject`+offset once M4
   exists/a private word M2.4's slab allocator uses), at the §6.1 metadata VA region, indexed by
   pfn with plain integer arithmetic. Backing is **sparse**: only pfn ranges the BootInfo map
   reports as USABLE/LOADER_RECLAIM/KERNEL/INITRD/ACPI_RECLAIM (clipped to the HHDM window) get
   Page entries, widened to 1024-frame (order-10) envelopes and merged -- a free block is always
   naturally aligned and at most order 10, so a buddy merge never needs a separate validity check.
   A zero-filled entry is `PAGE_STATE_RESERVED` by construction.
2. **Early (M2.2, D-080):** a bump allocator over the BootInfo USABLE ranges (top-down, highest
   range first), used only to back the Page array and its own page tables before the buddy
   allocator exists; sealed once `pmmInit()` finishes. USABLE memory below 1 MiB is withheld from
   the buddy allocator entirely (reserved for the M3.5 SMP trampoline / BIOS-area safety).
3. **Buddy allocator (M2.2, D-081):** orders 0-10 (4 KiB to 4 MiB), in zones `DMA32` (below 4 GiB,
   fixed by address) and `NORMAL`, with block state living entirely in the Page array (no separate
   bitmap). `pmmAddFreeRange()` is the only way memory enters a zone -- used for BootInfo's USABLE
   ranges at boot, and reused as-is for `LOADER_RECLAIM` (M2.3), `ACPI_RECLAIM` (M3.1), and
   `INITRD` (M5.5). A BSP-only per-CPU page cache (order 0 only, one free list per zone) fronts
   every order-0 request through a single accessor M3.5 replaces with real per-CPU state.
4. **Misuse detection (M2.2, D-082):** `pmmFreePages()` validates every call against the Page
   state machine *before* mutating anything -- a double free, a wrong order, or freeing an
   interior/reserved page panics via `panicBug()` (always on, not just debug builds). `KERNEL_DEBUG`
   builds (§3) additionally poison a freed block's content and verify it on the next allocation,
   catching a write-after-free.
5. **Slab allocator, kmalloc, vmalloc (M2.4, D-092..D-098):** named object caches
   (`slabCacheCreate`/`slabAlloc`/`slabFree`, optional constructor/destructor) with one magazine
   per cache (today: BSP-only, same honest single-CPU pattern as the pmm's own page cache; M3.5
   makes it real per-CPU state). An out-of-band `bufctl` free list sits after each slab's header,
   never inside the objects themselves, so constructed state and write-after-free poison never
   fight over the same bytes. `kmalloc`/`kfree` are 12 fixed size classes, 16 to 8192 bytes,
   16-byte aligned; a custom cache's own `align` is honored exactly. Ownership of a slab's pages
   is tracked in `Page.flags` (`PAGE_F_SLAB`, orthogonal to `PageState`) with `Page.privateWord`
   pointing at the owning `Slab`; the pmm refuses to free a page still carrying an owner flag
   (`PMM_BUG_OWNED_PAGE`). Anything over 8192 bytes goes to `vmalloc`: eager, page-granular,
   built on `vmmMapKernel`/the KVA allocator (guard pages on both sides come from `vmmKvaAlloc`
   itself), tracked the same way via `PAGE_F_VMALLOC`. `KERNEL_DEBUG` builds add fixed redzones
   around every slab object (checked on free) and poison freed payloads (checked on the next
   reuse, D-082's exact pmm pattern) -- both reported through the existing `panicBug()`/
   `TRAP_CATCH_KERNEL_BUG` mechanism, no new catch kind. Neither `kmalloc` nor `vmalloc` is ever
   called by the pmm or vmm themselves (no recursion). See `docs/logs/M2.4.md` for the design
   writeup.
6. **Kernel stacks:** 16 KiB plus a guard page, in the kernel virtual area.
7. **Reclaim:** `LOADER_RECLAIM` is handed to the buddy allocator once the kernel switches to its
   own page tables and no longer needs the loader's (M2.3, D-083/D-089 -- superseding this
   section's earlier "after switching stacks" wording, which predates the kernel having its own
   page tables at all), via `pmmReclaimLoaderMemory()`, which keeps the one BootInfo page reserved
   until M2.6; `ACPI_RECLAIM` once ACPI tables are parsed (M3.1); `INITRD` once it's no longer
   needed (M5.5).

### 6.3 Paging
- 4-level paging. Kernel mappings are marked global.
- **Permissions:** kernel text is R-X, rodata is R--, and data, bss, and the direct map are
  RW-/NX. No mapping is ever both writable and executable, in the kernel or in userspace. M2.3's
  `archPagingVerifyWx()` (D-090) walks the live kernel PML4 and panics on any violation
  (including a writable HHDM alias of an executable frame); it logs `vmm: W^X verified: ...` on
  success, and runs once at boot plus again from a ktest.
- **PAT (M2.3, D-087):** `IA32_PAT` = `0x0007010600070106` -- index 0/4 WB (the power-on
  default, unchanged), index 1/5 **WC** (was WT; used for framebuffers), index 2/6 UC- (unchanged
  -- the loader's own pre-M2.3 framebuffer fallback, D-068/D-071, relies on this), index 3/7 UC
  (for MMIO, once anything maps it that way). Programmed via the SDM Vol 3A §11.11.8/§11.12.4
  MP-safe procedure (cache-disable, WBINVD, flush, WRMSR, WBINVD, flush, restore), read back and
  checked. No mapping ever sets the PTE's PAT bit; only PWT selects WC.
- **Kernel page tables (M2.3, D-086):** built entirely from `pmmAllocPages()` (order 0, zeroed)
  after `pmmInit()`, on top of the still-active loader tables, then activated by toggling
  CR4.PGE off, loading CR3, and toggling CR4.PGE back on (flushes every TLB/paging-structure-
  cache entry both times, so no stale loader-global entry survives the switch). The Page-array
  PML4 subtree (slots 448-479) is adopted from the loader's tables by value, not rebuilt.
- **CPU protections:** SMEP, SMAP, and UMIP are on when supported (M2.3, `archCpuEnableProtections()`,
  checked against CPUID). The kernel touches user
  memory only through `copyFromUser`, `copyToUser`, and `copyStringFromUser` (STAC/CLAC plus
  an exception fixup table).
- **PCID:** enabled when present (a later milestone). Without it, switching CR3 does a full flush.
- **TLB shootdown:** by IPI, with a batched list of invalidations. Kernel threads use lazy TLB.
  M2.3's `vmmMapKernel`/`vmmUnmapKernel` (D-088) invalidate only the local CPU (`archTlbInvalidate
  KernelRange()`, one INVLPG per page) -- M3.5 replaces that one function's body with a real
  shootdown once SMP exists, nothing else in `kernel/mm/vmm.c` changes.
- **Kernel virtual area (M2.3, D-088):** `vmmMapKernel`/`vmmUnmapKernel`/`vmmLookupKernel`
  (`kernel/include/vmm.h`) hand out 4 KiB RW/RO, WB/WC mappings within `[VM_KVA_BASE,
  VM_KVA_END)` (§6.1), backed by a pure, host-tested first-fit extent allocator
  (`kernel/mm/kva.c`) that reserves an unmapped guard page on each side of every allocation.
  `VMM_EXEC` is rejected until a module loader needs it. A WC request over a physical page the
  pmm already manages (and so already maps WB via the HHDM) is rejected too (SDM Vol 3A §11.12.4:
  one physical page can't have two memory types at once). The extent allocator admits at most
  `KVA_MAX_EXTENTS-1` (511) concurrent reservations (M2.4, D-098): since removing `k` disjoint
  ranges from one bounded interval can never leave more than `k+1` free pieces, this makes the
  fixed-size free-extent table provably always big enough, so a legitimate `vmmKvaFree` can never
  hit the table-full case D-091(7) originally flagged.

### 6.4 Address spaces and VM objects
- **`AddressSpace`:** the PML4, a red-black tree of `VmRegion`s keyed by start address, a
  mutex, and usage stats.
- **`VmRegion`:** `[start, end)`, protection, flags (`PRIVATE` or `SHARED`), a `VmObject*`,
  and an offset.
- **`VmObject` kinds:**
  - `ANON`: zero-fill, copy-on-write capable
  - `FILE`: backed by a vnode's page cache
  - `PHYS`: MMIO, framebuffer, and DMA buffers
  - `SHARED`: explicitly created shareable memory, passed around as a handle
- **Page fault path:**
  1. Look up the region. A missing region or a protection violation is an exception for the
     process.
  2. Otherwise resolve the page: zero-fill, a page-cache hit, a file read, a copy-on-write copy
     (on a write fault when refcount > 1), or a swap-in.
  3. Map it.
- **Copy-on-write:** cloning an address space marks the private writable pages read-only in
  both, and bumps their refcounts.
- **Memory-mapped files:** page-cache pages are mapped directly. A flusher thread writes
  dirty shared pages back.

### 6.5 Swap
- **Storage:** a swap partition (by type GUID) or a swapfile. A swap map tracks per-slot
  refcounts.
- **Reclaim:** a clock-style scan over active and inactive lists, driven by min/low/high
  watermarks and done by a reclaim thread (`kswapd`).
  - Anonymous pages go to swap.
  - Clean file pages are dropped.
  - Dirty file pages are written back first.

### 6.6 Randomization
- **KASLR:** the kernel slide is chosen by the loader (§5.5). Randomizing the HHDM base and
  the vmalloc base comes later.
- **User ASLR:** the PIE base, mmap base, stack, and heap are randomized, with at least 28
  bits of entropy for mmap.
- **Kernel RNG:** an entropy pool fed by RDSEED/RDRAND, `BootInfo.randomSeed`, and interrupt
  timing, feeding a ChaCha20-based CSPRNG (`randomGetBytes`), from M2.6 onward.
- **Stack canaries (M2.1, D-077):** `__stack_chk_guard` is set exactly once, very early in
  `kernelMain`, from a splitmix64-style fold of all 8 qwords of `BootInfo.randomSeed` mixed with
  one `rdtsc` reading (the CSPRNG doesn't exist yet at this point) -- never reseeded afterward.
  The reseed function is both `noinline` and `no_stack_protector`, called from a
  `no_stack_protector` `kernelMain`, so the store itself is never inside a canary-checked frame.

---

## 7. CPU, SMP, interrupts, time

### 7.1 Per-CPU data and CPU setup
- **BSP-only through M3.4 (D-072):** SMP bring-up is M3.5. From M2.1 through M3.4 there is one
  static `ArchCpuTables` (GDT+TSS) built and loaded for the BSP only; the `CpuLocal`/per-CPU
  design below is the target M3.5 moves to, not what exists yet. `gdtBuild`/`tssBuild` already
  take an explicit struct pointer so that move needs no rewrite.
- **Per-CPU data:** each CPU's GS base points to a `CpuLocal` struct. It holds `self`,
  `cpuId`, `apicId`, `currentThread`, `idleThread`, `runQueue`, `preemptCount`, `irqDepth`,
  the TSS, the GDT, scratch space for syscall entry, and stats. The kernel uses `swapgs` on
  entry from user mode.
- **CPU features:** CPUID results are stored in a feature bitset. If a required feature is
  missing, boot panics with a clear message naming it.
- **FPU/SIMD:** saved and restored eagerly on context switch, with XSAVE/XSAVEOPT (or FXSAVE
  as a fallback). Kernel code never uses FP or SIMD, except inside explicit
  `fpuBegin()`/`fpuEnd()` sections (AES-NI, fast memcpy later).
- **GDT (one per CPU):** null (0x00), kernel code (0x08), kernel data (0x10), user code32
  (0x18, placeholder for the STAR layout -- **not present**, since SYSRET never reads this
  descriptor and a present compat-mode descriptor is needless attack surface before userspace
  exists), user data (0x20), user code64 (0x28), then the TSS (0x30, 16 bytes). All descriptors
  have the Accessed bit preset (D-062). `GDT_USER_CS64_RPL3`=0x2B/`GDT_USER_DS_RPL3`=0x23 are
  reserved for STAR once syscalls exist.
- **TSS:** RSP0=0 until per-thread kernel stacks exist (M4/M5) -- a stray privilege transition
  before then #PFs near-null instead of corrupting memory silently. No I/O permission bitmap
  (`iomapBase` = TSS limit + 1), so all ring-3 port I/O is denied by construction.
- **IDT:** shared by all CPUs, all 256 entries populated (D-074) so a stray vector gets a
  diagnosable #GP/#DF chain instead of a triple fault off a not-present gate. Each CPU gets three
  16 KiB IST stacks, each behind its own unmapped guard page (D-073, superseding D-061's
  "exactly four `PT_LOAD`s"): IST1 for #DF, IST2 for NMI, IST3 for #MC. Every other vector uses
  the normal kernel stack. IF stays 0 until the first IRQ source is wired up (M3.2).

### 7.2 Interrupt vectors
All 256 IDT gates are populated (interrupt gates, DPL0 except vector 3's DPL3); an unregistered
vector still reports a diagnosable panic rather than triple-faulting. #BP (int3) resumes normally
(a trap, not a fault: the saved RIP already points past the `int3` byte); every other exception
panics unless a ktest has armed `archTrapCatch()` for it (§23) -- NMI/#DF/#MC can never be caught
this way and always panic, since M2.1 has no legitimate source for any of them and CR4.MCE isn't
set until M3.6.

| Vectors | Use |
|---|---|
| 0-31 | CPU exceptions |
| 32-47 | legacy 8259 (remapped, then fully masked) |
| 48-239 | device IRQs, allocated dynamically (IOAPIC and MSI/MSI-X) |
| 0xF0 | TLB shootdown IPI |
| 0xF1 | reschedule IPI |
| 0xF2 | call-function IPI |
| 0xF3 | stop / panic IPI |
| 0xFE | LAPIC timer |
| 0xFF | spurious |

### 7.3 Interrupt controllers
- **Local APIC:** x2APIC (MSR interface) when supported, otherwise xAPIC over MMIO.
- **IOAPIC:** configured from the MADT, including interrupt source overrides and
  polarity/trigger flags.
- **PCIe devices:** use **MSI/MSI-X** by default. Legacy INTx goes through `_PRT` routing.
- **API:** `irqAllocVector`, `irqRegister(vector, handler, ctx)`, and MSI programming helpers.
  Device IRQs are spread across CPUs.

### 7.4 SMP bring-up (multi-core from day one)
1. The MADT lists the CPUs (Local APIC and x2APIC entries, respecting the enabled and
   online-capable flags).
2. A trampoline page below 1 MiB is reserved early.
3. For each AP, send INIT, then SIPI twice, with the delays from the Intel SDM's MP
   initialization protocol.
4. The AP goes from real mode to protected mode to long mode, loads the kernel CR3, GDT, and
   IDT, sets up its per-CPU state, and enters the scheduler idle loop.
5. The BSP waits for each AP with a timeout. An AP that fails is logged, and boot continues
   without it.

`cpus=N` on the command line limits how many CPUs are brought up.

**IPIs:** reschedule, TLB shootdown, call-function (with completion), and stop.

### 7.5 Time
- **Clocksource: the TSC.** It must be invariant on real hardware. It's calibrated at boot
  against the **ACPI PM timer** (from the FADT). The HPET is optional and never required,
  since some AM5 boards disable it. Cross-CPU TSC sync is checked, and per-CPU offsets are
  used if needed.
- **Clock events:** the LAPIC timer in one-shot mode, or TSC-deadline mode when CPUID
  advertises it. Calibrated against the TSC.
- **Timers:** a high-resolution timer queue per CPU (a min-heap). Tickless idle comes later.
- **Wall clock:** read from the CMOS RTC at boot (UTC). Later corrected by SNTP in `netd`.
- **APIs:** `timeMonotonicNs()`, `timeWallNs()`, and timer objects.

### 7.6 Locking and preemption
- **Primitives:**
  - ticket `Spinlock`, with irqsave variants
  - `Mutex` (sleeping, has an owner; priority inheritance comes later for realtime)
  - `RwLock`
  - `Semaphore`
  - `WaitQueue`
  - `Completion`
  - C11 atomics
- **Lock validator (debug builds):** tracks lock classes and the graph of acquisition order.
  It reports order inversions, recursive locking, sleeping while atomic, and IRQ-unsafe
  locking. It panics in tests.
- **Preemptible kernel:** each CPU has a `preemptCount`, and holding a spinlock disables
  preemption. Preemption points are on IRQ return and in `preemptEnable()` when
  `needResched` is set.

---

## 8. Scheduler
Each CPU has its own run queue with three classes, checked in strict priority order:

| Class | Policy |
|---|---|
| **Realtime** | 100 priorities (0 highest), `FIFO` or `RR` (10 ms quantum), a bitmap of queues. Used by audio, input, and the compositor's input thread |
| **Normal** | fair scheduling: per-thread `vruntime`, a red-black tree per run queue, weights from nice -20..+19 (`weight ≈ 1024 / 1.25^nice`), 6 ms target latency, 1 ms minimum granularity, wakeup preemption |
| **Background** | runs only when both other classes are empty; round-robin with a 20 ms slice |

- **Load balancing:** an idle CPU steals work from the busiest CPU (pull). Busy CPUs also
  balance periodically. CPU affinity masks are respected.
- **Unit of scheduling:** the thread. A process is an address space, a handle table,
  credentials, a namespace, and a set of threads.
- **CPU accounting:** per-thread user and system time, used by `sysmon` and `ps`.
- **Later:** priority inheritance on `Mutex`, tickless idle, and job CPU quotas.

---

## 9. Kernel objects, handles, and capabilities

Everything a program can touch is a **kernel object**, reached only through a **handle**.
There is **no ambient authority**: a process can't open a file by absolute path, and path
lookups are always relative to a directory handle it holds.

- **Object header:** type, an atomic refcount, a `koid` (unique 64-bit id), the current
  signal bitmask plus a list of observers, and an optional debug name.
- **Handles:** 32-bit values. Bits 0-23 are the table index, bits 24-30 are a generation
  counter, and bit 31 is always 0. The value 0 is always invalid.
- **Handle table:** one per process, growable, with its own lock. Each handle carries a
  **rights** bitmask.
- **Rights:** `READ`, `WRITE`, `EXECUTE`, `MAP`, `DUPLICATE`, `TRANSFER`, `WAIT`, `SIGNAL`,
  `MANAGE`, `INSPECT`, `ENUMERATE` (directories), `CREATE` (directories).
- **The rule:** `handleDuplicate(h, rights)` can only keep or drop rights, never add them.
  Sending a handle through a channel *moves* it, and requires `TRANSFER`.

| Object | Purpose | Main signals |
|---|---|---|
| Process | address space + handles + credentials + namespace | `TERMINATED` |
| Thread | schedulable entity | `TERMINATED`, `SUSPENDED` |
| Job | group of processes; resource limits; sandbox policy; kill-tree | `NO_PROCESSES` |
| VmObject | memory (anon/file/phys/shared) | — |
| Channel | message IPC endpoint (bytes + handles) | `READABLE`, `WRITABLE`, `PEER_CLOSED` |
| Port | wait set / event queue | `PACKET_AVAILABLE` |
| Event, EventPair | user-signalable flags | `SIGNALED` |
| Timer | one-shot / periodic | `SIGNALED` |
| File, Directory | open VFS nodes | `READABLE`, `WRITABLE` |
| Namespace | a process's view of the mount tree | — |
| Socket | network endpoint | `READABLE`, `WRITABLE`, `CONNECTED`, `PEER_CLOSED` |
| Pipe | byte stream | `READABLE`, `WRITABLE`, `PEER_CLOSED` |
| Pty | pseudo-terminal master/slave | `READABLE`, `WRITABLE`, `PEER_CLOSED` |
| Resource | rights to hardware ranges (only devmgr holds the root) | — |
| Interrupt | device IRQ delivered to a userspace driver | `SIGNALED` |
| BusTransaction | DMA pinning through the IOMMU | — |
| Display, InputDevice | framebuffer / input for compositor | `VSYNC`, `READABLE` |
| AudioStream | PCM stream to the sound hardware | `WRITABLE` |

---

## 10. Native system call ABI

- **Calling convention:** the `syscall` instruction. The number goes in `rax`; arguments go in
  `rdi, rsi, rdx, r10, r8, r9`; the result comes back in `rax` (≥ 0 means success or a
  value, < 0 is a `Status` error). `rcx` and `r11` are clobbered, and every other register is
  preserved.
- **One list defines the syscalls:** `kernel/include/uapi/syscalls.def`, an X-macro list of
  `SYSCALL(number, name, argCount)`. It generates both the kernel dispatch table and the
  libc stubs.
- **Personality:** each process has one (`NATIVE`, or `LINUX` later), which selects the
  dispatch table.
- **Validation:** every user pointer is checked and copied. Strings have a maximum length.
  Handles are checked for type and rights before use.
- **vDSO page (later):** a read-only page mapped into every process, holding time data under
  a seqlock so `timeMonotonicNs` needs no syscall.

**Syscalls (v1 names; numbers get assigned in `syscalls.def`):**
- **Handles and objects:** `handleClose`, `handleDuplicate`, `handleReplace`,
  `objectWaitOne`, `objectSignal`, `objectGetInfo`, `objectSetProperty`
- **Ports:** `portCreate`, `portWait`, `portQueue`, `portBind`
- **Processes and threads:** `processCreate`, `processStart`, `processExit`,
  `processClone` (address-space clone, used for fork), `threadCreate`, `threadStart`,
  `threadExit`, `threadYield`, `threadSleep`, `threadSetPriority`, `jobCreate`, `jobSetPolicy`
- **Memory:** `vmObjectCreate`, `vmMap`, `vmUnmap`, `vmProtect`, `vmObjectRead`,
  `vmObjectWrite`, `futexWait`, `futexWake`
- **IPC:** `channelCreate`, `channelWrite`, `channelRead`, `channelCall`, `eventCreate`,
  `timerCreate`, `timerSet`, `pipeCreate`, `ptyCreate`
- **Files:** `fsOpen` (relative to a directory handle), `fsRead`, `fsWrite`, `fsSeek`,
  `fsStat`, `fsReadDir`, `fsMkdir`, `fsUnlink`, `fsRename`, `fsLink`, `fsSymlink`,
  `fsReadLink`, `fsTruncate`, `fsSync`, `fsChmod`, `fsChown`, `fsMount`, `fsUnmount`,
  `namespaceCreate`, `namespaceBind`
- **Sockets:** `socketCreate`, `socketBind`, `socketListen`, `socketAccept`,
  `socketConnect`, `socketSend`, `socketRecv`, `socketSetOption`, `socketGetOption`,
  `socketShutdown`
- **Time and system:** `clockGet`, `randomGet`, `logWrite`, `systemInfo`,
  `systemPowerOff`, `systemReboot`
- **Credentials:** `credGet`, `credSet` (privileged)
- **Drivers:** `resourceCreate`, `interruptCreate`, `interruptWait`, `interruptAck`,
  `busTransactionCreate`, `ioPortGrant`

**Process startup:** the new process gets one **bootstrap channel**. Its first message holds
the args, the environment, and handles for stdin, stdout, stderr, the root directory, the cwd,
the namespace, and the job. crt0 unpacks this. The POSIX layer turns it into
`argc`/`argv`/`environ` and file descriptors.

---

## 11. IPC and userspace drivers

- **Channels:**
  - A message is up to 64 KiB of bytes plus up to 64 handles.
  - Each endpoint's queue has a limit; when it's full, `channelWrite` returns
    `STATUS_SHOULD_WAIT`.
  - `channelCall` is a write followed by a wait for the matching `txid` reply. A later
    optimization switches directly to the server thread.
- **Message format:** protocols are hand-written C structs in `libs/proto/`, each starting
  with a common header `{ uint32_t type; uint32_t txid; uint32_t size; uint32_t flags; }`.
  No IDL compiler.
- **Ports:** objects register observers that deliver packets to a port. `portWait` returns
  those packets. User packets go in with `portQueue`.
- **Userspace drivers:**
  - `devmgr` holds the root `Resource`. It hands each driver process exactly its device's
    resources: MMIO `VmObject`s (mapped UC), an `Interrupt` object, I/O port grants, and
    `BusTransaction`s for DMA.
  - **The IOMMU (AMD-Vi)** gives each driver process its own DMA domain, so a buggy driver
    can only reach buffers it pinned. QEMU tests run with `-device amd-iommu`.
  - When a driver crashes, its handles close, the kernel tears down its IOMMU domain and
    masks its IRQs, and `svcd` restarts it with backoff.

---

## 12. Loadable kernel modules

- **Format:** a relocatable ELF (`*.kmod`) built with the kernel's flags.
- **`.modinfo` section:** name, version, `abiVersion`, license, dependencies, and a PCI/USB
  match table.
- **Exports:** the kernel exports symbols with `EXPORT_SYMBOL(fn)`, which emits a `.ksymtab`
  entry. Modules can only link against exported symbols.
- **ABI check:** `abiVersion` must equal the kernel's `KERNEL_MODULE_ABI` exactly, or the
  module is refused. Any change to an exported symbol or struct bumps it.
- **Loading:**
  1. Map the module into the module area and apply `R_X86_64_64`, `R_X86_64_PC32`,
     `R_X86_64_PLT32`, and `R_X86_64_32S` relocations.
  2. Resolve its symbols and dependencies.
  3. Set page protections (text R-X, data RW- NX).
  4. Call `moduleInit()`.
- **Unloading:** refcounted. Calls `moduleExit()`.
- **Discovery:** the kernel's driver registry matches devices against `modules.idx` (generated
  at build time) and loads modules from the initrd, then from `/lib/modules` once root is
  mounted.
- **Signatures:** required only once Secure Boot is enabled (a late milestone).

---

## 13. Devices and drivers

**Device model:** bus drivers (PCI/PCIe via the ACPI MCFG table and ECAM, ACPI, and USB)
enumerate `Device`s. Drivers register match tables. The device manager binds devices to
drivers, whether built-in, a module, or a userspace driver.

| Driver | Where | Target | Notes |
|---|---|---|---|
| 16550 serial | built-in | both | early debug log, `console=serial` |
| Framebuffer console (fbcon) | built-in | both | GOP/VBE, WC-mapped, used until the compositor takes over |
| PCI/PCIe | built-in | both | ECAM, BAR sizing, MSI/MSI-X |
| ACPI core + AML | built-in | both | §14 |
| LAPIC, IOAPIC, PM timer, HPET, RTC | built-in (arch) | both | |
| AMD-Vi IOMMU | built-in (arch) | both | QEMU `amd-iommu`, Zen 4 |
| PS/2 keyboard/mouse (i8042) | module | QEMU (+ board if it has a port) | early input before USB |
| NVMe | module | both | admin + one I/O queue pair per CPU, MSI-X |
| AHCI (SATA) | module | QEMU + any SATA drive | |
| virtio-blk, virtio-net, virtio-gpu (2D), virtio-input | modules | QEMU | modern virtio-pci |
| e1000 | module | QEMU | first NIC driver |
| RTL8125 | module | real | confirm chip with `lspci -nn` |
| xHCI | module | both | USB 2/3 host controller |
| USB hub | module | both | |
| USB mass storage (BOT) | module | both | UAS later |
| USB HID (keyboard, mouse) | **userspace driver** | both | the showcase for crash-restartable drivers |
| HD Audio + generic codec parser | module | both | Realtek codec + NVIDIA HDMI audio |
| AMD iGPU display (DCN) | module | real | **stretch** |
| Wi-Fi, Bluetooth | — | — | **later list** |

**Power:** shutdown through ACPI S5. Reboot through the FADT reset register, then the 8042,
then a triple fault. No sleep states in v1.

---

## 14. ACPI (fully custom)

**Phase 1 (early):**
- Tables: find the RSDP, walk the XSDT (RSDT as fallback), and validate checksums.
- Parse the FADT, MADT, MCFG, HPET, and IVRS (for the IOMMU).
- Load the DSDT and SSDTs.
- A **minimal AML evaluator**, enough to evaluate `\_S5_` for shutdown.
- The SCI and the power-button fixed event, which trigger an orderly shutdown through `svcd`.

**Phase 2 (full interpreter):**
- The complete ACPI 6.x AML opcode set, the namespace, methods with locals and args, and
  `Mutex`/`Event`.
- Operation regions: SystemMemory, SystemIO, and PCI_Config.
- `Notify`, `_STA`/`_INI` device init, `_PRT` for INTx routing, and GPEs.
- `_OSI` answers as recent Windows, because firmware tailors its behavior to it.

**Testing:** the interpreter builds host-side too, so it can run against QEMU's DSDT and
against tables dumped from the reference PC (`acpidump` from a Linux live USB, stored in
`tests/data/acpi/`).

---

## 15. Storage and filesystems

- **Block layer:** a `BlockDevice` exposes sector size, sector count, async `submitIo`
  (scatter-gather), and `flush`. It includes a GPT and MBR partition scanner, and NVMe gets
  one queue per CPU.
- **Page cache:** a radix tree of `Page`s per vnode, with readahead, a writeback thread, and
  `fsync`. It's unified with VM: mmap maps these same pages.
- **VFS:**
  - Core types: `Vnode`, a dentry cache (including negative entries), `Mount`, and
    `FileSystemType` ops.
  - Path lookup is relative to a directory handle and can't escape the namespace root with
    `..`. Symlinks resolve with a depth limit.
  - Permission checks: Unix uid/gid/mode first, then handle rights.
- **Namespaces:** a `Namespace` is a set of mounts (with bind mounts). This is how the sandbox
  works: an app sees only what its namespace contains.
- **Synthetic filesystems:** devfs (`/dev`), procfs (`/proc`, text files for tools and the
  Linux layer), tmpfs (`/tmp`), and initrd (cpio newc, read into tmpfs).

| Filesystem | Support | Verified by |
|---|---|---|
| FAT32 (+LFN) | read/write | `mkfs.fat` / `fsck.fat` on host |
| **bongfs** (native) | read/write, journaled | our `mkfs.bongfs` / `fsck.bongfs` |
| ISO9660 (+Joliet, Rock Ridge) | read | `xorriso` images |
| exFAT | read/write | `mkfs.exfat` / `fsck.exfat` |
| ext4 | read, then write behind `ext4.write=1` | `mkfs.ext4` / `e2fsck -fn` |
| NTFS | read-only | `mkntfs` + `ntfs-3g` images |

### 15.1 bongfs v1 (the full spec gets written in `docs/specs/bongfs.md` before any code)
- **Basics:** 4 KiB blocks, little-endian, 64-bit block numbers.
- **Superblock:** in block 1, with backups in groups 1, 3, 5, 7, 9, 25, 27, 49, and so on.
- **Block groups:** 128 MiB each, with a descriptor, a block bitmap, an inode bitmap, and an
  inode table.
- **Inodes:** 256 bytes each. Hold mode, uid/gid, nanosecond timestamps, size, link count,
  and flags. The extent tree root sits inline in the inode, with a B+tree for large files.
  Extended attributes live inline, plus one overflow block.
- **Directories:** small directories are stored inline in the inode; larger ones use a hashed
  B+tree. File names are up to 255 bytes of UTF-8.
- **Journal:** metadata journaling in **ordered mode** (data is written before the metadata
  that points to it commits). A circular log with descriptor blocks and commit blocks, replayed
  on mount.
- **Checksums:** CRC32C on the superblock, group descriptors, inodes, directory blocks, extent
  tree blocks, and journal blocks.
- **Feature flags:** `compat`, `incompat`, and `roCompat`. Snapshots will be an `incompat`
  feature (stretch goal: refcounted extents with copy-on-write for snapshotted inodes).
- **Shared code:** the same `libs/bongfs` code runs in the kernel, `mkfs.bongfs`,
  `fsck.bongfs`, and `bongfs-cp`.

### 15.2 Full-disk encryption (late, optional at install)
- **Layer:** an encrypting block-device layer sits under bongfs, using **AES-256-XTS** (AES-NI
  on real hardware).
- **Header format:** custom. A random master key is wrapped by a key derived from the
  passphrase with **Argon2id**.
- **Unlock:** at boot, from an early-userspace prompt in the initrd.
- **Status:** labeled **EXPERIMENTAL** (§17).

---

## 16. Networking (in-kernel)

- **Link layer:** `NetDevice` (driver ops, MTU, MAC), with `NetBuf` packet buffers
  (scatter-gather, headroom). Receive work runs per device in a softirq-style worker thread.
- **Protocols:**
  - loopback
  - ARP
  - IPv4 (fragment reassembly, routing table)
  - ICMP (ping)
  - IPv6 (NDP, ICMPv6, SLAAC)
  - UDP
  - **TCP** following RFC 9293:
    - sliding windows and window scaling
    - timestamps
    - retransmit timeout per RFC 6298
    - fast retransmit and recovery
    - congestion control: NewReno first, CUBIC later
    - SACK later
- **Sockets:** `Socket` objects natively. BSD sockets come in the POSIX layer. Non-blocking
  sockets integrate with ports.
- **`netd` (userspace):** DHCPv4 client, SLAAC config, a DNS stub resolver (served over a
  channel, and exposed to POSIX `getaddrinfo`), SNTP time sync, and interface configuration.
- **Firewall:** an in-kernel packet filter using stateful connection tracking.
  - Default policy: allow outbound; allow inbound only for established connections, ICMP echo,
    and explicitly enabled services (SSH on 22 once `sshd` is enabled).
  - Rules are managed with the `fw` tool.
- **Planned services:** `sshd` (§17), and later per-app network permissions as part of the
  sandbox.

---

## 17. Crypto (custom, **EXPERIMENTAL**)

| Primitive | Used by | Test vectors |
|---|---|---|
| SHA-256, SHA-512 | everything | NIST CAVP / RFC 6234 |
| HMAC, HKDF | TLS, SSH, disk | RFC 4231, RFC 5869 |
| ChaCha20, Poly1305, ChaCha20-Poly1305 | TLS, SSH, CSPRNG | RFC 8439 |
| AES-128/256 (AES-NI + constant-time bitsliced fallback), AES-GCM, AES-XTS | TLS, disk | NIST CAVP, IEEE 1619 |
| X25519 | TLS, SSH | RFC 7748 |
| Ed25519 | SSH host keys, package + boot signatures | RFC 8032 |
| ECDSA P-256 verify, RSA-PSS / PKCS#1 v1.5 verify | TLS certificate chains | NIST CAVP, Wycheproof |
| Argon2id | passwords, disk keys | RFC 9106 |
| CRC32C | bongfs | known values |

**Rules:**
- No branches or table lookups that depend on secret data.
- Compare secrets with `cryptoEqual` (constant time).
- Zero secrets after use (`cryptoWipe`).
- Every primitive has host-side unit tests against the official vectors. The protocol layers
  also get **interop tests against real OpenSSH and OpenSSL/curl**.
- Docs and the `sshd`/`browser` about screens state that the crypto is experimental and
  unaudited.

**Protocol profiles:**
- **TLS 1.3 client only:** X25519 key exchange; AES-128-GCM, AES-256-GCM, and
  ChaCha20-Poly1305; X.509 chain validation against the Mozilla CA bundle; SNI.
- **SSH-2 server:**
  - key exchange: `curve25519-sha256`
  - host key: `ssh-ed25519`
  - ciphers: `chacha20-poly1305@openssh.com` and `aes256-gcm@openssh.com`
  - authentication: public key (`authorized_keys`) and password
  - channel types: sessions (PTY + shell) and exec

---

## 18. Userspace

- **libc (custom):** crt0, the native syscall stubs, the C17 standard library (stdio, stdlib,
  string, math, time, locale "C" plus UTF-8), and the **POSIX layer**:
  - file descriptors mapped onto handles
  - pthreads on top of futexes
  - `fork()` through `processClone` plus copy-on-write
  - signals on top of kernel exception and async delivery
  - termios on PTYs
  - BSD sockets
  - `dirent`, `mmap`, `poll`/`select`
  - `posix_spawn`
- **Linking:** static at first. A dynamic linker (`ld.so`, ELF PIC shared libraries) arrives
  before the big ports and before Linux compatibility.
- **Executables:** ELF64 PIE with a `.note.bongos` ABI note. The Linux layer identifies Linux
  binaries by the missing note or a `PT_INTERP` of `ld-linux`.
- **Filesystem layout (classic Unix):** `/bin`, `/sbin`, `/lib`, `/etc`, `/usr` (`bin`,
  `lib`, `include`, `share`), `/var` (`log`, `cache`, `lib/pkg`), `/tmp`, `/home`, `/root`,
  `/dev`, `/proc`, `/boot` (the ESP), and later `/compat/linux`.
- **`svcd` (PID 1, the service manager):**
  - Reads `/etc/svc/*.svc` (INI format: `exec`, `after`, `requires`, `restart = always |
    on-failure | never`, `user`, `sandbox`).
  - Starts services in dependency order. Restarts crashed ones with exponential backoff and
    rate limiting.
  - Gives each service its own Job.
  - Handles orderly shutdown.
  - Includes a control tool: `svc status|start|stop|restart|logs`.
- **`logd`:** kernel and service logs go into a ring buffer and `/var/log/*`. Read them with
  `svc logs` and `dmesg`.
- **Shells:**
  - **bsh** (native, early):
    - pipelines and redirection
    - variables, `if`/`for`/`while`, functions
    - job control once PTYs and signals exist
    - history, tab completion, colored prompt
  - **bash** gets ported later through the POSIX layer.
- **coreutils (custom):** `ls cat cp mv rm mkdir rmdir ln touch echo pwd head tail wc sort
  uniq grep find ps kill top df du mount umount chmod chown date uname whoami id env sleep
  clear hexdump dmesg reboot poweroff`.
- **Users:**
  - `/etc/passwd` and `/etc/group`, plus `/etc/shadow` holding **Argon2id** hashes.
  - Login happens through `getty` + `login` on the text console, and the graphical `greeter`
    on the desktop.
  - `elevate` is the sudo-style tool, configured in `/etc/elevate.conf` (the admin group).
- **Sandboxing:** GUI apps started from the desktop run in a Job with a sandbox namespace that
  holds:
  - the app's own directory
  - `~/.local/share/<app>`
  - `/tmp/<app>`
  - fonts and themes (read-only)
  - anything else, granted through **`permd`** (the permission portal). `permd` shows file
    pickers and "allow access to Documents?" prompts and hands over directory handles.
    Grants are stored per app.

  **Terminal tools are not sandboxed.**
- **Package manager `pkg`:**
  - Package format `.bpkg`: a tar archive, deflate-compressed with our own implementation,
    containing a manifest, plus an Ed25519 signature.
  - The repo index is signed too. It's hosted on GitHub Releases/Pages and fetched over our
    own TLS.
  - Commands: `pkg install|remove|update|search|info|verify`.
  - Packages are built by `ports/` recipes.
- **Self-hosting (late):** port LLVM (clang, lld), make, and nasm, then build bongOS on bongOS.

---

## 19. Graphics and GUI

- **Kernel side:**
  - `Display` objects: mode info, a mappable WC framebuffer, `setMode` (virtio-gpu), cursor
    plane when available, and a 60 Hz `VSYNC` signal from a timer.
  - `InputDevice` objects: keyboard (scancode to keycode) and pointer.
- **`compositor` (userspace):**
  - Clients create **surfaces** over a channel protocol and share pixel buffers as `VmObject`
    handles.
  - On commit, a client sends its damage rectangles. The compositor composites only the
    damaged regions into a back buffer, then blits to the framebuffer.
  - Frames are paced to 60 Hz. Input handling runs on a realtime-class thread, so the cursor
    never lags behind rendering.
- **Window management:** built into the compositor. Floating windows by default, and a
  **tiling mode** that can be toggled in Settings or with Super+T.
- **`desktop` (the shell UI):**
  - a bottom **panel** holding the app launcher, the task manager, the system tray (network,
    volume, clock), and notifications
  - the lock screen
  - wallpaper
- **Theme ("dark, Plasma-like" but original):**
  - a cool blue-gray dark palette with a blue accent
  - flat widgets with slightly rounded corners and subtle shadows
  - original icons, wallpaper, and window decorations (no KDE assets)
  - all colors and sizes come from theme tokens in `data/themes/dark.theme`
- **Toolkit `libs/ui`:**
  - **widgets:** label, button, toggle, checkbox, radio, slider, text field, multi-line text,
    list, tree, table, tabs, scroll area, menus, context menus, dialogs, and the file picker
    (which goes through `permd`)
  - **layout:** box, grid, and flex
  - HiDPI **scale factor** (integer first, fractional later)
  - keyboard navigation
- **Text:**
  - A custom TrueType/OpenType rasterizer (glyf outlines first, CFF later) with anti-aliasing
    and a glyph cache.
  - UTF-8 throughout, basic kerning, and fallback fonts.
  - **Later:** full GSUB/GPOS shaping, bidi, and color emoji (COLR/CPAL).
  - Fonts: an OFL-licensed UI sans-serif and monospace font in `data/fonts/`.
- **`libs/gfx`:** 2D rasterization (antialiased paths, rounded rects, blur for shadows) and
  image decoders for PNG, JPEG (baseline and progressive), BMP, and GIF.
- **Apps:**
  - **terminal:** xterm-256color on a PTY, with tabs and scrollback
  - **files:** the file manager
  - **editor:** multi-tab, with syntax highlighting later
  - **settings:** display, keyboard, users, network, sound, theme, window mode, and app
    permissions
  - **sysmon:** CPU, memory, processes, network
  - **imageview**
  - **player:** plays WAV, FLAC, MP3, and Ogg Vorbis audio; video later
  - **calc**
  - **browser:** HTTP/1.1 over our TLS 1.3, a WHATWG-lite HTML parser, and a CSS subset (box
    model, block/inline flow, colors, fonts, images; flexbox later). **No JavaScript.**
- **Ports:** SDL2 (video through the compositor, audio through `sndd`, input), then
  chocolate-doom with the **Freedoom** WAD.

---

## 20. Audio
- **HDA driver (module):**
  - enumerates codecs and walks the widget graph to find the line-out and headphone paths
  - handles jack detection
  - DMA uses buffer descriptor lists at 48 kHz, 16 and 24 bit
- **Targets:** the Realtek codec on the reference board, NVIDIA HDMI audio, and QEMU
  intel-hda.
- **`sndd` (userspace sound server):**
  - mixes streams from every client (shared ring buffers)
  - per-app and master volume
  - runs on a realtime-class thread
- **Testing:** QEMU runs with `-audiodev wav,path=...`, so tests capture the output and verify
  its frequency and level automatically.

---

## 21. Security summary

| Area | Measures |
|---|---|
| **Kernel hardening** | NX; W^X everywhere; SMEP/SMAP/UMIP; stack canaries (kernel + user); guard pages on every stack; KASLR; UBSan + lock validator in debug builds; checked user copies |
| **DMA protection** | AMD-Vi IOMMU per userspace driver |
| **Capabilities** | handles with rights; no ambient authority; rights can only shrink |
| **Users** | Argon2id passwords; `elevate` for admin |
| **Sandboxing** | GUI apps in namespace sandboxes with `permd` grants; terminal tools unsandboxed |
| **Network** | stateful firewall, default-deny inbound |
| **Boot integrity (late)** | UEFI Secure Boot with the user's own keys; loader verifies the kernel and initrd; kernel verifies modules |
| **Data at rest (late)** | AES-XTS full-disk encryption with Argon2id |
| **Crypto** | custom, tested, labeled EXPERIMENTAL (§17) |

---

## 22. Linux binary compatibility (late)
- **Detection:** the ELF has no `.note.bongos` note, or its `PT_INTERP` is `ld-linux`. Such a
  process gets the **LINUX personality**: its syscall numbers and structs follow the Linux
  x86_64 ABI, and are translated onto native operations.
- **Required pieces:**
  - `clone` (on the kernel's address-space clone and threads)
  - futex
  - epoll (on top of ports)
  - signals with Linux frame layout
  - `/proc` and `/sys` subsets
  - Linux errno mapping
  - `ioctl` for terminals
- **Userland:** Linux programs run against their own libraries in `/compat/linux` (a sysroot
  with glibc and its `ld-linux`).
- **Order of targets:** static busybox, then dynamic glibc command-line programs, then larger
  apps.

---

## 23. Testing strategy

Every milestone ships with tests, and `make test` must pass before any PR.

| Level | What | Where |
|---|---|---|
| Host unit tests | pure logic: allocators' algorithms, bongfs core, crypto (official vectors), parsers (ELF, GPT, FAT, AML, HTML, CSS, fonts, PNG) | `make host-tests`, runs natively on Linux, fast |
| In-kernel tests (ktest) | `KTEST(name) { ... }` registered in a section; run at boot when `cmdline` has `ktest=all` or a pattern | `kernel/test/` |
| Boot matrix | UEFI × BIOS × 1 CPU × 4 CPUs × an optional memory size (D-084) | `make test` (quick), `make test-full` (with swap, IOMMU, all devices, and a >4 GiB row to exercise the pmm's NORMAL zone) |
| Userspace tests | test binaries in the test initrd, run by `svcd` in test mode | `tests/user/` |
| Filesystem interop | create images with Linux tools, mutate in bongOS, verify with Linux `fsck` | `tests/fs/` |
| Network interop | from the host: `ping`, OpenSSH `ssh`, `curl`, `openssl s_server`, via QEMU user networking + `hostfwd` | `tests/net/` |
| Audio | capture QEMU WAV output, check tone | `tests/audio/` |
| GUI | QEMU monitor `screendump` compared against reference PNGs with a tolerance (`tools/imgdiff`) | `tests/gui/` |
| Real hardware | manual checklist in the milestone, run by the owner, result recorded in the milestone log | `docs/ROADMAP.md` |

**Result protocol:** the kernel prints `KTEST PASS name` or `KTEST FAIL name: reason` on
serial. The run ends by writing to the `isa-debug-exit` port (0xF4): `0x10` means all passed
(QEMU exits 33), `0x11` means failure (QEMU exits 35). The harness maps a timeout to HANG and
a reset to CRASH.

**Deliberately faulting a ktest (M2.1, D-078):** a ktest that needs to prove the kernel *detects*
a real fault (a deliberate #PF, #UD) or a software-checked violation (a stack smash, a UBSan trip,
or -- M2.2, D-082 -- a `panicBug()`-reported kernel-internal invariant violation like a pmm double
free, via `TRAP_CATCH_KERNEL_BUG`) without ending the whole test run calls `archTrapCatch(mask,
fn, arg, &info)`, which runs `fn` with a saved callee-saved-register context. A matching
**hardware fault** (vector 0-31) prints the normal report first (still exercising the symbolized
backtrace) before redirecting execution back to `archTrapCatch`'s caller; a matching **software
trip** (`TRAP_CATCH_STACK_SMASH`/`TRAP_CATCH_UBSAN`/`TRAP_CATCH_KERNEL_BUG`, offered by
`__stack_chk_fail()`/the UBSan handlers/`panicBug()` via `archTrapCatchSoftware()`) is deliberately
silent on serial and redirects immediately -- the ktest itself reports PASS/FAIL, and the point of
catching it there is to *avoid* the loud panic report a real, uncaught trip still prints. Either
way, `*info` is filled in with what was caught. One-shot, non-nesting, ktest-only (`archTrapCatch`
itself panics if called outside a ktest run), and it can never catch NMI/#DF/#MC or #BP -- those
stay always-fatal, or (#BP) already resume unconditionally before archTrapCatch ever sees them
(§7.2).

---

## 24. Debugging and observability
- **klog:** levels (error, warn, info, debug, trace), per-subsystem tags, and output to serial
  plus fbcon plus a ring buffer (`dmesg`).
- **Panic screen:** reason, registers, CPU number, current thread, and a **symbolized
  backtrace**. Frame pointers are kept, and the kernel embeds a compressed symbol table (KSYM v1,
  `docs/specs/ksyms.md`, D-075 -- built by a two-pass link + `tools/ksyms`, looked up by
  `ksymSymbolize()`). Other CPUs are stopped by IPI (once SMP exists, M3.5).
- **Debugger:** `make gdb` runs QEMU's gdbstub with symbols loaded. `make debug` adds
  `-d int,cpu_reset` logging.
- **Userspace crashes:** the crashing process's registers and backtrace go to `logd`, and
  `svcd` restarts the service if configured to.
- **procfs:** `/proc/meminfo`, `/proc/cpuinfo`, `/proc/<pid>/status`, `/proc/interrupts`,
  `/proc/modules`.

---

## 25. Project process
- **Releases:**
  - Each finished milestone is tagged `v0.<phase>.<milestone>`. CI builds `bongos.img` and
    attaches it (compressed) to a GitHub Release.
  - **Each phase has a cannabis-strain codename** (alphabetical, listed in ROADMAP.md), used
    in the release titles, e.g. "bongOS 0.2.3 (Blue Dream)".
  - `v1.0` means daily-drivable: the desktop, the core apps, networking, packages, and
    installing to a real disk all work.
- **Merge policy:** PRs auto-merge when CI is green **and** the `reviewer` subagent reports no
  Critical findings. The exception is any PR labeled `needs-owner`, which waits for the owner.
  These PRs always get the label:
  - memory management
  - interrupts or SMP
  - the scheduler
  - security features or crypto
  - on-disk filesystem formats
  - the boot handoff ABI
- **Progress logging, decisions, and session rules:** see `CLAUDE.md`.
