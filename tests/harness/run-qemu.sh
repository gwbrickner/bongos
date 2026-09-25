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
  --expect-serial S banner-match mode: PASS once every given S has appeared on serial, instead of
                     waiting on the isa-debug-exit/KTEST protocol (for milestones before the
                     kernel exists, e.g. M1.2's loader, which only prints and halts). Repeatable.
  --script FILE     GUI-test mode (D-070): starts QEMU paused (-S) with the serial port and QMP
                     on unix sockets, then hands off to tests/harness/qemu-script.py to run FILE
                     (expect/screendump/send steps) before letting the guest (ktest=all) drive
                     QEMU to its own isa-debug-exit, mapped to PASS/FAIL/HANG as usual.
                     Screendumps land in build/shots/NAME-<step-name>.ppm.
Monitor socket: build/run/NAME.monitor (screendump, sendkey, system_powerdown).
Serial log:     build/logs/NAME.serial.log
USAGE
}
IMAGE=build/bongos.img FW=uefi CPUS=1 MEM=512 TIMEOUT=120 NAME="" DEBUG=0 INTERACTIVE=0 GDBMODE=0 EXTRA="" SCRIPT=""
EXPECT_PATTERNS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --image) IMAGE=$2; shift 2 ;; --fw) FW=$2; shift 2 ;; --cpus) CPUS=$2; shift 2 ;;
        --mem) MEM=$2; shift 2 ;; --timeout) TIMEOUT=$2; shift 2 ;; --name) NAME=$2; shift 2 ;;
        --debug) DEBUG=1; shift ;; --interactive) INTERACTIVE=1; shift ;; --gdb) GDBMODE=1; shift ;;
        --extra) EXTRA=$2; shift 2 ;; --expect-serial) EXPECT_PATTERNS+=("$2"); shift 2 ;;
        --script) SCRIPT=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
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
    # -daemonize detaches QEMU into its own session, so Ctrl-C inside gdb (which targets the
    # foreground process group) can't also kill QEMU the way a plain backgrounded `&` would.
    # shellcheck disable=SC2086
    "${BASE[@]}" -display none -serial "file:$LOG" -s -S -daemonize \
        -pidfile "build/run/$NAME.pid" $EXTRA < /dev/null
    echo "QEMU started (pid $(cat "build/run/$NAME.pid")), paused, listening for gdb on :1234; serial log: $LOG"
    exit 0
fi

if [ "$INTERACTIVE" = 1 ]; then
    # shellcheck disable=SC2086
    exec "${BASE[@]}" -serial stdio $EXTRA
fi

if [ -n "$SCRIPT" ]; then
    # GUI-test mode (D-070): QEMU starts paused so qemu-script.py can connect both sockets before
    # any guest code runs; the script itself issues `cont`. Serial goes over a chardev socket
    # (not `-serial file:`, which is output-only and can't carry the menu key presses `send`
    # steps write) alongside the usual HMP `-monitor` and the new QMP socket `screendump` needs.
    : > "$LOG"
    SERIAL_SOCK="build/run/$NAME.serial.sock"
    QMP_SOCK="build/run/$NAME.qmp"
    rm -f "$SERIAL_SOCK" "$QMP_SOCK"
    SHOTS_DIR="build/shots"
    mkdir -p "$SHOTS_DIR"
    # shellcheck disable=SC2086
    "${BASE[@]}" -display none -S \
        -chardev "socket,id=ser0,path=$SERIAL_SOCK,server=on,wait=off" -serial chardev:ser0 \
        -qmp "unix:$QMP_SOCK,server=on,wait=off" \
        $EXTRA < /dev/null &
    qemupid=$!
    trap 'kill "$qemupid" 2>/dev/null' EXIT

    python3 tests/harness/qemu-script.py --serial "$SERIAL_SOCK" --qmp "$QMP_SOCK" \
        --log "$LOG" --shots "$SHOTS_DIR" --prefix "$FW" --timeout "$TIMEOUT" "$SCRIPT"
    scriptStatus=$?

    # The script's own steps are done; the guest's `ktest=all` cmdline still needs to run its
    # ktests and drive QEMU to isa-debug-exit on its own, within whatever's left of the timeout.
    SECONDS=0
    while kill -0 "$qemupid" 2>/dev/null && [ "$SECONDS" -lt "$TIMEOUT" ]; do
        sleep 0.2
    done
    if kill -0 "$qemupid" 2>/dev/null; then
        kill "$qemupid" 2>/dev/null
        wait "$qemupid" 2>/dev/null
        qemuExit=124
    else
        wait "$qemupid"
        qemuExit=$?
    fi
    trap - EXIT

    if [ "$scriptStatus" -ne 0 ]; then
        echo "RESULT $NAME: ERROR (script $SCRIPT failed; see above and $LOG)"
        exit 1
    fi

    fails=$(grep -c '^KTEST FAIL' "$LOG" 2>/dev/null || true)
    case $qemuExit in
        33)  echo "RESULT $NAME: PASS"; exit 0 ;;
        35)  echo "RESULT $NAME: FAIL ($fails failing ktests; see $LOG)" ;;
        124) echo "RESULT $NAME: HANG (qemu did not exit after the script completed; see $LOG)" ;;
        0)   echo "RESULT $NAME: CRASH (reset/triple fault or poweroff before reporting; see $LOG)" ;;
        *)   echo "RESULT $NAME: ERROR (qemu exit $qemuExit; see $LOG)" ;;
    esac
    grep '^KTEST FAIL' "$LOG" 2>/dev/null | head -n 10
    exit 1
