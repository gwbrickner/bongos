#!/bin/bash
# bongOS QEMU boot runner. Boots an image once and maps the result to PASS/FAIL/CRASH/HANG.
# The kernel reports through isa-debug-exit (port 0xf4): 0x10 -> exit 33 (PASS), 0x11 -> exit 35 (FAIL).
set -u
usage() {
cat << 'USAGE'
usage: run-qemu.sh [options]
  --image PATH      disk image (default build/bongos.img)
  --fw uefi|bios    firmware (default uefi)
  --cpus N          vCPUs (default 1)
  --mem MB          RAM in MiB (default 512)
  --timeout S       seconds before HANG (default 120)
  --name NAME       log name (default <fw>-<cpus>cpu)
  --debug           add -d int,cpu_reset logging to build/logs/NAME.qemu.log
  --interactive     show a window / serial on stdio, no timeout (for humans)
  --gdb             start paused with -s -S, serial to a log file, and return immediately;
                     writes its pid to build/run/NAME.pid so a caller can gdb it and kill it
  --extra "ARGS"    extra QEMU arguments (devices, netdev, audio, iommu...)
Monitor socket: build/run/NAME.monitor (screendump, sendkey, system_powerdown).
Serial log:     build/logs/NAME.serial.log
USAGE
}
IMAGE=build/bongos.img FW=uefi CPUS=1 MEM=512 TIMEOUT=120 NAME="" DEBUG=0 INTERACTIVE=0 GDBMODE=0 EXTRA=""
while [ $# -gt 0 ]; do
    case "$1" in
        --image) IMAGE=$2; shift 2 ;; --fw) FW=$2; shift 2 ;; --cpus) CPUS=$2; shift 2 ;;
        --mem) MEM=$2; shift 2 ;; --timeout) TIMEOUT=$2; shift 2 ;; --name) NAME=$2; shift 2 ;;
        --debug) DEBUG=1; shift ;; --interactive) INTERACTIVE=1; shift ;; --gdb) GDBMODE=1; shift ;;
        --extra) EXTRA=$2; shift 2 ;; -h|--help) usage; exit 0 ;;
        *) echo "unknown option $1"; usage; exit 2 ;;
    esac
done
[ -n "$NAME" ] || NAME="${FW}-${CPUS}cpu"
mkdir -p build/logs build/run
[ -f "$IMAGE" ] || { echo "RESULT $NAME: ERROR (no image at $IMAGE)"; exit 1; }

# Copy-on-write overlay so every run starts from the pristine image.
OVERLAY="build/run/$NAME.qcow2"
rm -f "$OVERLAY"
qemu-img create -q -f qcow2 -b "$(realpath "$IMAGE")" -F raw "$OVERLAY"

FWARGS=()
if [ "$FW" = "uefi" ]; then
    CODE="" VARS=""
    for c in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/ovmf/OVMF.fd; do
        [ -f "$c" ] && { CODE=$c; break; }
    done
    for v in /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd; do
        [ -f "$v" ] && { VARS=$v; break; }
    done
    [ -n "$CODE" ] || { echo "RESULT $NAME: ERROR (OVMF not found; run tools/ci/install-deps.sh)"; exit 1; }
    if [ -n "$VARS" ]; then
        cp "$VARS" "build/run/$NAME.vars.fd"
        FWARGS=(-drive "if=pflash,format=raw,unit=0,file=$CODE,readonly=on"
                -drive "if=pflash,format=raw,unit=1,file=build/run/$NAME.vars.fd")
    else
        FWARGS=(-bios "$CODE")
    fi
fi   # bios: QEMU's default SeaBIOS

ACCEL=(-accel tcg)
[ -w /dev/kvm ] && ACCEL=(-accel kvm -accel tcg)

DBG=()
[ "$DEBUG" = 1 ] && DBG=(-d int,cpu_reset -D "build/logs/$NAME.qemu.log")

LOG="build/logs/$NAME.serial.log"
BASE=(qemu-system-x86_64 -M q35 -cpu max -smp "$CPUS" -m "$MEM" "${ACCEL[@]}" "${FWARGS[@]}"
      -drive "file=$OVERLAY,format=qcow2,if=none,id=bootdisk" -device ide-hd,drive=bootdisk,bus=ide.0
      -device isa-debug-exit,iobase=0xf4,iosize=0x04 -no-reboot
      -monitor "unix:build/run/$NAME.monitor,server,nowait" "${DBG[@]}")

if [ "$GDBMODE" = 1 ]; then
    # shellcheck disable=SC2086
    "${BASE[@]}" -display none -serial "file:$LOG" -s -S $EXTRA < /dev/null &
    pid=$!
    echo "$pid" > "build/run/$NAME.pid"
    echo "QEMU started (pid $pid), paused, listening for gdb on :1234; serial log: $LOG"
    exit 0
fi

if [ "$INTERACTIVE" = 1 ]; then
    # shellcheck disable=SC2086
    exec "${BASE[@]}" -serial stdio $EXTRA
fi

# shellcheck disable=SC2086
timeout --foreground "$TIMEOUT" "${BASE[@]}" -display none -serial "file:$LOG" $EXTRA < /dev/null
code=$?

fails=$(grep -c '^KTEST FAIL' "$LOG" 2>/dev/null || true)
case $code in
    33)  echo "RESULT $NAME: PASS"; exit 0 ;;
    35)  echo "RESULT $NAME: FAIL ($fails failing ktests; see $LOG)" ;;
    124) echo "RESULT $NAME: HANG (no result after ${TIMEOUT}s; see $LOG)" ;;
    0)   echo "RESULT $NAME: CRASH (reset/triple fault or poweroff before reporting; rerun with --debug)" ;;
    *)   echo "RESULT $NAME: ERROR (qemu exit $code; see $LOG)" ;;
esac
grep '^KTEST FAIL' "$LOG" 2>/dev/null | head -n 10
exit 1
