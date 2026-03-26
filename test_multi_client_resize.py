#!/usr/bin/env python3
import os
import select
import signal
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BINARY = ROOT / "mini-tmux"
TIMEOUT = 8.0
MAX_MESSAGE_SIZE = 16 + 8192

MSG_CLIENT_HELLO = 1
MSG_CLIENT_INPUT = 2
MSG_CLIENT_RESIZE = 3
MSG_CLIENT_COMMAND = 4
MSG_SERVER_OUTPUT = 5
MSG_SERVER_EXIT = 6
MSG_SERVER_REDRAW = 7
MSG_SERVER_STATUS = 8


class TestFailure(RuntimeError):
    pass


def socket_path(instance: str) -> str:
    return f"/tmp/mini-tmux-{os.getuid()}-{instance}.sock"


def cleanup_socket(instance: str) -> None:
    try:
        os.unlink(socket_path(instance))
    except FileNotFoundError:
        pass


def pack_message(msg_type: int, arg0: int = 0, arg1: int = 0, payload: bytes = b"") -> bytes:
    return struct.pack("=IIii", msg_type, len(payload), arg0, arg1) + payload


def recv_message(sock: socket.socket, timeout: float = TIMEOUT):
    deadline = time.time() + timeout
    while time.time() < deadline:
        remaining = max(0.0, deadline - time.time())
        ready, _, _ = select.select([sock], [], [], remaining)
        if sock not in ready:
            continue
        data = sock.recv(MAX_MESSAGE_SIZE)
        if not data:
            raise TestFailure("socket closed unexpectedly")
        if len(data) < 16:
            raise TestFailure("received short message header")
        msg_type, size, arg0, arg1 = struct.unpack("=IIii", data[:16])
        if len(data) != 16 + size:
            raise TestFailure("received malformed message")
        return msg_type, arg0, arg1, data[16:]
    raise TestFailure("timeout waiting for message")


def connect_client(instance: str) -> socket.socket:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    sock.connect(socket_path(instance))
    sock.sendall(pack_message(MSG_CLIENT_HELLO, 0, 0))
    return sock


def wait_for_socket(instance: str, timeout: float = TIMEOUT) -> None:
    deadline = time.time() + timeout
    path = socket_path(instance)
    while time.time() < deadline:
        if os.path.exists(path):
            return
        time.sleep(0.05)
    raise TestFailure(f"timeout waiting for socket: {path}")


def wait_for_redraw(sock: socket.socket, token: str, timeout: float = TIMEOUT) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        remaining = max(0.0, deadline - time.time())
        msg_type, _arg0, _arg1, payload = recv_message(sock, timeout=remaining)
        if msg_type == MSG_SERVER_REDRAW and token.encode() in payload:
            return
    raise TestFailure(f"timeout waiting for redraw token: {token}")


def send_input(sock: socket.socket, text: str) -> None:
    sock.sendall(pack_message(MSG_CLIENT_INPUT, 0, 0, text.encode()))


def read_output_until(sock: socket.socket, token: str, timeout: float = TIMEOUT) -> str:
    deadline = time.time() + timeout
    combined = ""
    while time.time() < deadline:
        remaining = max(0.0, deadline - time.time())
        msg_type, _arg0, _arg1, payload = recv_message(sock, timeout=remaining)
        if msg_type != MSG_SERVER_OUTPUT:
            continue
        combined += payload.decode(errors="replace")
        if token in combined:
            return combined
    raise TestFailure(f"timeout waiting for output token: {token}")


def main() -> int:
    if not BINARY.exists():
        print(f"missing binary: {BINARY}", file=sys.stderr)
        print("run `make` first", file=sys.stderr)
        return 2

    instance = "multi-client-resize-test"
    cleanup_socket(instance)

    env = os.environ.copy()
    env["TERM"] = env.get("TERM", "xterm-256color")
    env["MINI_TMUX_SERVER"] = instance

    proc = subprocess.Popen(
        [str(BINARY)],
        cwd=str(ROOT),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=env,
        start_new_session=True,
    )

    client_a = None
    client_b = None
    try:
        wait_for_socket(instance)

        client_a = connect_client(instance)
        client_a.sendall(pack_message(MSG_CLIENT_RESIZE, 30, 100))
        wait_for_redraw(client_a, "focus=0 layout=0*[30x100@0]")

        client_b = connect_client(instance)
        client_b.sendall(pack_message(MSG_CLIENT_RESIZE, 20, 70))
        wait_for_redraw(client_a, "focus=0 layout=0*[20x70@0]")
        wait_for_redraw(client_b, "focus=0 layout=0*[20x70@0]")

        send_input(client_a, 'stty size\n')
        size_output = read_output_until(client_a, '20 70')
        if '20 70' not in size_output:
            raise TestFailure(f'unexpected minimum winsize output: {size_output}')

        client_b.close()
        client_b = None
        wait_for_redraw(client_a, "focus=0 layout=0*[30x100@0]")

        send_input(client_a, 'stty size\n')
        size_output = read_output_until(client_a, '30 100')
        if '30 100' not in size_output:
            raise TestFailure(f'unexpected resized-after-detach output: {size_output}')

        print('multi-client resize checks passed')
        print('minimum winsize across attached clients: ok')
        print('remaining client winsize restored after detach: ok')
        return 0
    except TestFailure as exc:
        print(f"TEST FAILED: {exc}", file=sys.stderr)
        return 1
    finally:
        if client_a is not None:
            client_a.close()
        if client_b is not None:
            client_b.close()
        if proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.wait(timeout=2.0)
        cleanup_socket(instance)


if __name__ == "__main__":
    sys.exit(main())
