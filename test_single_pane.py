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
FORK_EXIT = ROOT / 'helpers' / 'fork_exit'
TIMEOUT = 8.0
READY_TOKEN = '__MINI_TMUX_READY__'

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


def read_briefly(fd, duration=0.3):
    deadline = time.time() + duration
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
    return bytes(data)


def send_line(fd, line: str):
    os.write(fd, line.encode() + b'\n')


def wait_for_shell(fd):
    send_line(fd, f'printf "{READY_TOKEN}\\n"')
    output = read_until(fd, lambda data: READY_TOKEN.encode() in data)
    return output


def assert_no_zombies():
    proc = subprocess.run(
        ['ps', '-eo', 'stat='],
        cwd=str(ROOT),
        capture_output=True,
        text=True,
        check=True,
    )
    zombie_lines = [line.strip() for line in proc.stdout.splitlines() if 'Z' in line]
    if zombie_lines:
        raise TestFailure(f'zombie processes detected: {zombie_lines}')


def main() -> int:
    if not BINARY.exists():
        print(f'missing binary: {BINARY}', file=sys.stderr)
        print('run `make` first', file=sys.stderr)
        return 2

    cleanup_sockets()

    env = os.environ.copy()
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
        read_briefly(master_fd, duration=0.8)
        wait_for_shell(master_fd)

        send_line(master_fd, 'tty')
        tty_output = read_until(master_fd, lambda data: b'/dev/pts/' in data)
        if b'/dev/pts/' not in tty_output:
            raise TestFailure('tty did not report a PTY path')

        send_line(master_fd, "python3 -c \"import os; print(os.isatty(0), os.isatty(1), os.isatty(2))\"")
        isatty_output = read_until(master_fd, lambda data: b'True True True' in data)
        if b'True True True' not in isatty_output:
            raise TestFailure('isatty check failed')

        send_line(master_fd, 'stty size')
        size_output = read_until(master_fd, lambda data: re.search(rb'\b\d+ \d+\b', data) is not None)
        match = re.search(rb'\b(\d+) (\d+)\b', size_output)
        if not match:
            raise TestFailure('could not parse stty size output')
        rows = int(match.group(1))
        cols = int(match.group(2))
        if rows <= 0 or cols <= 0:
            raise TestFailure(f'invalid winsize reported: {rows}x{cols}')

        token = f'SINGLE_PANE_TOKEN_{int(time.time())}'
        send_line(master_fd, f'printf "%s\n" {token}')
        token_output = read_until(master_fd, lambda data: token.encode() in data)
        if token.encode() not in token_output:
            raise TestFailure('token output did not round-trip')

        send_line(master_fd, 'top -b -n 1 | head -n 3')
        top_output = read_until(master_fd, lambda data: b'load average' in data or b'Tasks:' in data)
        if b'load average' not in top_output and b'Tasks:' not in top_output:
            raise TestFailure('top compatibility check failed')

        send_line(master_fd, "vim -Nu NONE -n -es +'silent !printf VIM_OK\\n' +q")
        vim_output = read_until(master_fd, lambda data: b'VIM_OK' in data)
        if b'VIM_OK' not in vim_output:
            raise TestFailure('vim compatibility check failed')

        if not FORK_EXIT.exists():
            raise TestFailure(f'missing helper: {FORK_EXIT}')
        send_line(master_fd, f'{FORK_EXIT} /tmp/mini-tmux-unused-sideband.sock single-pane-test 5 >/dev/null 2>&1 || true')
        read_briefly(master_fd, duration=0.5)
        time.sleep(0.5)
        assert_no_zombies()

        send_line(master_fd, 'exit')
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
        print('fork_exit/zombie reap check: ok')
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
