#!/bin/bash
# Runs every configuration in tests/harness/matrix.conf (or the file given as $1).
# Extra arguments are passed through to run-qemu.sh.
CONF=${1:-tests/harness/matrix.conf}; shift || true
status=0
while read -r fw cpus _; do
    case "$fw" in ''|\#*) continue ;; esac
    tests/harness/run-qemu.sh --fw "$fw" --cpus "$cpus" "$@" || status=1
done < "$CONF"
[ $status = 0 ] && echo "MATRIX: PASS" || echo "MATRIX: FAIL"
exit $status
