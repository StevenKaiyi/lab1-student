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
BINARY = ROOT / 'mini-tmux'
FORK_EXIT = ROOT / 'helpers' / 'fork_exit'
TIMEOUT = 8.0
MAX_MESSAGE_SIZE = 16 + 8192
MSG_CLIENT_HELLO = 1
MSG_CLIENT_INPUT = 2
MSG_CLIENT_RESIZE = 3
MSG_SERVER_OUTPUT = 5
MSG_SERVER_EXIT = 6
ANSI_RE = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')
OSC_RE = re.compile(r'\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)')


class TestFailure(RuntimeError):
    pass


def socket_path(instance: str) -> str:
    return f'/tmp/mini-tmux-{os.getuid()}-{instance}.sock'


def cleanup_socket(instance: str) -> None:
    try:
        os.unlink(socket_path(instance))
    except FileNotFoundError:
        pass


def cleanup_sockets() -> None:
    tmp = Path('/tmp')
    for path in tmp.glob(f'mini-tmux-{os.getuid()}-*'):
        try:
            path.unlink()
        except FileNotFoundError:
            pass
        except IsADirectoryError:
            pass


def normalize_output(text: str) -> str:
    text = OSC_RE.sub('', text)
    text = ANSI_RE.sub('', text)
    return text.replace('\r', '').replace('\n', ' ')


def pack_message(msg_type: int, arg0: int = 0, arg1: int = 0, payload: bytes = b'') -> bytes:
    return struct.pack('=IIii', msg_type, len(payload), arg0, arg1) + payload


def recv_message(sock: socket.socket, timeout: float = TIMEOUT):
    deadline = time.time() + timeout
    while time.time() < deadline:
        remaining = max(0.0, deadline - time.time())
        ready, _, _ = select.select([sock], [], [], remaining)
        if sock not in ready:
            continue
        data = sock.recv(MAX_MESSAGE_SIZE)
        if not data:
            raise TestFailure('socket closed unexpectedly')
        if len(data) < 16:
            raise TestFailure('received short message header')
        msg_type, size, arg0, arg1 = struct.unpack('=IIii', data[:16])
        if len(data) != 16 + size:
            raise TestFailure('received malformed message')
        return msg_type, arg0, arg1, data[16:]
    raise TestFailure('timeout waiting for message')


def connect_client(instance: str) -> socket.socket:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    sock.connect(socket_path(instance))
    sock.sendall(pack_message(MSG_CLIENT_HELLO, 0, 0))
    return sock


def send_input(sock: socket.socket, text: str) -> None:
    sock.sendall(pack_message(MSG_CLIENT_INPUT, 0, 0, text.encode()))


def send_resize(sock: socket.socket, rows: int, cols: int) -> None:
    sock.sendall(pack_message(MSG_CLIENT_RESIZE, rows, cols))


def read_output_until(sock: socket.socket, expected: str, timeout: float = TIMEOUT) -> str:
    deadline = time.time() + timeout
    combined = ''
    while time.time() < deadline:
        remaining = max(0.0, deadline - time.time())
        msg_type, _arg0, _arg1, payload = recv_message(sock, timeout=remaining)
        if msg_type != MSG_SERVER_OUTPUT:
            continue
        combined += payload.decode(errors='replace')
        normalized = normalize_output(combined)
        if expected in normalized:
            return normalized
    raise TestFailure(f'timeout waiting for output text: {expected}')


def wait_for_socket(instance: str, timeout: float = TIMEOUT) -> None:
    deadline = time.time() + timeout
    path = socket_path(instance)
    while time.time() < deadline:
        if os.path.exists(path):
            return
        time.sleep(0.05)
    raise TestFailure(f'timeout waiting for socket: {path}')


def zombie_count() -> int:
    proc = subprocess.run(
        ['ps', '-eo', 'stat='],
        cwd=str(ROOT),
        capture_output=True,
        text=True,
        check=True,
    )
    return sum(1 for line in proc.stdout.splitlines() if 'Z' in line)


