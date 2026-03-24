#!/usr/bin/env python3
import os
import pty
import select
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BINARY = ROOT / "mini-tmux"
TIMEOUT = 8.0


class TestFailure(RuntimeError):
    pass


def read_until(fd: int, token: bytes, timeout: float = TIMEOUT) -> bytes:
    deadline = time.time() + timeout
    data = bytearray()
    while time.time() < deadline:
        remaining = max(0.0, deadline - time.time())
        ready, _, _ = select.select([fd], [], [], remaining)
        if fd not in ready:
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        data.extend(chunk)
        if token in data:
            return bytes(data)
    raise TestFailure(f"timeout waiting for token: {token!r}")


def read_briefly(fd: int, duration: float = 0.5) -> bytes:
    deadline = time.time() + duration
    data = bytearray()
    while time.time() < deadline:
        remaining = max(0.0, deadline - time.time())
        ready, _, _ = select.select([fd], [], [], remaining)
        if fd not in ready:
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        data.extend(chunk)
    return bytes(data)


def main() -> int:
    if not BINARY.exists():
        print(f"missing binary: {BINARY}", file=sys.stderr)
        print("run `make` first", file=sys.stderr)
        return 2

    instance = f"interactive-render-test-{os.getpid()}"
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

    try:
        attached = read_until(master_fd, b"mini-tmux: attached")
        attached += read_briefly(master_fd, duration=1.0)
        if b"$ " not in attached:
            raise TestFailure(f"shell prompt did not appear after attach: {attached!r}")

        read_briefly(master_fd, duration=0.5)
        os.write(master_fd, b"tty\x7f\n")
        backspace_output = read_until(master_fd, b"Command 'tt' not found")
        backspace_output += read_briefly(master_fd, duration=0.5)
        if b"Command 'tt' not found" not in backspace_output:
            raise TestFailure(f"backspace editing did not affect the executed command: {backspace_output!r}")
        if b"tty[K" in backspace_output:
            raise TestFailure(f"literal erase-line junk leaked into output: {backspace_output!r}")

        print("interactive render checks passed")
        print("single-pane shell prompt after attach: ok")
        print("backspace line editing execution: ok")
        return 0
    except TestFailure as exc:
        print(f"TEST FAILED: {exc}", file=sys.stderr)
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


if __name__ == "__main__":
    sys.exit(main())