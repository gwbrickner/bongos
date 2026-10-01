#!/bin/bash
# KASLR boot check (M2.6, ARCHITECTURE §6.6, D-125): proves, for one firmware, that the shipped
# image really slides the kernel and that `kaslr = off` really doesn't.
#
#   1. Boots build/bongos.img (the shipped boot.cfg, KASLR on by default) N times (default 4).
#      Every boot must print the loader's `loader: kaslr: slide=0x<16 hex> base=0x<16 hex>
#      relocs=<n>` line and the kernel's `[info] kaslr: virtBase=0x<16 hex> slide=0x<16 hex>`
#      line, and they must agree: virtBase - slide == 0xffffffff80000000, the slide is 2 MiB
#      aligned and <= 0x1FE00000, loader slide == kernel slide, loader base == virtBase.
#   2. At least 2 distinct slides among the N boots (256 slots; a false failure needs all 4
#      seeds to land in one slot: 256^-3 = 6e-8).
#   3. Boots build/bongos-kaslroff.img (tests/harness/kaslr-off-boot.cfg: `kaslr = off`, ktest=all):
#      exit 33 (all ktests pass), `loader: kaslr: off (boot.cfg); base=0xffffffff80000000`, a kernel
#      line with slide 0, and `KTEST PASS kaslr_slide_consistent`.
#
# Run from anywhere; needs `make image` first. Serial logs: build/logs/kaslr-<fw>-<i>.serial.log and
# build/logs/kaslr-<fw>-off.serial.log. One QEMU at a time (run-qemu.sh shares build/run/).
set -u
cd "$(dirname "$0")/../.." || exit 2

FW=uefi
BOOTS=4
IMAGE=build/bongos.img
OFF_IMAGE=build/bongos-kaslroff.img
while [ $# -gt 0 ]; do
    case "$1" in
        --fw) FW=$2; shift 2 ;;
        --boots) BOOTS=$2; shift 2 ;;
        -h|--help)
            echo "usage: kaslr-check.sh --fw uefi|bios [--boots N]"; exit 0 ;;
        *) echo "kaslr-check: unknown option $1"; exit 2 ;;
    esac
done
case "$FW" in uefi|bios) ;; *) echo "kaslr-check: --fw must be uefi or bios"; exit 2 ;; esac
[ "$BOOTS" -ge 2 ] 2>/dev/null || { echo "kaslr-check: --boots must be >= 2"; exit 2; }
for img in "$IMAGE" "$OFF_IMAGE"; do
    [ -f "$img" ] || { echo "kaslr-check: $img missing (run 'make image')"; exit 2; }
done

