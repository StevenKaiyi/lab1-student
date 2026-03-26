#!/usr/bin/env python3
import argparse
import codecs
import fcntl
import json
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import time
from pathlib import Path

TOOL_ROOT = Path(__file__).resolve().parent
PROJECT_ROOT = TOOL_ROOT.parent
DEFAULT_BINARY = PROJECT_ROOT / 'mini-tmux'
DEFAULT_OUTPUT_ROOT = TOOL_ROOT / 'screenshot-debug'
DEFAULT_ROWS = 24
DEFAULT_COLS = 80
DEFAULT_SETTLE_MS = 120
DEFAULT_INITIAL_SETTLE_MS = 1200
DEFAULT_STARTUP_TIMEOUT_MS = 4000
DEFAULT_POST_STOP_WAIT_MS = 300

NAMED_KEYS = {
    'ENTER': b'\r',
    'RETURN': b'\r',
    'CR': b'\r',
    'LF': b'\n',
    'TAB': b'\t',
    'ESC': b'\x1b',
    'BACKSPACE': b'\x7f',
    'BS': b'\x7f',
    'SPACE': b' ',
    'LT': b'<',
    'GT': b'>',
}


class ScenarioError(RuntimeError):
    pass


class TerminalScreen:
    def __init__(self, rows: int, cols: int) -> None:
        self.rows = max(1, rows)
        self.cols = max(1, cols)
        self.primary = self._make_buffer()
        self.alternate = self._make_buffer()
        self.use_alternate = False
        self.cursor_row = 0
        self.cursor_col = 0
        self.cursor_visible = True
        self.escape_state = 'text'
        self.csi_buffer = ''
        self.decoder = codecs.getincrementaldecoder('utf-8')('replace')

    def _make_buffer(self):
        return [[' ' for _ in range(self.cols)] for _ in range(self.rows)]

    def _buffer(self):
        return self.alternate if self.use_alternate else self.primary

    def _clamp_cursor(self) -> None:
        self.cursor_row = max(0, min(self.rows - 1, self.cursor_row))
        self.cursor_col = max(0, min(self.cols - 1, self.cursor_col))

    def _clear_buffer(self) -> None:
        buf = self._buffer()
        for row in range(self.rows):
            for col in range(self.cols):
                buf[row][col] = ' '

    def _scroll_up(self) -> None:
        buf = self._buffer()
        buf.pop(0)
        buf.append([' ' for _ in range(self.cols)])
        self.cursor_row = self.rows - 1

    def _newline(self) -> None:
        if self.cursor_row >= self.rows - 1:
            self._scroll_up()
        else:
            self.cursor_row += 1

    def _put_char(self, ch: str) -> None:
        if not ch:
            return
        buf = self._buffer()
        buf[self.cursor_row][self.cursor_col] = ch[0]
        if self.cursor_col >= self.cols - 1:
            self.cursor_col = self.cols - 1
        else:
            self.cursor_col += 1

    def _clear_line(self, mode: int) -> None:
        buf = self._buffer()
        if mode == 1:
            start = 0
            end = self.cursor_col + 1
        elif mode == 2:
            start = 0
            end = self.cols
        else:
            start = self.cursor_col
            end = self.cols
        for col in range(max(0, start), min(self.cols, end)):
            buf[self.cursor_row][col] = ' '

    def _clear_screen(self, mode: int) -> None:
        buf = self._buffer()
        if mode == 1:
            row_range = range(0, self.cursor_row + 1)
        elif mode == 2 or mode == 3:
            row_range = range(self.rows)
        else:
            row_range = range(self.cursor_row, self.rows)
        for row in row_range:
            start = 0
            end = self.cols
            if mode == 0 and row == self.cursor_row:
                start = self.cursor_col
            if mode == 1 and row == self.cursor_row:
                end = self.cursor_col + 1
            for col in range(max(0, start), min(self.cols, end)):
                buf[row][col] = ' '

    def _parse_params(self, params: str):
        if not params:
            return []
        out = []
        for part in params.split(';'):
            if part == '':
                out.append(0)
            else:
                try:
                    out.append(int(part))
                except ValueError:
                    out.append(0)
        return out

    def _handle_private_mode(self, mode: int, enable: bool) -> None:
        if mode == 25:
            self.cursor_visible = enable
        elif mode == 1049:
            if enable:
                self.use_alternate = True
                self.alternate = self._make_buffer()
                self.cursor_row = 0
                self.cursor_col = 0
            else:
                self.use_alternate = False
                self._clamp_cursor()

    def _handle_csi(self, final_byte: str) -> None:
        raw = self.csi_buffer
        self.csi_buffer = ''
        private = raw.startswith('?')
        params = raw[1:] if private else raw
        values = self._parse_params(params)

        if final_byte in ('H', 'f'):
            row = values[0] if len(values) >= 1 and values[0] > 0 else 1
            col = values[1] if len(values) >= 2 and values[1] > 0 else 1
            self.cursor_row = row - 1
            self.cursor_col = col - 1
            self._clamp_cursor()
            return

        if final_byte == 'A':
            amount = values[0] if values and values[0] > 0 else 1
            self.cursor_row -= amount
            self._clamp_cursor()
            return
        if final_byte == 'B':
            amount = values[0] if values and values[0] > 0 else 1
            self.cursor_row += amount
            self._clamp_cursor()
            return
        if final_byte == 'C':
            amount = values[0] if values and values[0] > 0 else 1
            self.cursor_col += amount
            self._clamp_cursor()
            return
        if final_byte == 'D':
            amount = values[0] if values and values[0] > 0 else 1
            self.cursor_col -= amount
            self._clamp_cursor()
            return
        if final_byte == 'J':
            mode = values[0] if values else 0
            self._clear_screen(mode)
            return
        if final_byte == 'K':
            mode = values[0] if values else 0
            self._clear_line(mode)
            return
        if final_byte == 'm':
            return
        if final_byte in ('h', 'l') and private:
            enable = final_byte == 'h'
            for value in values:
                self._handle_private_mode(value, enable)
            return

    def feed(self, data: bytes) -> None:
        for ch in self.decoder.decode(data, final=False):
            self._feed_char(ch)

    def _feed_char(self, ch: str) -> None:
        if self.escape_state == 'text':
            if ch == '\x1b':
                self.escape_state = 'escape'
                return
            if ch == '\r':
                self.cursor_col = 0
                return
            if ch == '\n':
                self._newline()
                return
            if ch == '\b':
                self.cursor_col = max(0, self.cursor_col - 1)
                return
            if ch == '\t':
                spaces = 8 - (self.cursor_col % 8)
                for _ in range(spaces):
                    self._put_char(' ')
                return
            if ord(ch) < 32:
                return
            self._put_char(ch)
            return

        if self.escape_state == 'escape':
            if ch == '[':
                self.escape_state = 'csi'
                self.csi_buffer = ''
            elif ch == ']':
                self.escape_state = 'osc'
            else:
                self.escape_state = 'text'
            return

        if self.escape_state == 'csi':
            if '@' <= ch <= '~':
                self._handle_csi(ch)
                self.escape_state = 'text'
            else:
                self.csi_buffer += ch
            return

        if self.escape_state == 'osc':
            if ch == '\a':
                self.escape_state = 'text'
            elif ch == '\x1b':
                self.escape_state = 'osc_escape'
            return

        if self.escape_state == 'osc_escape':
            self.escape_state = 'text' if ch == '\\' else 'osc'

    def snapshot_lines(self):
        return [''.join(row) for row in self._buffer()]

    def snapshot_text(self) -> str:
        return '\n'.join(self.snapshot_lines()) + '\n'


