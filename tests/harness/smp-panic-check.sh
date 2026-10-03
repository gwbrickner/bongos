#!/bin/bash
# SMP panic check (M3.5, D-197, D-201): a panic with several CPUs online must end the run cleanly.
# For each probe in kernel/test/smp_panic_probe_test.c, boots an image whose cmdline is
# `ktest=<probe> smp-panic-probe` on 4 vCPUs and requires:
#   - RESULT FAIL (the isa-debug-exit FAIL code: not a HANG, not a CRASH),
#   - exactly one "PANIC: " report line and no "PANIC while already panicking" (the CPUs that lose
#     the race park silently; no report interleaves with the winner's),
#   - the probe's own "KTEST FAIL <probe>: panic: <message>" line.
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
    if [ ${#errs[@]} = 0 ]; then
        echo "RESULT smp-panic-check[$FW] $probe: PASS"
    else
        for e in "${errs[@]}"; do echo "smp-panic-check[$FW] $probe: $e"; done
        echo "RESULT smp-panic-check[$FW] $probe: FAIL (see $log)"
        status=1
    fi
done
exit $status
