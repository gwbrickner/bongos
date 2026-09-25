#!/bin/bash
# Runs the GUI (boot-menu screenshot) tests (ARCHITECTURE §23, D-070): builds
# build/bongos-menutest.img from tests/gui/menutest-boot.cfg, drives each tests/gui/scripts/*
# through tests/harness/run-qemu.sh --script, then compares every screendump against its
# checked-in reference PNG with tools/imgdiff. `--update-refs` captures new references instead of
# comparing -- the caller must view every regenerated PNG and explain why in the milestone log
# (CLAUDE.md: regenerating a reference to turn a failing test green counts as weakening it).
set -u
FW=uefi
UPDATE_REFS=0
while [ $# -gt 0 ]; do
    case "$1" in
        --fw) FW=$2; shift 2 ;;
        --update-refs) UPDATE_REFS=1; shift ;;
        -h|--help) echo "usage: $0 [--fw uefi|bios] [--update-refs]"; exit 0 ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
done

IMGDIFF=build/tools/imgdiff/imgdiff
[ -x "$IMGDIFF" ] || { echo "GUI: $IMGDIFF not built (run 'make imgdiff' first)"; exit 1; }

IMAGE=build/bongos-menutest.img
build/tools/mkimage/mkimage --output "$IMAGE" --efi build/boot-uefi/BOOTX64.EFI \
    --kernel build/kernel/kernel.elf --boot-cfg tests/gui/menutest-boot.cfg || exit 1

mkdir -p build/shots
status=0
for script in tests/gui/scripts/*.script; do
    name=$(basename "$script" .script)
    tests/harness/run-qemu.sh --image "$IMAGE" --fw "$FW" --cpus 1 --timeout 30 \
        --name "gui-$name" --script "$script" || status=1
done

if [ "$UPDATE_REFS" = 1 ]; then
    mkdir -p tests/gui/ref
    updated=0
    for f in build/shots/"$FW"-*.ppm; do
        [ -e "$f" ] || continue
        shot=$(basename "$f" .ppm)
        shot=${shot#"$FW"-}
        if "$IMGDIFF" convert "$f" "tests/gui/ref/${FW}-${shot}.png"; then
            updated=$((updated + 1))
        else
            status=1
        fi
    done
    echo "GUI: updated $updated reference PNG(s) under tests/gui/ref/ -- view every one before" \
        "committing, and explain why in the milestone log"
    exit $status
fi

compared=0
for f in build/shots/"$FW"-*.ppm; do
    [ -e "$f" ] || continue
    shot=$(basename "$f" .ppm)
    shot=${shot#"$FW"-}
    ref="tests/gui/ref/${FW}-${shot}.png"
    [ -f "$ref" ] || ref="tests/gui/ref/${shot}.png"
    if [ ! -f "$ref" ]; then
        echo "GUI: no reference for $f (looked for tests/gui/ref/${FW}-${shot}.png and tests/gui/ref/${shot}.png)"
        status=1
        continue
    fi
    compared=$((compared + 1))
    maskArg=()
    maskFile="${ref%.png}.mask"
    [ -f "$maskFile" ] && maskArg=(--mask "$maskFile")
    diffOut="build/shots/${FW}-${shot}.diff.png"
    if "$IMGDIFF" compare "$f" "$ref" "${maskArg[@]}" --diff-out "$diffOut"; then
        echo "GUI: $shot matches $ref"
    else
        echo "GUI: $shot does not match $ref (diff: $diffOut)"
        status=1
    fi
done
if [ "$compared" -eq 0 ]; then
    echo "GUI: no screenshots found under build/shots/$FW-*.ppm"
    status=1
fi

[ "$status" -eq 0 ] && echo "GUI: PASS" || echo "GUI: FAIL"
exit $status