def set_pty_winsize(fd: int, rows: int, cols: int) -> None:
    packed = struct.pack('HHHH', rows, cols, 0, 0)
    fcntl.ioctl(fd, 0x5414, packed)


def socket_path(instance: str) -> str:
    return f'/tmp/mini-tmux-{os.getuid()}-{instance}.sock'


def cleanup_socket(instance: str) -> None:
    try:
        os.unlink(socket_path(instance))
    except FileNotFoundError:
        pass


def read_until_quiet(fd: int, settle_ms: int, max_wait_ms: int) -> bytes:
    settle_deadline = time.monotonic() + (settle_ms / 1000.0)
    hard_deadline = time.monotonic() + (max_wait_ms / 1000.0)
    data = bytearray()
    while True:
        now = time.monotonic()
        deadline = min(settle_deadline, hard_deadline)
        if now >= deadline:
            break
        ready, _, _ = select.select([fd], [], [], deadline - now)
        if fd not in ready:
            break
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        data.extend(chunk)
        settle_deadline = min(hard_deadline, time.monotonic() + (settle_ms / 1000.0))
    return bytes(data)


def read_for_duration(fd: int, duration_ms: int) -> bytes:
    deadline = time.monotonic() + (duration_ms / 1000.0)
    data = bytearray()
    while True:
        now = time.monotonic()
        if now >= deadline:
            break
        ready, _, _ = select.select([fd], [], [], deadline - now)
        if fd not in ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        data.extend(chunk)
    return bytes(data)


