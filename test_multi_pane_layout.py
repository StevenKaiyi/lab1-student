#!/usr/bin/env python3
import os
import re
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
ANSI_RE = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')
OSC_RE = re.compile(r'\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)')


class TestFailure(RuntimeError):
    pass


def normalize_output(text: str) -> str:
    text = OSC_RE.sub('', text)
    text = ANSI_RE.sub('', text)
    return text.replace('\r', '').replace('\n', ' ')


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


def recv_until(sock: socket.socket, predicate, timeout: float = TIMEOUT):
    deadline = time.time() + timeout
    messages = []
    while time.time() < deadline:
        remaining = max(0.0, deadline - time.time())
        message = recv_message(sock, timeout=remaining)
        messages.append(message)
        if predicate(message, messages):
            return message, messages
    raise TestFailure("timeout waiting for expected server message")


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


def wait_for_redraw(sock: socket.socket, token: str, timeout: float = TIMEOUT):
    def has_redraw(message, _messages):
        msg_type, _arg0, _arg1, payload = message
        return msg_type == MSG_SERVER_REDRAW and token.encode() in payload

    message, _ = recv_until(sock, has_redraw, timeout=timeout)
    return message


def send_resize_wait_redraw(sock: socket.socket, rows: int, cols: int, token: str,
                            timeout: float = TIMEOUT) -> None:
    sock.sendall(pack_message(MSG_CLIENT_RESIZE, rows, cols))
    wait_for_redraw(sock, token, timeout=timeout)


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


def send_input(sock: socket.socket, text: str) -> None:
    sock.sendall(pack_message(MSG_CLIENT_INPUT, 0, 0, text.encode()))


def read_output_until(sock: socket.socket, expected: str, timeout: float = TIMEOUT) -> str:
    deadline = time.time() + timeout
    combined = ""
    while time.time() < deadline:
        remaining = max(0.0, deadline - time.time())
        msg_type, _arg0, _arg1, payload = recv_message(sock, timeout=remaining)
        if msg_type != MSG_SERVER_OUTPUT:
            continue
        combined += payload.decode(errors="replace")
        normalized = normalize_output(combined)
        if expected in normalized:
            return normalized
    raise TestFailure(f"timeout waiting for output text: {expected}")


def assert_size_output(sock: socket.socket, expected: str) -> None:
    send_input(sock, 'stty size\n')
    output = read_output_until(sock, expected)
    if expected not in output:
        raise TestFailure(f"unexpected size output: {output}")


def main() -> int:
    if not BINARY.exists():
        print(f"missing binary: {BINARY}", file=sys.stderr)
        print("run `make` first", file=sys.stderr)
        return 2

    instance = "multi-pane-layout-test"
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
        send_resize_wait_redraw(client, 25, 80, "focus=0 layout=0*[25x80@0]")

        status = send_command_wait_redraw(client, ":new", "focus=1 layout=0[13x80@0],1*[12x80@13]")
        if "ok: created pane 1" not in status:
            raise TestFailure(f"unexpected first :new status: {status}")
        send_input(client, 'printf "__PANE1_READY__\\n"\n')
        read_output_until(client, '__PANE1_READY__')

        status = send_command_wait_redraw(client, ":new", "focus=2 layout=0[9x80@0],1[8x80@9],2*[8x80@17]")
        if "ok: created pane 2" not in status:
            raise TestFailure(f"unexpected second :new status: {status}")
        send_input(client, 'printf "__PANE2_READY__\\n"\n')
        read_output_until(client, '__PANE2_READY__')

        assert_size_output(client, "8 80")

        status = send_command_wait_redraw(client, ":focus 0", "focus=0 layout=0*[9x80@0],1[8x80@9],2[8x80@17]")
        if "ok: focused pane 0" not in status:
            raise TestFailure(f"unexpected :focus 0 status: {status}")
        assert_size_output(client, "9 80")

        send_resize_wait_redraw(client, 31, 90, "focus=0 layout=0*[11x90@0],1[10x90@11],2[10x90@21]")
        assert_size_output(client, "11 90")

        status = send_command_wait_redraw(client, ":focus 2", "focus=2 layout=0[11x90@0],1[10x90@11],2*[10x90@21]")
        if "ok: focused pane 2" not in status:
            raise TestFailure(f"unexpected :focus 2 status: {status}")
        assert_size_output(client, "10 90")

        print("multi-pane layout checks passed")
        print("initial single-pane redraw check: ok")
        print("two-pane vertical split check: ok")
        print("three-pane vertical split check: ok")
        print("pane 2 winsize check: ok")
        print("focus pane 0 check: ok")
        print("pane 0 winsize check: ok")
        print("resize redraw check: ok")
        print("resized pane winsize check: ok")
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