#!/usr/bin/env python3
"""Drives a paused QEMU instance through a boot-menu test script over its serial and QMP unix
sockets (ARCHITECTURE §23, D-070). Invoked by tests/harness/run-qemu.sh --script FILE, never run
standalone in CI.

QEMU is started with `-S` (paused at reset) so this script can connect both sockets before any
guest code runs, then issues `cont` itself -- otherwise the loader's very first serial bytes (or
the boot menu itself, if the countdown is short) could race the connection.

Script grammar (one step per line, blank lines and '#'-comments ignored):
  expect <literal>    wait for <literal> to appear in the serial stream, searching forward from
                       the end of the previous match (so the same text can appear more than once
                       and each `expect` finds the next occurrence, not the first).
  screendump <name>   QMP `screendump` to <shots-dir>/<prefix>-<name>.ppm; waits for the QMP
                       command's own {"return": {}} reply (skipping any async "event" messages
                       QEMU may interleave), then sanity-checks the written PPM's header.
  send <text>         writes <text> to the serial socket after unescaping \\r \\n \\e \\\\ and
                       \\xHH -- OVMF's TerminalDxe decodes VT100 escapes (e.g. \\e[B for the down
                       arrow) the same way a real serial terminal would, and delivers them to
                       ConIn merged with the PS/2/USB keyboard (ARCHITECTURE §5.5, D-068).
"""
import argparse
import json
import os
import select
import socket
import sys
import time


class ScriptError(Exception):
    pass


def unescape(text):
    out = bytearray()
    i = 0
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text):
            nxt = text[i + 1]
            if nxt == "r":
                out.append(0x0D)
                i += 2
            elif nxt == "n":
                out.append(0x0A)
                i += 2
            elif nxt == "e":
                out.append(0x1B)
                i += 2
            elif nxt == "\\":
                out.append(0x5C)
                i += 2
            elif nxt == "x" and i + 3 < len(text):
                out.append(int(text[i + 2 : i + 4], 16))
                i += 4
            else:
                out.append(ord(c))
                i += 1
        else:
            out.extend(c.encode("ascii"))
            i += 1
    return bytes(out)


def connect_retry(path, timeout):
    deadline = time.monotonic() + timeout
    last_err = None
    while time.monotonic() < deadline:
        if os.path.exists(path):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                return s
            except OSError as e:
                last_err = e
        time.sleep(0.05)
    raise ScriptError(f"could not connect to {path}: {last_err}")


class Qmp:
    def __init__(self, sock):
        self.sock = sock
        self.buf = b""
        self.sock.settimeout(10)

    def _read_line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ScriptError("QMP socket closed unexpectedly")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode("utf-8"))

    def handshake(self):
        greeting = self._read_line()
        if "QMP" not in greeting:
            raise ScriptError(f"unexpected QMP greeting: {greeting}")
        self._send({"execute": "qmp_capabilities"})
        self._expect_return()

    def _send(self, obj):
        self.sock.sendall((json.dumps(obj) + "\n").encode("utf-8"))

    def _expect_return(self):
        while True:
            msg = self._read_line()
            if "return" in msg:
                return msg["return"]
            if "error" in msg:
                raise ScriptError(f"QMP command failed: {msg['error']}")
            # else: an async event ("event": ...) -- ignore and keep reading.

    def execute(self, command, **arguments):
        self._send({"execute": command, "arguments": arguments} if arguments else {"execute": command})
        return self._expect_return()


class SerialLog:
    """Tees every byte read from the serial socket into `logf`, and keeps a growing in-memory
    buffer `expect()` searches (only ever appended to -- the search position only moves forward,
    matching the grammar's "next occurrence" rule)."""

    def __init__(self, sock, logf):
        self.sock = sock
        self.logf = logf
        self.buf = ""
        self.search_from = 0

    def drain(self, timeout):
        r, _, _ = select.select([self.sock], [], [], timeout)
        if not r:
            return False
        chunk = self.sock.recv(65536)
        if not chunk:
            return False
        text = chunk.decode("utf-8", errors="replace")
        self.logf.write(text)
        self.logf.flush()
        self.buf += text
        return True

    def expect(self, literal, timeout):
        deadline = time.monotonic() + timeout
        while True:
            idx = self.buf.find(literal, self.search_from)
            if idx >= 0:
                self.search_from = idx + len(literal)
                return
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ScriptError(f'expect "{literal}" timed out after {timeout}s')
            self.drain(min(remaining, 0.5))