fi

if [ "${#EXPECT_PATTERNS[@]}" -gt 0 ]; then
    # Banner-match mode: no isa-debug-exit/reboot signal exists yet (the kernel that drives it
    # arrives in M1.3), so success is "every expected string showed up on serial before the
    # timeout or the process exiting" rather than a QEMU exit code.
    : > "$LOG"
    # shellcheck disable=SC2086
    "${BASE[@]}" -display none -serial "file:$LOG" $EXTRA < /dev/null &
    qemupid=$!
    trap 'kill "$qemupid" 2>/dev/null' EXIT

    allSeen() {
        local pattern
        for pattern in "${EXPECT_PATTERNS[@]}"; do
            grep -qF -- "$pattern" "$LOG" 2>/dev/null || return 1
        done
        return 0
    }

    SECONDS=0
    result=HANG
    qemuExit=""
    while [ "$SECONDS" -lt "$TIMEOUT" ]; do
        if allSeen; then
            result=PASS
            break
        fi
        if ! kill -0 "$qemupid" 2>/dev/null; then
            # qemu exited: one last check before declaring failure, since it may have flushed
            # the final serial bytes after this loop's last read but before exiting.
            wait "$qemupid"; qemuExit=$?
            if allSeen; then
                result=PASS
            else
                result=CRASH
            fi
            break
        fi
        sleep 0.2
    done
    kill "$qemupid" 2>/dev/null
    wait "$qemupid" 2>/dev/null
    trap - EXIT

    case "$result" in
        PASS)
            echo "RESULT $NAME: PASS"; exit 0 ;;
        CRASH)
            if [ -n "$qemuExit" ] && [ "$qemuExit" != 0 ] && [ ! -s "$LOG" ]; then
                echo "RESULT $NAME: ERROR (qemu exited $qemuExit before producing any serial output; see $LOG)"
            else
                missing=""
                for pattern in "${EXPECT_PATTERNS[@]}"; do
                    grep -qF -- "$pattern" "$LOG" 2>/dev/null || missing="$missing \"$pattern\""
                done
                echo "RESULT $NAME: CRASH (qemu exited before$missing appeared on serial; see $LOG)"
            fi
            exit 1 ;;
        *)
            missing=""
            for pattern in "${EXPECT_PATTERNS[@]}"; do
                grep -qF -- "$pattern" "$LOG" 2>/dev/null || missing="$missing \"$pattern\""
            done
            echo "RESULT $NAME: HANG ($missing not seen on serial after ${TIMEOUT}s; see $LOG)"
            exit 1 ;;
    esac
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
