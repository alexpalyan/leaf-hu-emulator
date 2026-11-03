#!/usr/bin/env python3
"""AT command and transparent data proxy for the QY8 CDC ACM TCU model."""

import argparse
import json
import select
import socket
import time


ESCAPE = b"+++"
GUARD_SECONDS = 1.0


def qmp_set_serial_state(path):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as qmp:
        qmp.connect(path)
        stream = qmp.makefile("rwb", buffering=0)
        stream.readline()
        stream.write(b'{"execute":"qmp_capabilities"}\r\n')
        stream.readline()
        command = {
            "execute": "qom-set",
            "arguments": {
                "path": "/machine/peripheral/qy8-tcu",
                "property": "serial-state",
                "value": 3,
            },
        }
        stream.write((json.dumps(command) + "\r\n").encode())
        reply = json.loads(stream.readline())
        if "error" in reply:
            raise RuntimeError(reply["error"])


def response(command):
    upper = command.upper()
    if upper in ("ATE0", "ATE0&C1&D2", "ATX4"):
        return b"\r\nOK\r\n", False
    if upper == "AT+CMER?" or upper.startswith("AT+CMER="):
        return b"\r\nOK\r\n", False
    if upper == "AT+CNUM":
        return b"\r\nOK\r\n", False
    if upper == "ATS7=60":
        return b"\r\nOK\r\n", False
    if upper == "ATI":
        return b"\r\nTCU Gen2 Kai CX.6_RC01\r\nOK\r\n", False
    if upper == "AT+CGDCONT=?":
        return b'\r\n+CGDCONT: (1-3),("IP","IPV6","IPV4V6")\r\nOK\r\n', False
    if upper == "AT+CGDCONT?":
        return b'\r\n+CGDCONT: 1,"IP","internet","0.0.0.0",0,0\r\nOK\r\n', False
    if upper.startswith("AT+CGDCONT=") or upper == "AT":
        return b"\r\nOK\r\n", False
    if upper == "ATH":
        return b"\r\nOK\r\n", False
    if upper.startswith("ATD*99***") and upper.endswith("#"):
        return b"\r\nCONNECT\r\n", True
    return b"\r\nERROR\r\n", False


def log_event(log, event, **fields):
    log.write(json.dumps({"event": event, **fields}, sort_keys=True) + "\n")
    log.flush()


def forward_data(conn, endpoint, log, guard_seconds):
    """Forward raw PPP bytes and consume Hayes escape only after its guard time."""
    pending_escape = bytearray()
    candidate_deadline = None
    last_to_endpoint = time.monotonic()
    last_from_endpoint = last_to_endpoint

    while True:
        timeout = None
        if candidate_deadline is not None:
            timeout = max(0, candidate_deadline - time.monotonic())
        readers = [conn] if endpoint is None else [conn, endpoint]
        readable, _, _ = select.select(readers, [], [], timeout)
        now = time.monotonic()

        if candidate_deadline is not None and now >= candidate_deadline:
            if (pending_escape == ESCAPE and
                    (endpoint is None or endpoint not in readable) and
                    now - last_from_endpoint >= guard_seconds):
                log_event(log, "escape", sequence="+++",
                          guard_seconds=guard_seconds)
                return True
            if endpoint and pending_escape:
                endpoint.sendall(pending_escape)
            elif pending_escape:
                log_event(log, "unpaired_data",
                          hex=bytes(pending_escape).hex())
            last_to_endpoint = now
            pending_escape.clear()
            candidate_deadline = None

        if conn in readable:
            data = conn.recv(4096)
            if not data:
                return False
            outgoing = bytearray()
            cursor = 0
            while cursor < len(data):
                byte = data[cursor:cursor + 1]
                cursor += 1
                if pending_escape:
                    if byte == b"+" and len(pending_escape) < len(ESCAPE):
                        pending_escape.extend(byte)
                        candidate_deadline = time.monotonic() + guard_seconds
                        continue
                    outgoing.extend(pending_escape)
                    pending_escape.clear()
                    candidate_deadline = None

                if (byte == b"+" and
                        time.monotonic() - last_to_endpoint >= guard_seconds and
                        time.monotonic() - last_from_endpoint >= guard_seconds):
                    pending_escape.extend(byte)
                    candidate_deadline = time.monotonic() + guard_seconds
                else:
                    outgoing.extend(byte)
            if outgoing:
                if endpoint:
                    endpoint.sendall(outgoing)
                else:
                    log_event(log, "unpaired_data", hex=bytes(outgoing).hex())
                last_to_endpoint = time.monotonic()

        if endpoint and endpoint in readable:
            data = endpoint.recv(4096)
            if not data:
                raise ConnectionError("PPP data endpoint closed")
            if pending_escape:
                endpoint.sendall(pending_escape)
                pending_escape.clear()
                candidate_deadline = None
            last_from_endpoint = time.monotonic()
            conn.sendall(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True, help="QEMU chardev Unix socket")
    parser.add_argument("--transcript", required=True, help="JSONL AT/data transcript")
    parser.add_argument("--data-endpoint",
                        help="Unix socket used to reach the PPP peer")
    parser.add_argument("--data-socket",
                        help="local Unix socket for the PPP peer to connect")
    parser.add_argument("--escape-guard", type=float, default=GUARD_SECONDS,
                        help="Hayes escape quiet time in seconds (default: 1)")
    parser.add_argument("--serial-state-qmp", help="QMP socket for DCD/DSR")
    args = parser.parse_args()

    if args.data_endpoint and args.data_socket:
        parser.error("choose either --data-endpoint or --data-socket")
    endpoint = None
    listener = None
    if args.data_endpoint:
        endpoint = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        endpoint.connect(args.data_endpoint)
    elif args.data_socket:
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(args.data_socket)
        listener.listen(1)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn, \
            open(args.transcript, "a", encoding="utf-8") as log:
        while True:
            try:
                conn.connect(args.socket)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.1)
        pending = bytearray()
        while True:
            data = conn.recv(4096)
            if not data:
                if endpoint:
                    endpoint.close()
                if listener:
                    listener.close()
                return
            pending.extend(data)
            while b"\r" in pending:
                raw, _, rest = pending.partition(b"\r")
                pending = bytearray(rest)
                command = raw.decode("ascii", errors="replace").strip("\n")
                reply, entering_data = response(command)
                log_event(log, "command", command=command,
                          reply_hex=reply.hex())
                conn.sendall(reply)
                if entering_data:
                    if args.serial_state_qmp:
                        qmp_set_serial_state(args.serial_state_qmp)
                        log_event(log, "serial_state", dcd=1, dsr=1)
                    if pending and endpoint:
                        log_event(log, "early_data", hex=bytes(pending).hex())
                        endpoint.sendall(pending)
                    log_event(log, "data_mode", endpoint=args.data_endpoint or
                              args.data_socket)
                    if listener:
                        endpoint, _ = listener.accept()
                        listener.close()
                        listener = None
                    escaped = forward_data(conn, endpoint, log,
                                           args.escape_guard)
                    if not escaped:
                        return
                    break
        if endpoint:
            endpoint.close()


if __name__ == "__main__":
    main()
