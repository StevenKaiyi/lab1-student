#!/usr/bin/env python3
import argparse, json, os, pty, re, select, shutil, signal, socket, struct, subprocess, sys, tempfile, time
from pathlib import Path
import yaml

ROOT = Path(__file__).resolve().parent
BINARY = ROOT / 'mini-tmux'
WORKLOAD_ROOT = ROOT / 'workloads' / 'public'
PROBE = ROOT / 'helpers' / 'probe'
FORK_EXIT = ROOT / 'helpers' / 'fork_exit'
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
LAYOUT_RE = re.compile(r'(\d+)(\*)?\[(\d+)x(\d+)@(\d+)\]')


def norm(text):
    return OSC_RE.sub('', ANSI_RE.sub('', text)).replace('\r', '')


def sock_path(instance):
    return f'/tmp/mini-tmux-{os.getuid()}-{instance}.sock'


def pack(t, a0=0, a1=0, payload=b''):
    return struct.pack('=IIii', t, len(payload), a0, a1) + payload


def parse_layout(payload, focused):
    summary = payload.split('\n', 1)[0]
    marker = summary.find(' layout=')
    if marker < 0:
        return {}
    raw = summary[marker + 8:].strip()
    if raw in ('<none>', 'none'):
        return {}
    out = {}
    for item in raw.split(','):
        m = LAYOUT_RE.fullmatch(item.strip())
        if not m:
            continue
        pane = int(m.group(1))
        out[pane] = {
            'pane': pane,
            'focused': bool(m.group(2)) or pane == focused,
            'rows': int(m.group(3)),
            'cols': int(m.group(4)),
            'top': int(m.group(5)),
        }
    return out


class EvalError(RuntimeError):
    pass


