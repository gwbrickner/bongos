#!/bin/bash
# SMP panic check (M3.5, D-197, D-201): a panic with several CPUs online must end the run cleanly.
# For each probe in kernel/test/smp_panic_probe_test.c, boots an image whose cmdline is
# `ktest=<probe> smp-panic-probe` on 4 vCPUs and requires:
#   - RESULT FAIL (the isa-debug-exit FAIL code: not a HANG, not a CRASH),
#   - exactly one "PANIC: " report line and no "PANIC while already panicking" (the CPUs that lose
#     the race park silently; no report interleaves with the winner's),
#   - the probe's own "KTEST FAIL <probe>: panic: <message>" line,
#   - for smp_panic_probe_bsp_aps_logging: AP log lines before the report and none after it.
# lockdep_probe_expect_other_cpu exists only in debug builds: with --release (make RELEASE=1) its
# run is skipped; without it, a probe that never starts is a failure.
#
# Run from anywhere; needs `make image` first. Images: build/smp-panic-<probe>.img (deleted after
# use). Serial logs: build/logs/smp-panic-<probe>.serial.log. One QEMU at a time.
set -u
cd "$(dirname "$0")/../.." || exit 2

FW=uefi
RELEASE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --fw) FW=$2; shift 2 ;;
        --release) RELEASE=1; shift ;;
        -h|--help) echo "usage: smp-panic-check.sh [--fw uefi|bios] [--release]"; exit 0 ;;
        *) echo "smp-panic-check: unknown option $1"; exit 2 ;;
    esac
done
case "$FW" in uefi|bios) ;; *) echo "smp-panic-check: --fw must be uefi or bios"; exit 2 ;; esac
for f in build/tools/mkimage/mkimage build/boot-uefi/BOOTX64.EFI build/kernel/kernel.elf \
         build/boot-bios/stage1.bin build/boot-bios/stage2.bin; do
    [ -f "$f" ] || { echo "smp-panic-check: $f missing (run 'make image')"; exit 2; }
done

# probe name | the panic message the report must carry (a fixed prefix, regex)
PROBES=(
    "smp_panic_probe_ap|smp-panic-probe: cpu 1 panics$"
    "smp_panic_probe_all|smp-panic-probe: cpu [0-9]+ panics with every other cpu$"
    "smp_panic_probe_bsp_aps_irqoff|smp-panic-probe: the boot cpu panics while 3 cpus spin with IF=0$"
    "smp_panic_probe_bsp_aps_logging|smp-panic-probe: the boot cpu panics while 3 cpus log$"
    "lockdep_probe_expect_other_cpu|lockdep: lock order inversion \(report above\)$"
)