# Parses one serial log; prints the loader's slide (hex, no 0x) on success, or the reasons on stderr.
# Args: log, mode ("on" or "off").
checkLog() {
    python3 - "$1" "$2" << 'PYEOF'
import re, sys
log, mode = sys.argv[1], sys.argv[2]
BASE = 0xFFFFFFFF80000000
text = open(log, "rb").read().decode("ascii", "replace").replace("\r", "")
lines = text.split("\n")
errs = []

def find(pat):
    rx = re.compile(pat)
    hits = [m for m in (rx.match(l) for l in lines) if m]
    return hits

kern = find(r"^\[info\] kaslr: virtBase=0x([0-9a-f]{16}) slide=0x([0-9a-f]{16})$")
if len(kern) != 1:
    errs.append("expected exactly 1 kernel 'kaslr: virtBase=... slide=...' line, found %d" % len(kern))
    virt = slide_k = None
else:
    virt, slide_k = int(kern[0].group(1), 16), int(kern[0].group(2), 16)
    if virt - slide_k != BASE:
        errs.append("virtBase 0x%x - slide 0x%x != 0xffffffff80000000" % (virt, slide_k))
    if slide_k % 0x200000 != 0:
        errs.append("kernel slide 0x%x is not 2 MiB aligned" % slide_k)
    if slide_k > 0x1FE00000:
        errs.append("kernel slide 0x%x > 0x1fe00000" % slide_k)

if mode == "on":
    ld = find(r"^loader: kaslr: slide=0x([0-9a-f]{16}) base=0x([0-9a-f]{16}) relocs=([0-9]+)$")
    if len(ld) != 1:
        errs.append("expected exactly 1 loader 'kaslr: slide=... base=... relocs=...' line, "
                    "found %d (a 'kaslr: disabled' fallback fails this check)" % len(ld))
    else:
        slide_l, base_l, relocs = int(ld[0].group(1), 16), int(ld[0].group(2), 16), int(ld[0].group(3))
        if relocs == 0:
            errs.append("loader applied 0 relocations")
        if base_l != BASE + slide_l:
            errs.append("loader base 0x%x != 0xffffffff80000000 + slide 0x%x" % (base_l, slide_l))
        if slide_k is not None and slide_l != slide_k:
            errs.append("loader slide 0x%x != kernel slide 0x%x" % (slide_l, slide_k))
        if virt is not None and base_l != virt:
            errs.append("loader base 0x%x != kernel virtBase 0x%x" % (base_l, virt))
    if find(r"^loader: kaslr: (off|disabled)"):
        errs.append("loader reported kaslr off/disabled")
else:
    ld = find(r"^loader: kaslr: off \(boot\.cfg\); base=0xffffffff80000000$")
    if len(ld) != 1:
        errs.append("expected exactly 1 'loader: kaslr: off (boot.cfg); base=0xffffffff80000000' line, found %d" % len(ld))
    if find(r"^loader: kaslr: slide="):
        errs.append("kaslr=off but the loader printed a slide line")
    if slide_k is not None and slide_k != 0:
        errs.append("kaslr=off but the kernel slide is 0x%x" % slide_k)
    if not any(l == "KTEST PASS kaslr_slide_consistent" for l in lines):
        errs.append("no 'KTEST PASS kaslr_slide_consistent'")

if errs:
    for e in errs:
        sys.stderr.write("  %s\n" % e)
    sys.exit(1)
print("%x" % slide_k)
PYEOF
}

fail=0
slides=()
for i in $(seq 1 "$BOOTS"); do
    name="kaslr-$FW-$i"
    # Banner-match mode on the shipped image: it has no ktest=all, so it never reaches
    # isa-debug-exit; "kernel: init done" comes after the kernel's kaslr line.
    out=$(tests/harness/run-qemu.sh --fw "$FW" --image "$IMAGE" --name "$name" --timeout 60 \
              --expect-serial "[info] kaslr: virtBase=0x" --expect-serial "kernel: init done" 2>&1)
    rc=$?
    log="build/logs/$name.serial.log"
    if [ "$rc" -ne 0 ]; then
        echo "kaslr-check[$FW]: boot $i FAILED: $out"
        fail=1
        continue
    fi
    if slide=$(checkLog "$log" on); then
        echo "kaslr-check[$FW]: boot $i slide=0x$slide"
        slides+=("$slide")
    else
        echo "kaslr-check[$FW]: boot $i FAILED the slide checks (see $log)"
        fail=1
    fi
done

distinct=$(printf '%s\n' "${slides[@]:-}" | sed '/^$/d' | sort -u | wc -l)
if [ "$distinct" -lt 2 ]; then
    echo "kaslr-check[$FW]: FAILED: only $distinct distinct slide(s) among $BOOTS boots, need >= 2"
    fail=1
else
    echo "kaslr-check[$FW]: $distinct distinct slides among $BOOTS boots"
fi

name="kaslr-$FW-off"
out=$(tests/harness/run-qemu.sh --fw "$FW" --image "$OFF_IMAGE" --name "$name" --timeout 120 2>&1)
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "kaslr-check[$FW]: kaslr=off boot FAILED: $out"
    fail=1
elif slide=$(checkLog "build/logs/$name.serial.log" off); then
    echo "kaslr-check[$FW]: kaslr=off boot OK (slide=0x$slide)"
else
    echo "kaslr-check[$FW]: kaslr=off boot FAILED the checks (see build/logs/$name.serial.log)"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "RESULT kaslr-check[$FW]: FAIL"
    exit 1
fi
echo "RESULT kaslr-check[$FW]: PASS"