def normalize_token(token: str) -> bytes:
    key = token.strip()
    upper = key.upper().replace('CTRL-', 'C-')
    if upper in NAMED_KEYS:
        return NAMED_KEYS[upper]
    if upper.startswith('C-') and len(upper) == 3:
        return bytes([ord(upper[2]) & 0x1f])
    raise ScenarioError(f'unsupported key token <{token}>')


def tokenize_keys(spec: str):
    out = []
    index = 0
    while index < len(spec):
        if spec[index] == '<':
            end = spec.find('>', index + 1)
            if end > index:
                token = spec[index + 1:end]
                out.append((f'<{token}>', normalize_token(token)))
                index = end + 1
                continue
        ch = spec[index]
        out.append((ch, ch.encode('utf-8')))
        index += 1
    return out


def load_scenario(path: Path):
    try:
        data = json.loads(path.read_text())
    except FileNotFoundError as exc:
        raise ScenarioError(f'missing scenario file: {path}') from exc
    except json.JSONDecodeError as exc:
        raise ScenarioError(f'invalid JSON in {path}: {exc}') from exc
    if not isinstance(data, dict):
        raise ScenarioError('scenario root must be an object')
    steps = data.get('steps')
    if not isinstance(steps, list) or not steps:
        raise ScenarioError('scenario must contain a non-empty steps list')
    return data


def save_text(path: Path, text: str) -> None:
    path.write_text(text)


def write_step(run_dir: Path, step_index: int, screen: TerminalScreen, session_raw_path: Path,
               kind: str, label: str, input_bytes: bytes, delta: bytes, extra: dict) -> None:
    step_dir = run_dir / f'step-{step_index:04d}'
    step_dir.mkdir(parents=True, exist_ok=False)
    with session_raw_path.open('ab') as handle:
        handle.write(delta)
    (step_dir / 'delta.raw').write_bytes(delta)
    save_text(step_dir / 'screen.txt', screen.snapshot_text())
    meta = {
        'index': step_index,
        'kind': kind,
        'label': label,
        'input_bytes_hex': input_bytes.hex(),
        'delta_bytes': len(delta),
        'rows': screen.rows,
        'cols': screen.cols,
        'alternate_screen': screen.use_alternate,
        'cursor': {
            'row': screen.cursor_row + 1,
            'col': screen.cursor_col + 1,
            'visible': screen.cursor_visible,
        },
        'timestamp': time.time(),
    }
    meta.update(extra)
    save_text(step_dir / 'meta.json', json.dumps(meta, indent=2, ensure_ascii=False) + '\n')