status=0
for entry in "${PROBES[@]}"; do
    probe=${entry%%|*}
    msg=${entry#*|}
    if [ "$RELEASE" = 1 ] && [ "$probe" = lockdep_probe_expect_other_cpu ]; then
        echo "RESULT smp-panic-check[$FW] $probe: SKIP (release build: no lock validator)"
        continue
    fi
    name="smp-panic-$probe"
    img="build/$name.img"
    cfg="build/$name.cfg"
    printf 'kernel = /bong/kernel.elf\ncmdline = ktest=%s smp-panic-probe\n' "$probe" > "$cfg"
    if ! build/tools/mkimage/mkimage --output "$img" --efi build/boot-uefi/BOOTX64.EFI \
            --kernel build/kernel/kernel.elf --boot-cfg "$cfg" --stage1 build/boot-bios/stage1.bin \
            --stage2 build/boot-bios/stage2.bin > /dev/null 2>&1; then
        echo "smp-panic-check: mkimage failed for $probe"; status=1; rm -f "$cfg"; continue
    fi
    result=$(tests/harness/run-qemu.sh --image "$img" --fw "$FW" --cpus 4 --name "$name" \
             --timeout 90 2>&1 | grep '^RESULT')
    rm -f "$img" "$cfg"
    log="build/logs/$name.serial.log"
    text=$(tr -d '\r' < "$log" 2>/dev/null)
    if ! grep -qxF "KTEST START $probe" <<< "$text"; then
        echo "RESULT smp-panic-check[$FW] $probe: FAIL (the probe never started; see $log)"
        status=1; continue
    fi
    errs=()
    case "$result" in *": FAIL "*) ;; *) errs+=("run result is '$result', want FAIL") ;; esac
    npanic=$(grep -c '^PANIC' <<< "$text")
    [ "$npanic" = 1 ] || errs+=("$npanic lines start with PANIC, want exactly 1")
    grep -q 'PANIC while already panicking' <<< "$text" && errs+=("a nested-panic report was printed")
    grep -qE "^PANIC: $msg" <<< "$text" || errs+=("no 'PANIC: $msg' line")
    grep -qE "^KTEST FAIL $probe: panic: $msg" <<< "$text" ||
        errs+=("no 'KTEST FAIL $probe: panic: $msg' line")
    if [ "$probe" = smp_panic_probe_bsp_aps_logging ]; then
        # The APs logged before the panic, and the stop IPI silenced them before the report.
        before=$(awk '/^PANIC/ { exit } /probe-spam/ { n++ } END { print n + 0 }' <<< "$text")
        after=$(awk '/^PANIC/ { p = 1 } p && /probe-spam/ { n++ } END { print n + 0 }' <<< "$text")
        [ "$before" -gt 0 ] || errs+=("no AP log line before the panic report")
        [ "$after" = 0 ] || errs+=("$after AP log lines after the panic report (the other CPUs were not stopped)")
    fi
    if [ ${#errs[@]} = 0 ]; then
        echo "RESULT smp-panic-check[$FW] $probe: PASS"
    else
        for e in "${errs[@]}"; do echo "smp-panic-check[$FW] $probe: $e"; done
        echo "RESULT smp-panic-check[$FW] $probe: FAIL (see $log)"
        status=1
    fi
done

# A panic on an AP during bring-up, before its local APIC is set up (KERNEL_DEBUG hook in apMain,
# cmdline token smp-ap-early-panic-probe, no ktest): its stop IPI must reach the BSP (which is
# waiting for that AP with IF=1), and the report must be the only one. The run then halts (no ktest,
# so no isa-debug-exit): the expected result is HANG at the short timeout. Under x2APIC (KVM's
# `-cpu max`) this also proves the AP's APIC already follows the BSP's mode at that point.
probe=smp_ap_early_panic
if [ "$RELEASE" = 1 ]; then
    echo "RESULT smp-panic-check[$FW] $probe: SKIP (release build: no probe hook)"
else
    name="smp-panic-$probe"
    img="build/$name.img"
    cfg="build/$name.cfg"
    printf 'kernel = /bong/kernel.elf\ncmdline = smp-ap-early-panic-probe\n' > "$cfg"
    if ! build/tools/mkimage/mkimage --output "$img" --efi build/boot-uefi/BOOTX64.EFI \
            --kernel build/kernel/kernel.elf --boot-cfg "$cfg" --stage1 build/boot-bios/stage1.bin \
            --stage2 build/boot-bios/stage2.bin > /dev/null 2>&1; then
        echo "smp-panic-check: mkimage failed for $probe"; rm -f "$cfg"; exit 1
    fi
    result=$(tests/harness/run-qemu.sh --image "$img" --fw "$FW" --cpus 4 --name "$name" \
             --timeout 60 2>&1 | grep '^RESULT')
    rm -f "$img" "$cfg"
    log="build/logs/$name.serial.log"
    text=$(tr -d '\r' < "$log" 2>/dev/null)
    errs=()
    case "$result" in *": HANG"*) ;; *) errs+=("run result is '$result', want HANG (halted after the report)") ;; esac
    grep -q 'smp: trampoline page=' <<< "$text" || errs+=("the boot never reached AP bring-up")
    npanic=$(grep -c '^PANIC' <<< "$text")
    [ "$npanic" = 1 ] || errs+=("$npanic lines start with PANIC, want exactly 1")
    grep -q 'PANIC while already panicking' <<< "$text" && errs+=("a nested-panic report was printed")
    grep -qE '^PANIC: smp-ap-early-panic-probe: cpu 1 panics before its local APIC is set up$' \
        <<< "$text" || errs+=("no 'PANIC: smp-ap-early-panic-probe: cpu 1 ...' line")
    grep -qE 'did not come online|cpus online' <<< "$text" &&
        errs+=("the BSP kept booting after the AP's panic (it was not stopped)")
    mode=$(grep -oE 'lapic: mode=[a-z0-9]+' <<< "$text" | head -1)
    if [ ${#errs[@]} = 0 ]; then
        echo "RESULT smp-panic-check[$FW] $probe: PASS (${mode:-lapic mode unknown})"
    else
        for e in "${errs[@]}"; do echo "smp-panic-check[$FW] $probe: $e"; done
        echo "RESULT smp-panic-check[$FW] $probe: FAIL (see $log)"
        status=1
    fi
fi

# An AP that is too slow (KERNEL_DEBUG hooks in apMain: the AP with APIC id 1 spins for 3 s, past
# the BSP's 2 s deadline; token smp-ap-stall-probe right after it entered, smp-ap-stall-tsc-probe
# after the TSC check, just before its claim): the BSP must give up on it cleanly (one warning, its
# id reused by the next AP). The hooks also delay the BSP's park INIT past the end of the stall, so
# the AP's own half of the D-206 handshake is what must keep it out of the online mask: it must
# never print an "apic-id=1 online" line. The call-function and TLB-shootdown ktests that follow
# must neither hang nor panic (a dead CPU in the mask would stall them for 10 s and panic).
# 4 vCPUs -> 3 online.
for entry in "smp_ap_late|smp-ap-stall-probe" "smp_ap_late_tsc|smp-ap-stall-tsc-probe"; do
    probe=${entry%%|*}
    token=${entry#*|}
    if [ "$RELEASE" = 1 ]; then
        echo "RESULT smp-panic-check[$FW] $probe: SKIP (release build: no probe hook)"
        continue
    fi
    name="smp-panic-$probe"
    img="build/$name.img"
    cfg="build/$name.cfg"
    printf 'kernel = /bong/kernel.elf\ncmdline = ktest=smp_call_function_all_cpus,smp_tlb_shootdown_batched %s\n' "$token" > "$cfg"
    if ! build/tools/mkimage/mkimage --output "$img" --efi build/boot-uefi/BOOTX64.EFI \
            --kernel build/kernel/kernel.elf --boot-cfg "$cfg" --stage1 build/boot-bios/stage1.bin \
            --stage2 build/boot-bios/stage2.bin > /dev/null 2>&1; then
        echo "smp-panic-check: mkimage failed for $probe"; rm -f "$cfg"; exit 1
    fi
    result=$(tests/harness/run-qemu.sh --image "$img" --fw "$FW" --cpus 4 --name "$name" \
             --timeout 120 2>&1 | grep '^RESULT')
    rm -f "$img" "$cfg"
    log="build/logs/$name.serial.log"
    text=$(tr -d '\r' < "$log" 2>/dev/null)
    errs=()
    case "$result" in *": PASS"*) ;; *) errs+=("run result is '$result', want PASS") ;; esac
    grep -qx '\[info\] smp: 3 cpus online' <<< "$text" || errs+=("no '[info] smp: 3 cpus online' line")
    nlate=$(grep -cE '^\[warn\] smp: cpu apic-id=1 did not come online ' <<< "$text")
    [ "$nlate" = 1 ] || errs+=("$nlate 'did not come online' warnings for apic-id 1, want 1")
    grep -qE '^\[info\] smp: cpu 1 apic-id=2 online$' <<< "$text" || errs+=("the next AP did not reuse cpu id 1")
    grep -qE 'smp: cpu [0-9]+ apic-id=1 online' <<< "$text" && errs+=("the given-up AP published itself (apic-id=1 online)")
    grep -q '^PANIC' <<< "$text" && errs+=("a panic was printed")
    if [ ${#errs[@]} = 0 ]; then
        echo "RESULT smp-panic-check[$FW] $probe: PASS"
    else
        for e in "${errs[@]}"; do echo "smp-panic-check[$FW] $probe: $e"; done
        echo "RESULT smp-panic-check[$FW] $probe: FAIL (see $log)"
        status=1
    fi
done

# The BSP half of the D-206 handshake: the AP with APIC id 1 stalls 3 s before it claims the right to
# publish (smp-ap-stall-tsc-probe) and the BSP, past its 2 s deadline, waits 1.5 s between reading the
# AP's stage and its give-up compare-exchange (smp-bsp-giveup-lag-probe): the AP claims inside that
# window, so the exchange must fail and the BSP must go on waiting for it. Result: all 4 CPUs online,
# no 'did not come online', and the ktests that follow neither hang nor panic. With the exchange
# turned into a plain store the BSP INIT-parks a CPU that is already in the online mask.
probe=smp_bsp_giveup_race
if [ "$RELEASE" = 1 ]; then
    echo "RESULT smp-panic-check[$FW] $probe: SKIP (release build: no probe hook)"
else
    name="smp-panic-$probe"
    img="build/$name.img"
    cfg="build/$name.cfg"
    printf 'kernel = /bong/kernel.elf\ncmdline = ktest=smp_call_function_all_cpus,smp_tlb_shootdown_batched smp-ap-stall-tsc-probe smp-bsp-giveup-lag-probe\n' > "$cfg"
    if ! build/tools/mkimage/mkimage --output "$img" --efi build/boot-uefi/BOOTX64.EFI \
            --kernel build/kernel/kernel.elf --boot-cfg "$cfg" --stage1 build/boot-bios/stage1.bin \
            --stage2 build/boot-bios/stage2.bin > /dev/null 2>&1; then
        echo "smp-panic-check: mkimage failed for $probe"; rm -f "$cfg"; exit 1
    fi
    result=$(tests/harness/run-qemu.sh --image "$img" --fw "$FW" --cpus 4 --name "$name" \
             --timeout 120 2>&1 | grep '^RESULT')
    rm -f "$img" "$cfg"
    log="build/logs/$name.serial.log"
    text=$(tr -d '\r' < "$log" 2>/dev/null)
    errs=()
    case "$result" in *": PASS"*) ;; *) errs+=("run result is '$result', want PASS") ;; esac
    grep -qx '\[info\] smp: 4 cpus online' <<< "$text" || errs+=("no '[info] smp: 4 cpus online' line")
    grep -q 'bsp-giveup-lag-probe: delaying' <<< "$text" ||
        errs+=("the BSP never reached the lag hook (the probe would pass vacuously)")
    grep -q 'did not come online' <<< "$text" && errs+=("the BSP gave up on the AP that had claimed")
    grep -qE '^\[info\] smp: cpu 1 apic-id=1 online$' <<< "$text" || errs+=("cpu 1 (apic-id 1) never came online")
    grep -q '^PANIC' <<< "$text" && errs+=("a panic was printed")
    if [ ${#errs[@]} = 0 ]; then
        echo "RESULT smp-panic-check[$FW] $probe: PASS"
    else
        for e in "${errs[@]}"; do echo "smp-panic-check[$FW] $probe: $e"; done
        echo "RESULT smp-panic-check[$FW] $probe: FAIL (see $log)"
        status=1
    fi
fi
exit $status