class Client:
    def __init__(self, instance, readonly=False):
        self.instance = instance
        self.readonly = readonly
        self.sock = None
        self.outputs = {}
        self.status = None
        self.exit = None
        self.focus = None
        self.layout = {}

    def connect(self, timeout=8.0):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        end = time.time() + timeout
        last = None
        while time.time() < end:
            try:
                self.sock.connect(sock_path(self.instance))
                self.sock.sendall(pack(MSG_CLIENT_HELLO, 1 if self.readonly else 0, 0))
                return
            except OSError as e:
                last = e
                time.sleep(0.05)
        raise EvalError(f'connect failed: {last}')

    def close(self):
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def send_input(self, text):
        self.sock.sendall(pack(MSG_CLIENT_INPUT, 0, 0, text.encode()))

    def send_resize(self, rows, cols):
        self.sock.sendall(pack(MSG_CLIENT_RESIZE, rows, cols))

    def send_command(self, cmd):
        self.sock.sendall(pack(MSG_CLIENT_COMMAND, 0, 0, cmd.encode()))

    def recv(self, timeout=8.0):
        ready, _, _ = select.select([self.sock], [], [], timeout)
        if self.sock not in ready:
            raise EvalError('timeout waiting for server message')
        data = self.sock.recv(MAX_MESSAGE_SIZE)
        if not data or len(data) < 16:
            raise EvalError('socket closed unexpectedly')
        typ, size, a0, a1 = struct.unpack('=IIii', data[:16])
        if len(data) != 16 + size:
            raise EvalError('malformed message')
        payload = data[16:].decode(errors='replace')
        if typ == MSG_SERVER_OUTPUT:
            self.outputs[a0] = self.outputs.get(a0, '') + payload
            self.outputs[a0] = self.outputs[a0][-200000:]
        elif typ == MSG_SERVER_STATUS:
            self.status = payload
        elif typ == MSG_SERVER_EXIT:
            self.exit = (a0, a1)
        elif typ == MSG_SERVER_REDRAW:
            self.focus = a0
            self.layout = parse_layout(payload, a0)
        return typ, a0, a1, payload

    def drain(self, dur=0.05):
        end = time.time() + dur
        while time.time() < end:
            ready, _, _ = select.select([self.sock], [], [], min(0.02, end - time.time()))
            if self.sock not in ready:
                continue
            self.recv(0)

    def wait(self, pred, timeout=8.0, what='condition'):
        end = time.time() + timeout
        while time.time() < end:
            msg = self.recv(max(0.01, end - time.time()))
            if pred(msg):
                return msg
        raise EvalError(f'timeout waiting for {what}')

    def wait_status(self, timeout=8.0):
        return self.wait(lambda m: m[0] == MSG_SERVER_STATUS, timeout, 'status')[3]

    def wait_redraw(self, timeout=8.0):
        return self.wait(lambda m: m[0] == MSG_SERVER_REDRAW, timeout, 'redraw')[3]

    def wait_output(self, pane, token, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            if token in norm(self.outputs.get(pane, '')):
                return norm(self.outputs.get(pane, ''))
            self.recv(max(0.01, end - time.time()))
        raise EvalError(f'timeout waiting for output token {token!r} on pane {pane}')


class Sideband:
    def __init__(self):
        self.path = tempfile.mktemp(prefix='mini-tmux-sideband-', dir='/tmp')
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server.bind(self.path)
        self.server.listen(16)
        self.server.setblocking(False)
        self.conns = []
        self.buffers = {}

    def close(self):
        for c in self.conns:
            c.close()
        self.server.close()
        try:
            os.unlink(self.path)
        except FileNotFoundError:
            pass

    def poll(self, timeout=0.0):
        ready, _, _ = select.select([self.server] + self.conns, [], [], timeout)
        events = []
        if self.server in ready:
            while True:
                try:
                    c, _ = self.server.accept()
                except BlockingIOError:
                    break
                c.setblocking(False)
                self.conns.append(c)
                self.buffers[c] = b''
        for c in list(self.conns):
            if c not in ready:
                continue
            try:
                data = c.recv(4096)
            except BlockingIOError:
                continue
            if not data:
                c.close()
                self.conns.remove(c)
                self.buffers.pop(c, None)
                continue
            self.buffers[c] += data
            while b'\n' in self.buffers[c]:
                line, self.buffers[c] = self.buffers[c].split(b'\n', 1)
                line = line.strip()
                if line:
                    events.append(json.loads(line.decode(errors='replace')))
        return events


class Context:
    def __init__(self, workload, path):
        self.workload = workload
        self.path = path
        self.instance = f"eval-{workload['id']}-{os.getpid()}"
        self.rows = int(workload.get('terminal', {}).get('rows', 24))
        self.cols = int(workload.get('terminal', {}).get('cols', 80))
        self.server = None
        self.main = None
        self.extras = []
        self.sideband = Sideband()
        self.tmp = Path(tempfile.mkdtemp(prefix=f"mini-tmux-eval-{workload['id']}-"))
        self.panes = {0: {'session': 'pane_0', 'events': [], 'probe': False, 'env': None, 'signals': [], 'outs': [], 'ins': []}}

    def close(self):
        for c in self.extras:
            c.close()
        if self.main is not None:
            self.main.close()
        cleanup_client = None
        try:
            cleanup_client = Client(self.instance)
            cleanup_client.connect(timeout=0.5)
            for pane_id in sorted(self.panes.keys(), reverse=True):
                cleanup_client.send_command(f':kill {pane_id}')
                try:
                    cleanup_client.wait_status(timeout=0.5)
                except Exception:
                    break
        except Exception:
            pass
        finally:
            if cleanup_client is not None:
                cleanup_client.close()
        self.sideband.close()
        shutil.rmtree(self.tmp, ignore_errors=True)
        try:
            os.unlink(sock_path(self.instance))
        except FileNotFoundError:
            pass

    def path_sub(self, s):
        return str(s).replace('$TMP', str(self.tmp))

    def poll(self, dur=0.05):
        end = time.time() + dur
        while time.time() < end:
            for ev in self.sideband.poll(min(0.02, end - time.time())):
                sid = str(ev.get('session', ''))
                for pane in self.panes.values():
                    if pane['session'] == sid:
                        pane['events'].append(ev)
                        if ev.get('type') == 'env_check':
                            pane['env'] = ev
                        elif ev.get('type') == 'signal':
                            pane['signals'].append(ev.get('signal'))
                        elif ev.get('type') == 'output_token':
                            pane['outs'].append(ev.get('token'))
                        elif ev.get('type') == 'input_token':
                            pane['ins'].append(ev.get('received'))
                        elif ev.get('type') == 'ready':
                            pane['probe'] = True
            if self.main is not None:
                self.main.drain(0.01)
            for c in self.extras:
                c.drain(0.01)

    def wait_event(self, pane_id, pred, timeout=8.0, what='event'):
        end = time.time() + timeout
        while time.time() < end:
            self.poll(0.05)
            for ev in reversed(self.panes[pane_id]['events']):
                if pred(ev):
                    return ev
        raise EvalError(f'timeout waiting for pane {pane_id} {what}')


def ensure_prereqs():
    for p in (BINARY, PROBE, FORK_EXIT):
        if not p.exists():
            raise EvalError(f'missing required file: {p}')


def wait_socket(instance, timeout=8.0):
    end = time.time() + timeout
    path = sock_path(instance)
    while time.time() < end:
        if os.path.exists(path):
            return
        time.sleep(0.05)
    raise EvalError(f'timeout waiting for socket {path}')


def attach_and_detach(instance, timeout=8.0):
    env = os.environ.copy()
    env['TERM'] = env.get('TERM', 'xterm-256color')
    env['MINI_TMUX_SERVER'] = instance
    master_fd, slave_fd = pty.openpty()
    proc = subprocess.Popen([str(BINARY), 'attach'], cwd=str(ROOT), stdin=slave_fd, stdout=slave_fd, stderr=slave_fd, env=env, start_new_session=True)
    os.close(slave_fd)
    data = bytearray()
    deadline = time.time() + timeout
    try:
        while time.time() < deadline and b'mini-tmux: attached' not in data:
            ready, _, _ = select.select([master_fd], [], [], min(0.1, max(0.0, deadline - time.time())))
            if master_fd in ready:
                chunk = os.read(master_fd, 4096)
                if not chunk:
                    break
                data.extend(chunk)
        if b'mini-tmux: attached' not in data:
            raise EvalError('attach client did not report attached')
        os.write(master_fd, b'd')
        while time.time() < deadline and b'mini-tmux: detached' not in data:
            ready, _, _ = select.select([master_fd], [], [], min(0.1, max(0.0, deadline - time.time())))
            if master_fd in ready:
                chunk = os.read(master_fd, 4096)
                if not chunk:
                    break
                data.extend(chunk)
        if b'mini-tmux: detached' not in data:
            raise EvalError('attach client did not report detached')
        proc.wait(timeout=3.0)
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


def wait_layout(ctx, panes=None, focus=None, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        ctx.poll(0.05)
        lay = ctx.main.layout
        if lay and (panes is None or len(lay) == panes) and (focus is None or ctx.main.focus == focus):
            return
        try:
            ctx.main.wait_redraw(max(0.05, end - time.time()))
        except EvalError:
            pass
    raise EvalError('timeout waiting for layout')


def focus(ctx, pane_id):
    ctx.main.send_command(f':focus {pane_id}')
    status = ctx.main.wait_status()
    if not status.startswith('ok:'):
        raise EvalError(status)
    wait_layout(ctx, focus=pane_id)


def create_pane(ctx):
    before = len(ctx.main.layout) or len(ctx.panes)
    ctx.main.send_command(':new')
    status = ctx.main.wait_status()
    if not status.startswith('ok: created pane '):
        raise EvalError(status)
    pane_id = int(status.rsplit(' ', 1)[1])
    ctx.panes[pane_id] = {'session': f'pane_{pane_id}', 'events': [], 'probe': False, 'env': None, 'signals': [], 'outs': [], 'ins': []}
    wait_layout(ctx, panes=before + 1, focus=pane_id)
    return pane_id


def stop_probe(ctx, pane_id):
    if not ctx.panes[pane_id]['probe']:
        return
    if ctx.main.focus != pane_id:
        focus(ctx, pane_id)
    ctx.main.send_input('\x04')
    time.sleep(0.3)
    ctx.poll(0.2)
    ctx.panes[pane_id]['probe'] = False


def start_probe(ctx, pane_id):
    if ctx.main.focus != pane_id:
        focus(ctx, pane_id)
    pane = ctx.panes[pane_id]
    pane['events'].clear(); pane['signals'].clear(); pane['outs'].clear(); pane['ins'].clear(); pane['env'] = None
    ctx.main.send_input(f'{PROBE} {ctx.sideband.path} {pane["session"]}\n')
    ctx.wait_event(pane_id, lambda e: e.get('type') == 'ready', 8.0, 'probe ready')
    ctx.wait_event(pane_id, lambda e: e.get('type') == 'env_check', 8.0, 'env_check')
    ctx.wait_event(pane_id, lambda e: e.get('type') == 'output_token', 8.0, 'output_token')
    pane['probe'] = True


def run_cmd(ctx, pane_id, cmd, delay_ms=0):
    stop_probe(ctx, pane_id)
    if ctx.main.focus != pane_id:
        focus(ctx, pane_id)
    ctx.main.send_input(cmd + '\n')
    if delay_ms:
        time.sleep(delay_ms / 1000.0)
    ctx.poll(0.2)


def send_keys(ctx, pane_id, keys, delay_ms=0):
    if ctx.main.focus != pane_id:
        focus(ctx, pane_id)
    mapping = {'C-c': '\x03', 'C-z': '\x1a', 'Enter': '\r', 'Escape': '\x1b'}
    ctx.main.send_input(mapping.get(keys, keys))
    if delay_ms:
        time.sleep(delay_ms / 1000.0)
    ctx.poll(0.2)


class Runner:
    def __init__(self, path):
        self.path = path
        self.workload = yaml.safe_load(path.read_text())
        self.ctx = Context(self.workload, path)

    def run(self):
        ensure_prereqs()
        try:
            for step in self.workload['steps']:
                getattr(self, 'do_' + step['action'])(step)
        finally:
            self.ctx.close()

    def do_start(self, step):
        env = os.environ.copy(); env['TERM'] = env.get('TERM', 'xterm-256color'); env['MINI_TMUX_SERVER'] = self.ctx.instance
        self.ctx.server = subprocess.Popen([str(BINARY)], cwd=str(ROOT), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env, start_new_session=True)
        wait_socket(self.ctx.instance, float(self.workload.get('timeout_sec', 8)))
        self.ctx.main = Client(self.ctx.instance); self.ctx.main.connect(); self.ctx.main.send_resize(self.ctx.rows, self.ctx.cols); wait_layout(self.ctx, panes=1, focus=0)
        try:
            self.ctx.server.wait(timeout=0.2)
        except subprocess.TimeoutExpired:
            pass

    def do_wait(self, step):
        time.sleep(int(step.get('duration_ms', 0)) / 1000.0); self.ctx.poll(0.1)

    def do_create_pane(self, step):
        create_pane(self.ctx)

    def do_create_pane_with_probe(self, step):
        pane_id = 0 if not self.ctx.panes[0]['probe'] and len(self.ctx.main.layout) == 1 and len([p for p in self.ctx.panes.values() if p['probe']]) == 0 else create_pane(self.ctx)
        self.ctx.panes[pane_id]['session'] = str(step.get('session', self.ctx.panes[pane_id]['session']))
        start_probe(self.ctx, pane_id)

    def do_wait_probe_ready(self, step):
        self.ctx.wait_event(int(step['pane']), lambda e: e.get('type') == 'ready', 8.0, 'ready')

    def do_verify_env_check(self, step):
        pane = self.ctx.panes[int(step['pane'])]
        ev = pane['env'] or self.ctx.wait_event(int(step['pane']), lambda e: e.get('type') == 'env_check', 8.0, 'env_check')
        if not ev.get('isatty_stdin') or not ev.get('isatty_stdout'):
            raise EvalError(f'isatty failed: {ev}')
        ws = ev.get('winsize', {})
        if int(ws.get('rows', 0)) <= 0 or int(ws.get('cols', 0)) <= 0:
            raise EvalError(f'invalid winsize: {ev}')

    def do_verify_output_token(self, step):
        pane_id = int(step['pane']); pane = self.ctx.panes[pane_id]
        if not pane['outs']:
            self.ctx.wait_event(pane_id, lambda e: e.get('type') == 'output_token', 8.0, 'output_token')
        self.ctx.main.wait_output(pane_id, pane['outs'][-1], 8.0)

    def do_verify_input_token(self, step):
        pane_id = int(step['pane']); pane = self.ctx.panes[pane_id]
        token = pane['outs'][-1]
        if self.ctx.main.focus != pane_id:
            focus(self.ctx, pane_id)
        self.ctx.main.send_input(token + '\n')
        self.ctx.wait_event(pane_id, lambda e: e.get('type') == 'input_token' and e.get('received') == token, 8.0, 'input_token')

    def do_verify_pgrp_isolation(self, step):
        vals = []
        for pane_id in [int(x) for x in step['panes']]:
            ev = self.ctx.panes[pane_id]['env'] or self.ctx.wait_event(pane_id, lambda e: e.get('type') == 'env_check', 8.0, 'env_check')
            if int(ev.get('pgrp', -1)) <= 0 or int(ev.get('tcpgrp', -1)) != int(ev.get('pgrp', -1)):
                raise EvalError(f'bad process group for pane {pane_id}: {ev}')
            vals.append(int(ev['pgrp']))
        if len(set(vals)) != len(vals):
            raise EvalError(f'pane pgrps are not isolated: {vals}')

    def do_switch_focus(self, step):
        focus(self.ctx, int(step['pane']))

    def do_send_keys(self, step):
        send_keys(self.ctx, int(step['pane']), str(step['keys']), int(step.get('delay_ms', 0)))

    def do_run_command(self, step):
        pane_id = self.ctx.main.focus if self.ctx.main.focus is not None else 0
        run_cmd(self.ctx, pane_id, self.ctx.path_sub(step['command']), int(step.get('delay_ms', 0)))

    def do_resize(self, step):
        self.ctx.rows = int(step['rows']); self.ctx.cols = int(step['cols']); self.ctx.main.send_resize(self.ctx.rows, self.ctx.cols); wait_layout(self.ctx)

    def do_verify_signal(self, step):
        pane_id = int(step['pane']); sig = str(step['signal']); expect = str(step['expect'])
        self.ctx.poll(0.2)
        seen = sig in self.ctx.panes[pane_id]['signals']
        if expect == 'received' and not seen:
            self.ctx.wait_event(pane_id, lambda e: e.get('type') == 'signal' and e.get('signal') == sig, 8.0, sig)
            seen = True
        if expect == 'received' and not seen:
            raise EvalError(f'{sig} not received by pane {pane_id}')
        if expect == 'not_received' and seen:
            raise EvalError(f'{sig} unexpectedly received by pane {pane_id}')

    def do_kill_pane(self, step):
        self.ctx.main.send_command(f":kill {int(step['pane'])}")
        status = self.ctx.main.wait_status()
        if not status.startswith('ok:'):
            raise EvalError(status)
        time.sleep(0.5); self.ctx.poll(0.2)

    def do_verify_zombie_count(self, step):
        proc = subprocess.run(['ps', '-eo', 'ppid=,stat='], capture_output=True, text=True, check=True)
        zombies = sum(1 for line in proc.stdout.splitlines() if line.strip().startswith(str(self.ctx.server.pid)) and 'Z' in line)
        if zombies > int(step.get('max_zombies', 0)):
            raise EvalError(f'zombie count too high: {zombies}')

    def do_detach(self, step):
        attach_and_detach(self.ctx.instance)

    def do_verify_server_alive(self, step):
        if not os.path.exists(sock_path(self.ctx.instance)):
            raise EvalError('server socket is gone')

    def do_reattach(self, step):
        if self.ctx.main is not None:
            self.ctx.extras.append(self.ctx.main)
        self.ctx.main = Client(self.ctx.instance)
        self.ctx.main.connect()
        self.ctx.main.send_resize(self.ctx.rows, self.ctx.cols)
        wait_layout(self.ctx)

    def do_verify_pane_alive(self, step):
        pane_id = int(step['pane'])
        if self.ctx.panes[pane_id]['probe']:
            self.do_verify_output_token({'pane': pane_id})
        else:
            run_cmd(self.ctx, pane_id, "printf '__PANE_ALIVE__\\n'", 100); self.ctx.main.wait_output(pane_id, '__PANE_ALIVE__', 8.0)

    def do_log_start(self, step):
        self.ctx.main.send_command(f":log {int(step['pane'])} {self.ctx.path_sub(step['file_path'])}")
        status = self.ctx.main.wait_status();
        if not status.startswith('ok:'): raise EvalError(status)

    def do_log_stop(self, step):
        self.ctx.main.send_command(f":log-stop {int(step['pane'])}")
        status = self.ctx.main.wait_status();
        if not status.startswith('ok:'): raise EvalError(status)

    def do_pipeout_start(self, step):
        self.ctx.main.send_command(f":pipeout {int(step['pane'])} {self.ctx.path_sub(step['cmd'])}")
        status = self.ctx.main.wait_status();
        if not status.startswith('ok:'): raise EvalError(status)

    def do_pipeout_stop(self, step):
        self.ctx.main.send_command(f":pipeout-stop {int(step['pane'])}")
        status = self.ctx.main.wait_status();
        if not status.startswith('ok:'): raise EvalError(status)

    def do_verify_file_contains(self, step):
        path = Path(self.ctx.path_sub(step['file_path'])); pat = str(step.get('pattern', '')); end = time.time() + 8.0
        while time.time() < end:
            if path.exists():
                data = path.read_text(errors='replace')
                if pat in data or pat == '':
                    return
            time.sleep(0.1)
        raise EvalError(f'file check failed: {path} missing {pat!r}')

    def do_attach_client(self, step):
        c = Client(self.ctx.instance, bool(step.get('readonly', False))); c.connect(); c.send_resize(self.ctx.rows, self.ctx.cols); c.wait_redraw(); self.ctx.extras.append(c)

    def do_verify_extra_client_output(self, step):
        idx = int(step['client']); pane_id = int(step['pane']); c = self.ctx.extras[idx]; pane = self.ctx.panes[pane_id]
        if pane['probe'] and pane['outs']:
            c.wait_output(pane_id, pane['outs'][-1], 8.0)
        else:
            marker = f'__EXTRA_{int(time.time()*1000)}__'; run_cmd(self.ctx, pane_id, f"printf '{marker}\\n'", 100); c.wait_output(pane_id, marker, 8.0)

    def do_verify_extra_client_alive(self, step):
        c = self.ctx.extras[int(step['client'])]; c.send_resize(self.ctx.rows, self.ctx.cols); c.wait_redraw()

    def do_detach_extra_client(self, step):
        self.ctx.extras.pop(int(step['client'])).close()

    def do_verify_readonly_blocked(self, step):
        c = self.ctx.extras[int(step['client'])]; pane_id = int(step['pane']); marker = f'RO_{int(time.time()*1000)}'
        c.send_input(f"printf '{marker}\\n'\n"); time.sleep(0.5); self.ctx.poll(0.2)
        if marker in norm(self.ctx.main.outputs.get(pane_id, '')):
            raise EvalError('readonly client unexpectedly injected input')

    def do_capture_pane(self, step):
        self.ctx.main.send_command(f":capture {int(step['pane'])} {self.ctx.path_sub(step['file_path'])}")
        status = self.ctx.main.wait_status();
        if not status.startswith('ok:'): raise EvalError(status)

    def do_verify_client_exited(self, step):
        code, _ = self.ctx.main.wait(lambda m: m[0] == MSG_SERVER_EXIT, 8.0, 'server exit')[1:3]
        if code != 0:
            raise EvalError(f'client exit code {code}')

    def do_verify_server_dead(self, step):
        end = time.time() + 8.0
        while time.time() < end:
            if self.ctx.server.poll() is not None:
                return
            time.sleep(0.1)
        raise EvalError('server did not exit')

    def do_verify_all_panes_visible(self, step):
        pane_ids = [int(x) for x in step['panes']]; wait_layout(self.ctx, len(pane_ids))
        for pane_id in pane_ids:
            if pane_id not in self.ctx.main.layout or self.ctx.main.layout[pane_id]['rows'] <= 0:
                raise EvalError(f'pane {pane_id} not visible in layout')

    def do_verify_layout_winsize(self, step):
        pane_ids = [int(x) for x in step['panes']]; wait_layout(self.ctx, len(pane_ids))
        if sum(self.ctx.main.layout[p]['rows'] for p in pane_ids) != int(step['total_rows']):
            raise EvalError('layout row sum mismatch')
        for pane_id in pane_ids:
            stop_probe(self.ctx, pane_id); focus(self.ctx, pane_id); self.ctx.main.outputs[pane_id] = ''
            self.ctx.main.send_input('stty size\n')
            expected = f"{self.ctx.main.layout[pane_id]['rows']} {int(step['cols'])}"
            self.ctx.main.wait_output(pane_id, expected, 8.0)


def resolve(names):
    if not names:
        return sorted(WORKLOAD_ROOT.glob('*.yaml'))
    out = []
    for name in names:
        p = WORKLOAD_ROOT / name
        if p.exists():
            out.append(p); continue
        matches = sorted(WORKLOAD_ROOT.glob(f'*{name}*.yaml'))
        if len(matches) == 1:
            out.append(matches[0]); continue
        if not matches:
            raise EvalError(f'workload not found: {name}')
        raise EvalError(f'ambiguous workload name {name}: {[m.name for m in matches]}')
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description='Run public mini-tmux workloads locally')
    ap.add_argument('workloads', nargs='*')
    args = ap.parse_args(argv)
    try:
        paths = resolve(args.workloads)
    except EvalError as e:
        print(f'ERROR: {e}', file=sys.stderr); return 2
    fails = []
    for path in paths:
        print(f'=== {path.name} ===')
        t0 = time.time()
        try:
            Runner(path).run(); print(f'PASS ({time.time()-t0:.2f}s)')
        except Exception as e:
            print(f'FAIL: {e}')
            fails.append((path.name, str(e)))
    if fails:
        print('\nFailures:')
        for name, why in fails:
            print(f'- {name}: {why}')
        return 1
    print(f'\nAll {len(paths)} workload(s) passed.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