def run_scenario(binary: Path, scenario_path: Path, out_root: Path) -> Path:
    scenario = load_scenario(scenario_path)
    rows = int(scenario.get('rows', DEFAULT_ROWS))
    cols = int(scenario.get('cols', DEFAULT_COLS))
    settle_ms = int(scenario.get('settle_ms', DEFAULT_SETTLE_MS))
    initial_settle_ms = int(scenario.get('initial_settle_ms', DEFAULT_INITIAL_SETTLE_MS))
    startup_timeout_ms = int(scenario.get('startup_timeout_ms', DEFAULT_STARTUP_TIMEOUT_MS))
    instance = str(scenario.get('instance', f'screenshot-debug-{os.getpid()}'))
    run_stamp = time.strftime('%Y%m%d-%H%M%S')
    run_dir = out_root / f'{run_stamp}-{scenario_path.stem}'
    run_dir.mkdir(parents=True, exist_ok=False)
    session_raw_path = run_dir / 'session.raw'
    session_raw_path.write_bytes(b'')
    save_text(run_dir / 'scenario.json', json.dumps(scenario, indent=2, ensure_ascii=False) + '\n')

    screen = TerminalScreen(rows, cols)
    env = os.environ.copy()
    env['TERM'] = env.get('TERM', 'xterm-256color')
    env['MINI_TMUX_SERVER'] = instance

    cleanup_socket(instance)
    master_fd, slave_fd = pty.openpty()
    set_pty_winsize(slave_fd, rows, cols)
    proc = subprocess.Popen(
        [str(binary)],
        cwd=str(PROJECT_ROOT),
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=slave_fd,
        env=env,
        start_new_session=True,
    )
    os.close(slave_fd)

    step_index = 0
    try:
        delta = read_until_quiet(master_fd, initial_settle_ms, startup_timeout_ms)
        screen.feed(delta)
        write_step(
            run_dir,
            step_index,
            screen,
            session_raw_path,
            'initial',
            'initial settle',
            b'',
            delta,
            {'scenario': scenario_path.name, 'instance': instance},
        )
        step_index += 1

        for raw_step in scenario['steps']:
            if not isinstance(raw_step, dict):
                raise ScenarioError('each step must be an object')
            kind = str(raw_step.get('type', '')).strip().lower()
            label = str(raw_step.get('label', kind or 'step'))
            if kind == 'wait':
                duration_ms = int(raw_step.get('ms', 0))
                delta = read_for_duration(master_fd, duration_ms)
                screen.feed(delta)
                write_step(
                    run_dir,
                    step_index,
                    screen,
                    session_raw_path,
                    'wait',
                    label,
                    b'',
                    delta,
                    {'duration_ms': duration_ms},
                )
                step_index += 1
                continue

            if kind == 'snapshot':
                write_step(
                    run_dir,
                    step_index,
                    screen,
                    session_raw_path,
                    'snapshot',
                    label,
                    b'',
                    b'',
                    {},
                )
                step_index += 1
                continue

            if kind != 'keys':
                raise ScenarioError(f'unsupported step type: {kind}')

            spec = str(raw_step.get('keys', ''))
            per_key = bool(raw_step.get('per_key', True))
            step_settle_ms = int(raw_step.get('settle_ms', settle_ms))
            step_max_wait_ms = int(raw_step.get('max_wait_ms', max(step_settle_ms * 5, 400)))
            tokens = tokenize_keys(spec)
            if not tokens:
                raise ScenarioError('keys step cannot be empty')

            if per_key:
                for key_label, payload in tokens:
                    os.write(master_fd, payload)
                    delta = read_until_quiet(master_fd, step_settle_ms, step_max_wait_ms)
                    screen.feed(delta)
                    write_step(
                        run_dir,
                        step_index,
                        screen,
                        session_raw_path,
                        'key',
                        f'{label}: {key_label}',
                        payload,
                        delta,
                        {'key_label': key_label},
                    )
                    step_index += 1
            else:
                payload = b''.join(chunk for _, chunk in tokens)
                os.write(master_fd, payload)
                delta = read_until_quiet(master_fd, step_settle_ms, step_max_wait_ms)
                screen.feed(delta)
                write_step(
                    run_dir,
                    step_index,
                    screen,
                    session_raw_path,
                    'keys',
                    label,
                    payload,
                    delta,
                    {'token_count': len(tokens)},
                )
                step_index += 1

        delta = read_for_duration(master_fd, int(scenario.get('post_stop_wait_ms', DEFAULT_POST_STOP_WAIT_MS)))
        screen.feed(delta)
        write_step(
            run_dir,
            step_index,
            screen,
            session_raw_path,
            'final',
            'final settle',
            b'',
            delta,
            {},
        )
        return run_dir
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
        cleanup_socket(instance)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description='Drive mini-tmux through a PTY and save per-step screen snapshots.')
    parser.add_argument('scenario', help='Path to a JSON scenario file')
    parser.add_argument('--binary', default=str(DEFAULT_BINARY), help='mini-tmux binary to launch')
    parser.add_argument('--out-dir', default=str(DEFAULT_OUTPUT_ROOT), help='Directory that will receive the run folder')
    args = parser.parse_args(argv)

    binary = Path(args.binary).resolve()
    scenario_path = Path(args.scenario).resolve()
    out_root = Path(args.out_dir).resolve()

    if not binary.exists():
        print(f'missing binary: {binary}', file=sys.stderr)
        print('run `make` first', file=sys.stderr)
        return 2

    try:
        run_dir = run_scenario(binary, scenario_path, out_root)
    except ScenarioError as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        return 1

    print(run_dir)
    return 0


if __name__ == '__main__':
    sys.exit(main())
