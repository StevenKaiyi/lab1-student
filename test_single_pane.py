#!/usr/bin/env python3
import os
import pty
import re
import select
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BINARY = ROOT / 'mini-tmux'
SOCKET_GLOB_PREFIX = f"/tmp/mini-tmux-{os.getuid()}-"
TIMEOUT = 8.0
PROMPT = b'__PANE_READY__# '

class TestFailure(RuntimeError):
    pass


def cleanup_sockets():
    tmp = Path('/tmp')
    for path in tmp.glob(f'mini-tmux-{os.getuid()}-*'):
        try:
            path.unlink()
        except FileNotFoundError:
            pass
        except IsADirectoryError:
            pass


def read_until(fd, predicate, timeout=TIMEOUT):
    deadline = time.time() + timeout
    data = bytearray()
    while time.time() < deadline:
        remaining = deadline - time.time()
        rlist, _, _ = select.select([fd], [], [], max(0.0, remaining))
        if fd not in rlist:
            continue
        chunk = os.read(fd, 4096)
        if not chunk:
            break
        data.extend(chunk)
        if predicate(bytes(data)):
            return bytes(data)
    raise TestFailure(f'timeout waiting for expected output; received:\n{data.decode(errors="replace")}')


def send_line(fd, line: str):
    os.write(fd, line.encode() + b'\n')


def main() -> int:
    if not BINARY.exists():
        print(f'missing binary: {BINARY}', file=sys.stderr)
        print('run `make` first', file=sys.stderr)
        return 2

    cleanup_sockets()

    env = os.environ.copy()
    env['PS1'] = PROMPT.decode()
    env['TERM'] = env.get('TERM', 'xterm-256color')
    env['MINI_TMUX_SERVER'] = 'single-pane-test'

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

    try:
        read_until(master_fd, lambda data: PROMPT in data)

        send_line(master_fd, 'tty')
        tty_output = read_until(master_fd, lambda data: PROMPT in data and b'/dev/pts/' in data)
        if b'/dev/pts/' not in tty_output:
            raise TestFailure('tty did not report a PTY path')

        send_line(master_fd, "python3 -c \"import os; print(os.isatty(0), os.isatty(1), os.isatty(2))\"")
        isatty_output = read_until(master_fd, lambda data: PROMPT in data and b'True True True' in data)
        if b'True True True' not in isatty_output:
            raise TestFailure('isatty check failed')

        send_line(master_fd, 'stty size')
        size_output = read_until(master_fd, lambda data: PROMPT in data and re.search(rb'\b\d+ \d+\b', data) is not None)
        match = re.search(rb'\b(\d+) (\d+)\b', size_output)
        if not match:
            raise TestFailure('could not parse stty size output')
        rows = int(match.group(1))
        cols = int(match.group(2))
        if rows <= 0 or cols <= 0:
            raise TestFailure(f'invalid winsize reported: {rows}x{cols}')

        token = f'SINGLE_PANE_TOKEN_{int(time.time())}'
        send_line(master_fd, f'printf "%s\\n" {token}')
        token_output = read_until(master_fd, lambda data: PROMPT in data and token.encode() in data)
        if token.encode() not in token_output:
            raise TestFailure('token output did not round-trip')

        send_line(master_fd, 'exit')
        try:
            read_until(master_fd, lambda data: False, timeout=1.0)
        except TestFailure:
            pass

        proc.wait(timeout=5.0)
        if proc.returncode != 0:
            raise TestFailure(f'mini-tmux exited with status {proc.returncode}')

        print('single-pane checks passed')
        print(f'pty path check: ok')
        print(f'isatty check: ok')
        print(f'winsize check: {rows}x{cols}')
        print('io round-trip check: ok')
        print('exit/cleanup check: ok')
        print()
        print('Future checks to add when features land:')
        print('- run helpers/probe through the pane and validate sideband messages')
        print('- verify SIGWINCH propagation after client terminal resize')
        print('- verify vim/top compatibility on the PTY path')
        print('- verify child reap behavior with helpers/fork_exit')
        return 0
    except TestFailure as exc:
        print(f'TEST FAILED: {exc}', file=sys.stderr)
        return 1
    finally:
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
        cleanup_sockets()


if __name__ == '__main__':
    sys.exit(main())
