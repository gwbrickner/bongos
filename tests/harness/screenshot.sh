#!/bin/bash
# Captures what a normal boot of build/bongos.img leaves on screen, as a PNG (D-099). The finish
# protocol (CLAUDE.md) commits one per milestone as docs/screenshots/M<p>.<n>.png and embeds it
# in the PR. Uses run-qemu.sh's --script mode with --script-only, since the release image never
# writes isa-debug-exit; the PNG encoder is tools/imgdiff's own (`imgdiff convert`).
set -u
OUT=build/shots/final.png IMAGE=build/bongos.img FW=uefi CPUS=1 MEM=512 TIMEOUT=60 SETTLE=1
EXPECT="kernel: init done"
usage() {
    cat <<'EOF'
usage: screenshot.sh [options]
  --out PATH        PNG to write (default build/shots/final.png)
  --image PATH      disk image (default build/bongos.img)
  --fw uefi|bios    firmware (default uefi)
  --cpus N          vCPUs (default 1)
  --mem MB          RAM in MiB (default 512)
  --expect S        serial line that means "boot finished" (default "kernel: init done")
  --settle S        seconds to wait after --expect before the screendump (default 1)
  --timeout S       per-step timeout (default 60)
EOF
}
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT=$2; shift 2 ;; --image) IMAGE=$2; shift 2 ;; --fw) FW=$2; shift 2 ;;
        --cpus) CPUS=$2; shift 2 ;; --mem) MEM=$2; shift 2 ;; --expect) EXPECT=$2; shift 2 ;;
        --settle) SETTLE=$2; shift 2 ;; --timeout) TIMEOUT=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option $1"; usage; exit 2 ;;
    esac
done

IMGDIFF=build/tools/imgdiff/imgdiff
[ -x "$IMGDIFF" ] || { echo "screenshot: $IMGDIFF not built (run 'make imgdiff' first)"; exit 1; }

NAME="screenshot-$FW"
SCRIPT="build/run/$NAME.script"
# qemu-script.py names screendumps "<fw>-<step>.ppm" in build/shots/, the same directory
# tests/gui/run.sh globs for "<fw>-*.ppm" -- so this one is always removed once converted, or a
# leftover would make the next GUI test run fail with "no reference".
PPM="build/shots/$FW-screenshot-final.ppm"
mkdir -p build/run build/shots "$(dirname "$OUT")"
rm -f "$PPM"
printf 'expect %s\nsleep %s\nscreendump screenshot-final\nquit\n' "$EXPECT" "$SETTLE" > "$SCRIPT"

tests/harness/run-qemu.sh --image "$IMAGE" --fw "$FW" --cpus "$CPUS" --mem "$MEM" \
    --timeout "$TIMEOUT" --name "$NAME" --script "$SCRIPT" --script-only
status=$?
if [ "$status" -eq 0 ]; then
    "$IMGDIFF" convert "$PPM" "$OUT" || status=1
fi
rm -f "$PPM"
[ "$status" -eq 0 ] && echo "screenshot: wrote $OUT" || echo "screenshot: FAILED (see build/logs/$NAME.serial.log)"
exit $status