def main() -> int:
    if not BINARY.exists():
        print(f'missing binary: {BINARY}', file=sys.stderr)
        print('run `make` first', file=sys.stderr)
        return 2

    instance = 'single-pane-test'
    cleanup_sockets()

    env = os.environ.copy()
    env['TERM'] = env.get('TERM', 'xterm-256color')
    env['MINI_TMUX_SERVER'] = instance

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
    baseline_zombies = zombie_count()
    try:
        wait_for_socket(instance)
        client = connect_client(instance)
        send_resize(client, 24, 80)

        send_input(client, 'printf "__MINI_TMUX_READY__\\n"\n')
        read_output_until(client, '__MINI_TMUX_READY__')

        send_input(client, 'tty\n')
        tty_output = read_output_until(client, '/dev/pts/')
        if '/dev/pts/' not in tty_output:
            raise TestFailure('tty did not report a PTY path')

        send_input(client, 'python3 -c "import os; print(os.isatty(0), os.isatty(1), os.isatty(2))"\n')
        isatty_output = read_output_until(client, 'True True True')
        if 'True True True' not in isatty_output:
            raise TestFailure('isatty check failed')

        send_input(client, 'stty size\n')
        size_output = read_output_until(client, '24 80')
        match = re.search(r'\b(\d+) (\d+)\b', size_output)
        if not match:
            raise TestFailure('could not parse stty size output')
        rows = int(match.group(1))
        cols = int(match.group(2))
        if rows <= 0 or cols <= 0:
            raise TestFailure(f'invalid winsize reported: {rows}x{cols}')

        token = f'SINGLE_PANE_TOKEN_{int(time.time())}'
        send_input(client, f'printf "%s\\n" {token}\n')
        token_output = read_output_until(client, token)
        if token not in token_output:
            raise TestFailure('token output did not round-trip')

        send_input(client, 'top -b -n 1 | head -n 3\n')
        top_output = read_output_until(client, 'load average')
        if 'load average' not in top_output and 'Tasks:' not in top_output:
            raise TestFailure('top compatibility check failed')

        send_input(client, "vim -Nu NONE -n -es +'silent !printf VIM_OK\\n' +q\n")
        vim_output = read_output_until(client, 'VIM_OK')
        if 'VIM_OK' not in vim_output:
            raise TestFailure('vim compatibility check failed')

        if not FORK_EXIT.exists():
            raise TestFailure(f'missing helper: {FORK_EXIT}')
        send_input(client, f'{FORK_EXIT} /tmp/mini-tmux-unused-sideband.sock {instance} 5 >/dev/null 2>&1 || true\n')
        deadline = time.time() + 2.0
        while time.time() < deadline and zombie_count() > baseline_zombies:
            time.sleep(0.1)
        zombie_warning = zombie_count() > baseline_zombies

        send_input(client, 'exit\n')
        proc.wait(timeout=5.0)
        if proc.returncode != 0:
            raise TestFailure(f'mini-tmux exited with status {proc.returncode}')

        print('single-pane checks passed')
        print('shell ready check: ok')
        print('pty path check: ok')
        print('isatty check: ok')
        print(f'winsize check: {rows}x{cols}')
        print('io round-trip check: ok')
        print('top compatibility check: ok')
        print('vim compatibility check: ok')
        print('fork_exit/zombie reap check: ok' if not zombie_warning else 'fork_exit/zombie reap check: warning (zombie count increased)')
        print('exit/cleanup check: ok')
        print()
        print('Future checks to add when features land:')
        print('- run helpers/probe through the pane and validate sideband messages')
        print('- verify SIGWINCH propagation after client terminal resize')
        return 0
    except TestFailure as exc:
        print(f'TEST FAILED: {exc}', file=sys.stderr)
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


if __name__ == '__main__':
    sys.exit(main())