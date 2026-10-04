#!/bin/bash
# The `cpus=N` cmdline option end to end (M3.5, ROADMAP item 4, D-193): no matrix row boots with a
# `cpus=` token, so this boots the kernel on 4 vCPUs with each of these command lines and checks the
# boot log and the ktests that read the option (smp_online_matches_madt honors `cpus=`):
#   cpus=2    -> 2 cpus online, 1 AP line, no smp warning
#   cpus=1    -> 1 cpu online, no AP line, no smp warning
#   cpus=0    -> the invalid-value warning, all 4 cpus online
#   cpus=abc  -> the same
# Each run must end PASS. Run from anywhere; needs `make image` first. Images:
# build/smp-cpus-<case>.img (deleted after use). Serial logs: build/logs/smp-cpus-<case>.serial.log.
# One QEMU at a time.
set -u
cd "$(dirname "$0")/../.." || exit 2

FW=uefi
while [ $# -gt 0 ]; do
    case "$1" in
        --fw) FW=$2; shift 2 ;;
        -h|--help) echo "usage: smp-cpus-check.sh [--fw uefi|bios]"; exit 0 ;;
        *) echo "smp-cpus-check: unknown option $1"; exit 2 ;;
    esac
done
case "$FW" in uefi|bios) ;; *) echo "smp-cpus-check: --fw must be uefi or bios"; exit 2 ;; esac
for f in build/tools/mkimage/mkimage build/boot-uefi/BOOTX64.EFI build/kernel/kernel.elf \
         build/boot-bios/stage1.bin build/boot-bios/stage2.bin; do
    [ -f "$f" ] || { echo "smp-cpus-check: $f missing (run 'make image')"; exit 2; }
done

TESTS=smp_online_matches_madt,smp_call_function_all_cpus,smp_tlb_shootdown_batched
# value | cpus expected online | 1 if the invalid-value warning must be printed
CASES=("2|2|0" "1|1|0" "0|4|1" "abc|4|1")

status=0
for entry in "${CASES[@]}"; do
    IFS='|' read -r value want warn <<< "$entry"
    name="smp-cpus-$value"
    img="build/$name.img"
    cfg="build/$name.cfg"
    printf 'kernel = /bong/kernel.elf\ncmdline = ktest=%s cpus=%s\n' "$TESTS" "$value" > "$cfg"
    if ! build/tools/mkimage/mkimage --output "$img" --efi build/boot-uefi/BOOTX64.EFI \
            --kernel build/kernel/kernel.elf --boot-cfg "$cfg" --stage1 build/boot-bios/stage1.bin \
            --stage2 build/boot-bios/stage2.bin > /dev/null 2>&1; then
        echo "smp-cpus-check: mkimage failed for cpus=$value"; status=1; rm -f "$cfg"; continue
    fi
    result=$(tests/harness/run-qemu.sh --image "$img" --fw "$FW" --cpus 4 --name "$name" \
             --timeout 120 2>&1 | grep '^RESULT')
    rm -f "$img" "$cfg"
    text=$(tr -d '\r' < "build/logs/$name.serial.log" 2>/dev/null)
    errs=()
    case "$result" in *": PASS"*) ;; *) errs+=("run result is '$result', want PASS") ;; esac
    grep -qx "\[info\] smp: $want cpus online" <<< "$text" ||
        errs+=("no '[info] smp: $want cpus online' line")
    nap=$(grep -cE '^\[info\] smp: cpu [0-9]+ apic-id=[0-9]+ online$' <<< "$text")
    [ "$nap" = "$((want - 1))" ] || errs+=("$nap AP online lines, want $((want - 1))")
    nwarn=$(grep -cx '\[warn\] smp: ignoring invalid cpus= (want a number from 1 to 64)' <<< "$text")
    [ "$nwarn" = "$warn" ] || errs+=("$nwarn invalid-cpus= warnings, want $warn")
    nother=$(grep -E '^\[(warn|error)\] smp:' <<< "$text" | grep -vc 'ignoring invalid cpus=')
    [ "$nother" = 0 ] || errs+=("$nother other smp warn/error lines")
    if [ ${#errs[@]} = 0 ]; then
        echo "RESULT smp-cpus-check[$FW] cpus=$value: PASS"
    else
        for e in "${errs[@]}"; do echo "smp-cpus-check[$FW] cpus=$value: $e"; done
        echo "RESULT smp-cpus-check[$FW] cpus=$value: FAIL (see build/logs/$name.serial.log)"
        status=1
    fi
done
exit $status
