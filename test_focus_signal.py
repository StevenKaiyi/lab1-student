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


def send_input(sock: socket.socket, data: bytes) -> None:
    sock.sendall(pack_message(MSG_CLIENT_INPUT, 0, 0, data))


def wait_for_output(sock: socket.socket, token: str, timeout: float = TIMEOUT) -> None:
    deadline = time.time() + timeout
    combined = ""
    while time.time() < deadline:
        remaining = max(0.0, deadline - time.time())
        msg_type, _arg0, _arg1, payload = recv_message(sock, timeout=remaining)
        if msg_type != MSG_SERVER_OUTPUT:
            continue
        combined += payload.decode(errors="replace")
        if token in combined:
            return
    raise TestFailure(f"timeout waiting for output token: {token}")


def ensure_output_absent(sock: socket.socket, token: str, duration: float = 0.6) -> None:
    deadline = time.time() + duration
    combined = ""
    while time.time() < deadline:
        remaining = max(0.0, deadline - time.time())
        ready, _, _ = select.select([sock], [], [], remaining)
        if sock not in ready:
            continue
        msg_type, _arg0, _arg1, payload = recv_message(sock, timeout=max(0.1, remaining))
        if msg_type != MSG_SERVER_OUTPUT:
            continue
        combined += payload.decode(errors="replace")
        if token in combined:
            raise TestFailure(f'unexpected output token appeared: {token}')


def send_command_wait_redraw(sock: socket.socket, command: str, redraw_token: str,
                             timeout: float = TIMEOUT) -> str:
    sock.sendall(pack_message(MSG_CLIENT_COMMAND, 0, 0, command.encode()))
    deadline = time.time() + timeout
    status = None
    while time.time() < deadline:
        remaining = max(0.0, deadline - time.time())
        msg_type, _arg0, _arg1, payload = recv_message(sock, timeout=remaining)
        if msg_type == MSG_SERVER_STATUS:
            status = payload.decode(errors="replace")
            continue
        if msg_type == MSG_SERVER_REDRAW and redraw_token.encode() in payload and status is not None:
            return status
    raise TestFailure(f"timeout waiting for status+redraw for command: {command}")


def main() -> int:
    if not BINARY.exists():
        print(f"missing binary: {BINARY}", file=sys.stderr)
        print("run `make` first", file=sys.stderr)
        return 2

    instance = "focus-signal-test"
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

    client = None
    try:
        wait_for_socket(instance)

        client = connect_client(instance)
        client.sendall(pack_message(MSG_CLIENT_RESIZE, 24, 80))

        status = send_command_wait_redraw(client, ":new", "focus=1 layout=0[12x80@0],1*[12x80@12]")
        if "ok: created pane 1" not in status:
            raise TestFailure(f"unexpected :new status: {status}")

        send_input(client, b"stty -echo\n")
        send_input(client, ("python3 -c 'import signal,time; signal.signal(signal.SIGINT, lambda *args: print(\"__P1_INT__\", flush=True)); signal.signal(signal.SIGTSTP, lambda *args: print(\"__P1_TSTP__\", flush=True)); print(\"__P1_READY__\", flush=True); signal.pause()'\n").encode())
        wait_for_output(client, '__P1_READY__')

        status = send_command_wait_redraw(client, ":focus 0", "focus=0 layout=0*[12x80@0],1[12x80@12]")
        if "ok: focused pane 0" not in status:
            raise TestFailure(f"unexpected :focus 0 status: {status}")

        send_input(client, b"stty -echo\n")
        send_input(client, ("python3 -c 'import signal,time; signal.signal(signal.SIGINT, lambda *args: print(\"__P0_INT__\", flush=True)); signal.signal(signal.SIGTSTP, lambda *args: print(\"__P0_TSTP__\", flush=True)); print(\"__P0_READY__\", flush=True); signal.pause()'\n").encode())
        wait_for_output(client, '__P0_READY__')

        send_input(client, b"\x03")
        wait_for_output(client, '__P0_INT__')
        ensure_output_absent(client, '__P1_INT__')

        status = send_command_wait_redraw(client, ":focus 1", "focus=1 layout=0[12x80@0],1*[12x80@12]")
        if "ok: focused pane 1" not in status:
            raise TestFailure(f"unexpected :focus 1 status: {status}")

        send_input(client, b"\x1a")
        wait_for_output(client, '__P1_TSTP__')
        ensure_output_absent(client, '__P0_TSTP__')

        print('focus signal checks passed')
        print('ctrl+c only reaches focused pane: ok')
        print('ctrl+z only reaches focused pane: ok')
        return 0
    except TestFailure as exc:
        print(f"TEST FAILED: {exc}", file=sys.stderr)
        return 1
    finally:
        if client is not None:
            client.close()
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