def do_screendump(qmp, shots_dir, prefix, name):
    os.makedirs(shots_dir, exist_ok=True)
    path = os.path.join(shots_dir, f"{prefix}-{name}.ppm")
    qmp.execute("screendump", filename=path)
    if not os.path.exists(path):
        raise ScriptError(f"screendump {name}: {path} was not created")
    with open(path, "rb") as f:
        header_bytes = f.read(64)
    # Minimal PPM header sanity check: "P6\n<W> <H>\n255\n" then W*H*3 bytes.
    if not header_bytes.startswith(b"P6"):
        raise ScriptError(f"screendump {name}: {path} is not a P6 PPM")
    parts = header_bytes.split(None, 4)
    try:
        width, height, maxval = int(parts[1]), int(parts[2]), int(parts[3])
    except (IndexError, ValueError) as e:
        raise ScriptError(f"screendump {name}: malformed PPM header in {path}") from e
    if maxval != 255:
        raise ScriptError(f"screendump {name}: unexpected maxval {maxval} in {path}")
    # Recompute the exact header length actually used (varies with digit counts) by reformatting.
    header_str = f"P6\n{width} {height}\n255\n"
    expected_size = len(header_str.encode("ascii")) + width * height * 3
    actual_size = os.path.getsize(path)
    if actual_size != expected_size:
        raise ScriptError(
            f"screendump {name}: {path} is {actual_size} bytes, expected {expected_size} "
            f"({width}x{height})"
        )


def run_script(script_path, serial_sock_path, qmp_sock_path, log_path, shots_dir, prefix, timeout):
    with open(log_path, "a", buffering=1) as logf:
        serial_raw = connect_retry(serial_sock_path, timeout)
        qmp_raw = connect_retry(qmp_sock_path, timeout)
        qmp = Qmp(qmp_raw)
        qmp.handshake()
        qmp.execute("cont")

        serial = SerialLog(serial_raw, logf)

        with open(script_path) as f:
            lines = f.readlines()

        for lineno, raw_line in enumerate(lines, start=1):
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                if line.startswith("expect "):
                    serial.expect(line[len("expect ") :], timeout)
                elif line.startswith("screendump "):
                    do_screendump(qmp, shots_dir, prefix, line[len("screendump ") :].strip())
                elif line.startswith("send "):
                    serial_raw.sendall(unescape(line[len("send ") :]))
                else:
                    raise ScriptError(f"unrecognized step: {line}")
            except ScriptError as e:
                print(f"qemu-script.py: {script_path}:{lineno}: {e}", file=sys.stderr)
                return 1

        # Keep draining a little longer so the log captures whatever the guest prints right after
        # the last step (e.g. a kernel banner following the final menu selection), then let the
        # caller (run-qemu.sh) manage the QEMU process's own lifetime/exit-code handling.
        end = time.monotonic() + 2
        while time.monotonic() < end:
            if not serial.drain(0.2):
                break
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", required=True, help="path to the serial chardev unix socket")
    ap.add_argument("--qmp", required=True, help="path to the QMP unix socket")
    ap.add_argument("--log", required=True, help="serial log file to append to")
    ap.add_argument("--shots", required=True, help="directory screendumps are written into")
    ap.add_argument("--prefix", required=True, help="screendump filename prefix (e.g. the firmware)")
    ap.add_argument("--timeout", type=float, default=60, help="per-step timeout in seconds")
    ap.add_argument("script", help="the script file to run")
    args = ap.parse_args()

    try:
        return run_script(
            args.script, args.serial, args.qmp, args.log, args.shots, args.prefix, args.timeout
        )
    except ScriptError as e:
        print(f"qemu-script.py: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
