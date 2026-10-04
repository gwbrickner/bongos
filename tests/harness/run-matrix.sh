#!/bin/bash
# Runs every configuration in tests/harness/matrix.conf (or the file given as $1).
# Each row is "<firmware> <cpus> [memMiB]" (D-084) -- the memory column is optional and, when
# given, both selects run-qemu.sh's --mem and adds a "-<mem>m" suffix to the log name, so a row
# like "uefi 1 3072" doesn't collide with plain "uefi 1"'s uefi-1cpu.serial.log.
# Extra arguments are passed through to run-qemu.sh.
CONF=${1:-tests/harness/matrix.conf}; shift || true
status=0
while read -r fw cpus mem; do
    case "$fw" in ''|\#*) continue ;; esac
    if [ -n "$mem" ]; then
        # A 3 GiB guest spends minutes in the pmm's setup and the ktests' page poisoning under TCG
        # (about 135 s here, against run-qemu.sh's 120 s default): rows from 2 GiB up get a longer
        # HANG timeout. Only the detection of a real hang is slower; a caller's own --timeout (it
        # comes later on the command line) still wins.
        extra=()
        [ "$mem" -ge 2048 ] && extra=(--timeout 400)
        tests/harness/run-qemu.sh --fw "$fw" --cpus "$cpus" --mem "$mem" \
            --name "${fw}-${cpus}cpu-${mem}m" "${extra[@]}" "$@" || status=1
    else
        tests/harness/run-qemu.sh --fw "$fw" --cpus "$cpus" "$@" || status=1
    fi
done < "$CONF"
[ $status = 0 ] && echo "MATRIX: PASS" || echo "MATRIX: FAIL"
exit $status
