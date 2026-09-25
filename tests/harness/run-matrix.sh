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
        tests/harness/run-qemu.sh --fw "$fw" --cpus "$cpus" --mem "$mem" \
            --name "${fw}-${cpus}cpu-${mem}m" "$@" || status=1
    else
        tests/harness/run-qemu.sh --fw "$fw" --cpus "$cpus" "$@" || status=1
    fi
done < "$CONF"
[ $status = 0 ] && echo "MATRIX: PASS" || echo "MATRIX: FAIL"
exit $status
