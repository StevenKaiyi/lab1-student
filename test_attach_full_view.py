#!/usr/bin/env python3
import os
import pty
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


def send_input(sock: socket.socket, text: str) -> None:
    sock.sendall(pack_message(MSG_CLIENT_INPUT, 0, 0, text.encode()))


def read_output_until(sock: socket.socket, token: str, timeout: float = TIMEOUT) -> None:
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


def read_until_tokens(fd: int, tokens, timeout: float = TIMEOUT) -> bytes:
    deadline = time.time() + timeout
    data = bytearray()
    pending = [token for token in tokens]
    while time.time() < deadline:
        remaining = max(0.0, deadline - time.time())
        ready, _, _ = select.select([fd], [], [], remaining)
        if fd not in ready:
            continue
        chunk = os.read(fd, 4096)
        if not chunk:
            break
        data.extend(chunk)
        pending = [token for token in pending if token not in data]
        if not pending:
            return bytes(data)
    raise TestFailure(f"timeout waiting for attach view tokens: {pending}")


def main() -> int:
    if not BINARY.exists():
        print(f"missing binary: {BINARY}", file=sys.stderr)
        print("run `make` first", file=sys.stderr)
        return 2

    instance = "attach-full-view-test"
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

    control = None
    client_proc = None
    master_fd = -1
    try:
        wait_for_socket(instance)

        control = connect_client(instance)
        control.sendall(pack_message(MSG_CLIENT_RESIZE, 24, 80))

        status = send_command_wait_redraw(control, ":new", "focus=1 layout=0[12x80@0],1*[12x80@12]")
        if "ok: created pane 1" not in status:
            raise TestFailure(f"unexpected :new status: {status}")

        send_input(control, 'printf "__PANE1_SNAPSHOT__\\n"\n')
        read_output_until(control, '__PANE1_SNAPSHOT__')

        status = send_command_wait_redraw(control, ":focus 0", "focus=0 layout=0*[12x80@0],1[12x80@12]")
        if "ok: focused pane 0" not in status:
            raise TestFailure(f"unexpected :focus status: {status}")

        send_input(control, 'printf "__PANE0_SNAPSHOT__\\n"\n')
        read_output_until(control, '__PANE0_SNAPSHOT__')

        master_fd, slave_fd = pty.openpty()
        client_proc = subprocess.Popen(
            [str(BINARY), 'attach'],
            cwd=str(ROOT),
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            env=env,
            start_new_session=True,
        )
        os.close(slave_fd)
        slave_fd = -1

        view = read_until_tokens(
            master_fd,
            [
                b'mini-tmux: attached',
                b'Pane 0 [active]',
                b'Pane 1',
                b'__PANE0_SNAPSHOT__',
                b'__PANE1_SNAPSHOT__',
            ],
        )
        if b'__PANE0_SNAPSHOT__' not in view or b'__PANE1_SNAPSHOT__' not in view:
            raise TestFailure(f'attach client did not receive full pane snapshot: {view!r}')

        print('attach full-view checks passed')
        print('attach renders pane headers from redraw snapshot: ok')
        print('attach restores pane 0 text without new output: ok')
        print('attach restores pane 1 text without new output: ok')
        return 0
    except TestFailure as exc:
        print(f"TEST FAILED: {exc}", file=sys.stderr)
        return 1
    finally:
        if control is not None:
            control.close()
        if master_fd >= 0:
            try:
                os.close(master_fd)
            except OSError:
                pass
        if client_proc is not None and client_proc.poll() is None:
            try:
                os.killpg(client_proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                client_proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(client_proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                client_proc.wait(timeout=2.0)
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


if __name__ == '__main__':
    sys.exit(main())
