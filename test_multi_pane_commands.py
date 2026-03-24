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


def connect_client(instance: str, read_only: bool = False) -> socket.socket:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    sock.connect(socket_path(instance))
    sock.sendall(pack_message(MSG_CLIENT_HELLO, 1 if read_only else 0, 0))
    return sock


def send_input(sock: socket.socket, text: str) -> None:
    sock.sendall(pack_message(MSG_CLIENT_INPUT, 0, 0, text.encode()))


def send_command(sock: socket.socket, command: str) -> str:
    sock.sendall(pack_message(MSG_CLIENT_COMMAND, 0, 0, command.encode()))

    def is_status(message, _messages):
        msg_type, _arg0, _arg1, _payload = message
        return msg_type == MSG_SERVER_STATUS

    message, _ = recv_until(sock, is_status)
    return message[3].decode(errors="replace")


def wait_for_output(sock: socket.socket, token: str, timeout: float = TIMEOUT) -> None:
    def has_token(message, _messages):
        msg_type, _arg0, _arg1, payload = message
        return msg_type == MSG_SERVER_OUTPUT and token.encode() in payload

    recv_until(sock, has_token, timeout=timeout)


def wait_for_exit(sock: socket.socket, timeout: float = TIMEOUT):
    def is_exit(message, _messages):
        msg_type, _arg0, _arg1, _payload = message
        return msg_type == MSG_SERVER_EXIT

    message, _ = recv_until(sock, is_exit, timeout=timeout)
    return message[1], message[2]


def wait_for_socket(instance: str, timeout: float = TIMEOUT) -> None:
    deadline = time.time() + timeout
    path = socket_path(instance)
    while time.time() < deadline:
        if os.path.exists(path):
            return
        time.sleep(0.05)
    raise TestFailure(f"timeout waiting for socket: {path}")


def drain_fd(fd: int, duration: float = 0.3) -> bytes:
    deadline = time.time() + duration
    data = bytearray()
    while time.time() < deadline:
        remaining = max(0.0, deadline - time.time())
        ready, _, _ = select.select([fd], [], [], remaining)
        if fd not in ready:
            continue
        chunk = os.read(fd, 4096)
        if not chunk:
            break
        data.extend(chunk)
    return bytes(data)


def main() -> int:
    if not BINARY.exists():
        print(f"missing binary: {BINARY}", file=sys.stderr)
        print("run `make` first", file=sys.stderr)
        return 2

    instance = "multi-pane-command-test"
    cleanup_socket(instance)

    env = os.environ.copy()
    env["TERM"] = env.get("TERM", "xterm-256color")
    env["MINI_TMUX_SERVER"] = instance

    master_fd, slave_fd = pty.openpty()
    proc = subprocess.Popen(
        [str(BINARY)],
        cwd=str(ROOT),
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=slave_fd,
        env=env,
        start_new_session=True,
    )
    os.close(slave_fd)

    client = None
    ro_client = None
    try:
        wait_for_socket(instance)
        drain_fd(master_fd, duration=0.8)

        client = connect_client(instance, read_only=False)
        client.sendall(pack_message(MSG_CLIENT_RESIZE, 24, 80))

        send_input(client, 'printf "__PANE0_READY__\\n"\n')
        wait_for_output(client, "__PANE0_READY__")

        status = send_command(client, ":new")
        if "ok: created pane 1" not in status:
            raise TestFailure(f"unexpected :new status: {status}")

        send_input(client, 'printf "__PANE1_FOCUSED__\\n"\n')
        wait_for_output(client, "__PANE1_FOCUSED__")

        status = send_command(client, ":focus 0")
        if "ok: focused pane 0" not in status:
            raise TestFailure(f"unexpected :focus status: {status}")

        send_input(client, 'printf "__PANE0_FOCUSED__\\n"\n')
        wait_for_output(client, "__PANE0_FOCUSED__")

        ro_client = connect_client(instance, read_only=True)
        ro_status = send_command(ro_client, ":new")
        if "error: read-only clients cannot run :new" not in ro_status:
            raise TestFailure(f"unexpected read-only :new status: {ro_status}")

        status = send_command(client, ":kill 1")
        if "ok: terminating pane 1" not in status:
            raise TestFailure(f"unexpected :kill 1 status: {status}")

        send_input(client, 'printf "__PANE0_STILL_ALIVE__\\n"\n')
        wait_for_output(client, "__PANE0_STILL_ALIVE__")

        status = send_command(client, ":kill 0")
        if "ok: terminating pane 0" not in status:
            raise TestFailure(f"unexpected :kill 0 status: {status}")

        exit_code, exit_signal = wait_for_exit(client)
        if exit_code != 129 or exit_signal != 1:
            raise TestFailure(f"unexpected server exit tuple: code={exit_code} signal={exit_signal}")

        proc.wait(timeout=5.0)
        print("multi-pane command checks passed")
        print("pane 0 ready check: ok")
        print("create pane check: ok")
        print("new pane focused check: ok")
        print("focus pane 0 check: ok")
        print("read-only command rejection check: ok")
        print("kill secondary pane check: ok")
        print("remaining pane stays interactive check: ok")
        print("last pane exit propagates check: ok")
        return 0
    except TestFailure as exc:
        print(f"TEST FAILED: {exc}", file=sys.stderr)
        return 1
    finally:
        if client is not None:
            client.close()
        if ro_client is not None:
            ro_client.close()
        try:
            os.close(master_fd)
        except OSError:
            pass
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
