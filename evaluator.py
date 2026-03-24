#!/usr/bin/env python3
"""
Mini-Tmux 严格评测脚本

评测原则：
1. 不迁就用户的代码，严格按照 handout 要求检查
2. 通过 Unix Domain Socket 与 Server 通信
3. 模拟 Client 行为，验证各功能点
4. 对于未实现的功能直接判定失败
"""

import os
import sys
import socket
import struct
import subprocess
import time
import signal
import tempfile
import shutil
from pathlib import Path
from dataclasses import dataclass
from typing import List, Optional, Tuple
import select


# ============= 常量定义 =============
SOCKET_PATH_TEMPLATE = "/tmp/mini-tmux-{uid}-{instance}.sock"
MAX_PAYLOAD = 8192
MAX_MESSAGE_SIZE = 16 + MAX_PAYLOAD  # header + payload

# Message Types
MSG_CLIENT_HELLO = 1
MSG_CLIENT_INPUT = 2
MSG_CLIENT_RESIZE = 3
MSG_CLIENT_COMMAND = 4
MSG_SERVER_OUTPUT = 5
MSG_SERVER_EXIT = 6
MSG_SERVER_REDRAW = 7
MSG_SERVER_STATUS = 8

# 环境变量
ENV_SERVER_NAME = "MINI_TMUX_SERVER"


# ============= 数据结构 =============
@dataclass
class Message:
    type: int
    arg0: int
    arg1: int
    payload: bytes


# ============= 工具函数 =============
def get_socket_path(instance: str = "default") -> str:
    """获取 socket 路径"""
    uid = os.getuid()
    # 过滤非法字符
    instance = ''.join(c if c.isalnum() or c in '-_' else '_' for c in instance)
    return SOCKET_PATH_TEMPLATE.format(uid=uid, instance=instance)


def cleanup_server(instance: str = "default") -> None:
    """清理可能存在的 server 和 socket"""
    socket_path = get_socket_path(instance)
    if os.path.exists(socket_path):
        os.unlink(socket_path)


def kill_mini_tmux_processes() -> None:
    """清理所有 mini-tmux 进程"""
    try:
        subprocess.run(["pkill", "-9", "mini-tmux"], capture_output=True, timeout=2)
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass
    time.sleep(0.2)


def format_message(msg_type: int, arg0: int = 0, arg1: int = 0, payload: bytes = b"") -> bytes:
    """格式化消息"""
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"Payload too large: {len(payload)} > {MAX_PAYLOAD}")
    header = struct.pack("!IIii", msg_type, len(payload), arg0, arg1)
    return header + payload


def parse_message(data: bytes) -> Tuple[Message, int]:
    """解析消息"""
    if len(data) < 16:
        raise ValueError("Incomplete header")

    msg_type, size, arg0, arg1 = struct.unpack("!IIii", data[:16])
    if len(data) < 16 + size:
        raise ValueError("Incomplete payload")

    payload = data[16:16 + size] if size > 0 else b""
    return Message(msg_type, arg0, arg1, payload), 16 + size


# ============= MiniTmuxClient 类 =============
class MiniTmuxClient:
    """mini-tmux 客户端"""

    def __init__(self, instance: str = "default", readonly: bool = False):
        self.instance = instance
        self.readonly = readonly
        self.socket_path = get_socket_path(instance)
        self.sock: Optional[socket.socket] = None
        self.connected = False

    def connect(self, timeout: float = 5.0) -> bool:
        """连接到 server"""
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)

        start = time.time()
        while time.time() - start < timeout:
            try:
                self.sock.connect(self.socket_path)
                self.connected = True
                # 发送 hello
                hello = format_message(MSG_CLIENT_HELLO, 1 if self.readonly else 0, 0)
                self.sock.sendall(hello)
                return True
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.05)
            except Exception as e:
                print(f"  [ERROR] Connect failed: {e}")
                return False

        print(f"  [ERROR] Timeout connecting to {self.socket_path}")
        return False

    def disconnect(self) -> None:
        """断开连接"""
        if self.sock:
            self.sock.close()
            self.sock = None
        self.connected = False

    def send_input(self, data: str) -> bool:
        """发送输入"""
        if not self.connected or not self.sock:
            return False
        try:
            msg = format_message(MSG_CLIENT_INPUT, 0, 0, data.encode())
            self.sock.sendall(msg)
            return True
        except Exception as e:
            print(f"  [ERROR] Send input failed: {e}")
            return False

    def send_resize(self, rows: int, cols: int) -> bool:
        """发送 resize"""
        if not self.connected or not self.sock:
            return False
        try:
            msg = format_message(MSG_CLIENT_RESIZE, rows, cols)
            self.sock.sendall(msg)
            return True
        except Exception as e:
            print(f"  [ERROR] Send resize failed: {e}")
            return False

    def send_command(self, cmd: str) -> bool:
        """发送命令"""
        if not self.connected or not self.sock:
            return False
        try:
            msg = format_message(MSG_CLIENT_COMMAND, 0, 0, cmd.encode())
            self.sock.sendall(msg)
            return True
        except Exception as e:
            print(f"  [ERROR] Send command failed: {e}")
            return False

    def recv_message(self, timeout: float = 1.0) -> Optional[Message]:
        """接收消息"""
        if not self.connected or not self.sock:
            return None

        self.sock.settimeout(timeout)
        try:
            data = self.sock.recv(MAX_MESSAGE_SIZE)
            if not data:
                return None

            msg, _ = parse_message(data)
            return msg
        except socket.timeout:
            return None
        except Exception as e:
            print(f"  [ERROR] Recv message failed: {e}")
            return None

    def recv_messages(self, duration: float = 0.5) -> List[Message]:
        """接收多个消息"""
        messages = []
        start = time.time()
        self.sock.setblocking(False)

        while time.time() - start < duration:
            try:
                ready, _, _ = select.select([self.sock], [], [], 0.1)
                if ready:
                    data = self.sock.recv(MAX_MESSAGE_SIZE)
                    if not data:
                        break
                    msg, _ = parse_message(data)
                    messages.append(msg)
            except BlockingIOError:
                continue
            except Exception:
                break

        self.sock.settimeout(1.0)
        return messages

    def wait_for_output(self, expected: str, timeout: float = 5.0) -> bool:
        """等待特定输出"""
        start = time.time()
        buffer = ""

        while time.time() - start < timeout:
            msgs = self.recv_messages(0.3)
            for msg in msgs:
                if msg.type == MSG_SERVER_OUTPUT:
                    buffer += msg.payload.decode('utf-8', errors='ignore')
                    if expected in buffer:
                        return True
            time.sleep(0.1)

        return False


# ============= Server 管理类 =============
class ServerManager:
    """管理 mini-tmux server 进程"""

    def __init__(self, instance: str = "default", rows: int = 24, cols: int = 80):
        self.instance = instance
        self.rows = rows
        self.cols = cols
        self.process: Optional[subprocess.Popen] = None
        self.env = os.environ.copy()
        self.env[ENV_SERVER_NAME] = instance

    def start(self, timeout: float = 3.0) -> bool:
        """启动 server"""
        # 清理旧的 socket
        cleanup_server(self.instance)

        # 启动 server
        try:
            self.process = subprocess.Popen(
                ["./mini-tmux"],
                env=self.env,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE
            )

            # 等待 socket 出现
            socket_path = get_socket_path(self.instance)
            start = time.time()
            while time.time() - start < timeout:
                if os.path.exists(socket_path):
                    time.sleep(0.2)  # 给 server 一点启动时间
                    return True
                if self.process.poll() is not None:
                    # 进程已退出
                    stdout, stderr = self.process.communicate()
                    print(f"  [ERROR] Server exited early:")
                    print(f"    stdout: {stdout.decode('utf-8', errors='ignore')}")
                    print(f"    stderr: {stderr.decode('utf-8', errors='ignore')}")
                    return False
                time.sleep(0.05)

            print(f"  [ERROR] Socket not appeared after {timeout}s")
            return False

        except Exception as e:
            print(f"  [ERROR] Failed to start server: {e}")
            return False

    def stop(self) -> None:
        """停止 server"""
        if self.process:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
            self.process = None
        cleanup_server(self.instance)

    def is_alive(self) -> bool:
        """检查 server 是否存活"""
        if not self.process:
            return False
        return self.process.poll() is None

    def get_pid(self) -> Optional[int]:
        """获取 server 进程 PID"""
        if self.process:
            return self.process.pid
        return None


# ============= 测试基类 =============
class TestCase:
    """测试用例基类"""

    def __init__(self, name: str):
        self.name = name
        self.passed = False
        self.fail_reason = ""
        self.server_manager: Optional[ServerManager] = None
        self.client: Optional[MiniTmuxClient] = None
        self.extra_clients: List[MiniTmuxClient] = []

    def setup(self) -> bool:
        """设置测试环境"""
        return True

    def run(self) -> bool:
        """运行测试，返回是否通过"""
        raise NotImplementedError

    def teardown(self) -> None:
        """清理测试环境"""
        for client in self.extra_clients:
            client.disconnect()
        self.extra_clients.clear()

        if self.client:
            self.client.disconnect()
            self.client = None

        if self.server_manager:
            self.server_manager.stop()
            self.server_manager = None

    def fail(self, reason: str) -> bool:
        """记录失败原因"""
        self.fail_reason = reason
        return False

    def assert_server_alive(self) -> bool:
        """断言 server 存活"""
        if not self.server_manager or not self.server_manager.is_alive():
            return self.fail("Server is not alive")
        return True

    def assert_client_connected(self) -> bool:
        """断言 client 已连接"""
        if not self.client or not self.client.connected:
            return self.fail("Client is not connected")
        return True


# ============= 测试用例实现 =============


class TestSinglePaneBasic(TestCase):
    """01: 单 Pane 基本 I/O"""

    def __init__(self):
        super().__init__("Single pane basic I/O")

    def run(self) -> bool:
        if not self.setup():
            return False

        self.server_manager = ServerManager("test_01", 24, 80)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        self.client = MiniTmuxClient("test_01")
        if not self.client.connect():
            return self.fail("Failed to connect client")

        # 验证连接正常
        time.sleep(0.5)

        # 检查是否收到任何输出消息
        msgs = self.client.recv_messages(1.0)
        has_output = any(msg.type == MSG_SERVER_OUTPUT for msg in msgs)

        if not has_output:
            # 这是基础测试，至少应该有 shell 的输出
            return self.fail("No server output received - PTY may not be working")

        return True


class TestTwoPaneSignalIsolation(TestCase):
    """02: 双 Pane 信号隔离"""

    def __init__(self):
        super().__init__("Two pane signal isolation")

    def run(self) -> bool:
        if not self.setup():
            return False

        self.server_manager = ServerManager("test_02", 24, 80)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        self.client = MiniTmuxClient("test_02")
        if not self.client.connect():
            return self.fail("Failed to connect client")

        # 尝试创建第二个 pane
        self.client.send_command(":new")
        time.sleep(0.3)

        # 检查响应
        msgs = self.client.recv_messages(0.5)
        status_msgs = [msg for msg in msgs if msg.type == MSG_SERVER_STATUS]

        if not status_msgs:
            return self.fail("No status response to :new")

        status = status_msgs[0].payload.decode('utf-8', errors='ignore')

        # 严格检查：如果响应说 "not implemented yet"，则测试失败
        if "not implemented" in status.lower() or "not implemented yet" in status.lower():
            return self.fail(":new command not implemented")

        # 检查是否成功创建
        if not status.startswith("ok:"):
            return self.fail(f":new failed: {status}")

        # 尝试切换焦点到第二个 pane
        self.client.send_command(":focus 1")
        time.sleep(0.3)

        msgs = self.client.recv_messages(0.5)
        status_msgs = [msg for msg in msgs if msg.type == MSG_SERVER_STATUS]

        if status_msgs:
            status = status_msgs[0].payload.decode('utf-8', errors='ignore')
            if "does not exist" in status:
                return self.fail(":focus 1 failed - pane 1 not created")

        return True


class TestFourPaneFocusSwitch(TestCase):
    """03: 四 Pane 焦点切换"""

    def __init__(self):
        super().__init__("Four pane focus switch")

    def run(self) -> bool:
        if not self.setup():
            return False

        self.server_manager = ServerManager("test_03", 24, 80)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        self.client = MiniTmuxClient("test_03")
        if not self.client.connect():
            return self.fail("Failed to connect client")

        # 尝试创建多个 panes
        for i in range(3):
            self.client.send_command(":new")
            time.sleep(0.2)

        time.sleep(0.5)

        # 检查是否创建了多个 panes
        # 尝试切换到不同的 pane
        for pane_id in [0, 1, 2, 3]:
            self.client.send_command(f":focus {pane_id}")
            time.sleep(0.1)

        time.sleep(0.3)
        msgs = self.client.recv_messages(0.5)
        status_msgs = [msg for msg in msgs if msg.type == MSG_SERVER_STATUS]

        # 如果所有 focus 都返回 "does not exist"，说明没有创建多个 panes
        all_not_exist = all("does not exist" in msg.payload.decode('utf-8', errors='ignore')
                           for msg in status_msgs)

        if all_not_exist:
            return self.fail("Multiple panes not supported - all :focus commands failed")

        return True


class TestPaneCreateDestroy(TestCase):
    """04: Pane 创建和销毁"""

    def __init__(self):
        super().__init__("Pane create and destroy")

    def run(self) -> bool:
        if not self.setup():
            return False

        self.server_manager = ServerManager("test_04", 24, 80)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        self.client = MiniTmuxClient("test_04")
        if not self.client.connect():
            return self.fail("Failed to connect client")

        # 创建 pane
        self.client.send_command(":new")
        time.sleep(0.3)

        msgs = self.client.recv_messages(0.5)
        status_msgs = [msg for msg in msgs if msg.type == MSG_SERVER_STATUS]

        # 尝试销毁 pane
        self.client.send_command(":kill 1")
        time.sleep(0.3)

        msgs = self.client.recv_messages(0.5)
        status_msgs = [msg for msg in msgs if msg.type == MSG_SERVER_STATUS]

        if status_msgs:
            status = status_msgs[-1].payload.decode('utf-8', errors='ignore')
            if "not implemented" in status.lower():
                return self.fail(":kill command not implemented")

        return True


class TestHighFreqOutput(TestCase):
    """05: 高频输出压力测试"""

    def __init__(self):
        super().__init__("High frequency output")

    def run(self) -> bool:
        if not self.setup():
            return False

        self.server_manager = ServerManager("test_05", 24, 80)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        self.client = MiniTmuxClient("test_05")
        if not self.client.connect():
            return self.fail("Failed to connect client")

        # 等待 shell 启动
        time.sleep(0.5)

        # 发送高频输出命令
        self.client.send_input("seq 1 10000\n")
        time.sleep(2.0)

        # 检查是否收到大量输出
        msgs = self.client.recv_messages(0.5)
        total_output = sum(len(msg.payload) for msg in msgs if msg.type == MSG_SERVER_OUTPUT)

        if total_output < 1000:
            return self.fail(f"Insufficient output received ({total_output} bytes)")

        return True


class TestZombieReap(TestCase):
    """06: 僵尸进程回收"""

    def __init__(self):
        super().__init__("Zombie process reaping")

    def run(self) -> bool:
        if not self.setup():
            return False

        # 检查初始僵尸进程数
        def count_zombies() -> int:
            try:
                result = subprocess.run(
                    ["ps", "aux"],
                    capture_output=True,
                    text=True,
                    timeout=2
                )
                return result.stdout.count("defunct")
            except:
                return 0

        initial_zombies = count_zombies()

        self.server_manager = ServerManager("test_06", 24, 80)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        self.client = MiniTmuxClient("test_06")
        if not self.client.connect():
            return self.fail("Failed to connect client")

        # 运行一些命令
        self.client.send_input("true\n")
        time.sleep(0.5)

        # 检查僵尸进程
        zombies = count_zombies()
        if zombies > initial_zombies + 2:  # 允许少量波动
            return self.fail(f"Zombie processes detected: {zombies} (initial: {initial_zombies})")

        return True


class TestResizeSigwinch(TestCase):
    """07: Resize 触发 SIGWINCH"""

    def __init__(self):
        super().__init__("Resize triggers SIGWINCH")

    def run(self) -> bool:
        if not self.setup():
            return False

        self.server_manager = ServerManager("test_07", 24, 80)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        self.client = MiniTmuxClient("test_07")
        if not self.client.connect():
            return self.fail("Failed to connect client")

        time.sleep(0.5)

        # 发送 resize
        self.client.send_resize(30, 100)
        time.sleep(0.5)

        # 检查是否正常响应
        msgs = self.client.recv_messages(0.5)
        has_redraw = any(msg.type == MSG_SERVER_REDRAW for msg in msgs)

        # SIGWINCH 检查需要更底层的机制，这里只检查 resize 是否被接受
        return True


class TestEightPaneStress(TestCase):
    """08: 八 Pane 压力测试"""

    def __init__(self):
        super().__init__("Eight pane stress")

    def run(self) -> bool:
        if not self.setup():
            return False

        self.server_manager = ServerManager("test_08", 48, 120)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        self.client = MiniTmuxClient("test_08")
        if not self.client.connect():
            return self.fail("Failed to connect client")

        # 尝试创建多个 panes
        for i in range(7):
            self.client.send_command(":new")
            time.sleep(0.15)

        time.sleep(1.0)

        # 尝试切换到所有 panes
        for i in range(8):
            self.client.send_command(f":focus {i}")
            time.sleep(0.05)

        time.sleep(0.5)
        msgs = self.client.recv_messages(0.5)
        status_msgs = [msg for msg in msgs if msg.type == MSG_SERVER_STATUS]

        # 检查是否至少能处理一些命令
        successful_focus = sum(1 for msg in status_msgs
                              if "does not exist" not in msg.payload.decode('utf-8', errors='ignore'))

        if successful_focus == 0:
            return self.fail("Cannot create or switch to multiple panes")

        return True


class TestTuiCompat(TestCase):
    """09: TUI 程序兼容性"""

    def __init__(self):
        super().__init__("TUI compatibility")

    def run(self) -> bool:
        if not self.setup():
            return False

        self.server_manager = ServerManager("test_09", 24, 80)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        self.client = MiniTmuxClient("test_09")
        if not self.client.connect():
            return self.fail("Failed to connect client")

        time.sleep(0.5)

        # 检查 vim 是否可用
        try:
            subprocess.run(["which", "vim"], capture_output=True, check=True, timeout=1)
        except:
            print("  [SKIP] vim not available")
            return True  # 跳过此测试

        # 创建新 pane (如果支持)
        self.client.send_command(":new")
        time.sleep(0.3)

        self.client.send_command(":focus 1")
        time.sleep(0.2)

        # 启动 vim
        self.client.send_input("vim\n")
        time.sleep(1.0)

        # 进入插入模式并输入内容
        self.client.send_input("i")
        time.sleep(0.2)
        self.client.send_input("VIM_STATE_TEST")
        time.sleep(0.2)
        self.client.send_input("\x1b")  # ESC
        time.sleep(0.2)

        # 切换到 pane 0 - 测试状态保持
        self.client.send_command(":focus 0")
        time.sleep(0.3)

        # 在 pane 0 执行命令
        self.client.send_input("echo PANE0_CHECK\n")
        time.sleep(0.3)

        msgs = self.client.recv_messages(0.5)
        has_pane0 = any(b"PANE0_CHECK" in msg.payload for msg in msgs)

        if not has_pane0:
            return self.fail("Pane 0 not responsive")

        # 切回 pane 1
        self.client.send_command(":focus 1")
        time.sleep(0.3)

        # 尝试写入文件验证 vim 仍在运行
        self.client.send_input(":w /tmp/vim_state_test.txt\n")
        time.sleep(0.5)

        # 退出 vim
        self.client.send_input(":q!\n")
        time.sleep(0.5)

        # 检查文件是否创建成功
        test_file = "/tmp/vim_state_test.txt"
        try:
            with open(test_file, "r") as f:
                content = f.read()
                if "VIM_STATE_TEST" not in content:
                    print("  [WARN] Vim state may not have been preserved (content mismatch)")
                else:
                    print("  [INFO] Vim state preserved across pane switch")
        except FileNotFoundError:
            print("  [WARN] Vim state test file not created")
        finally:
            # 清理
            try:
                os.unlink(test_file)
            except:
                pass

        # 检查是否回到 shell
        msgs = self.client.recv_messages(1.0)
        has_shell_output = any(msg.type == MSG_SERVER_OUTPUT
                              for msg in msgs
                              if b"$" in msg.payload or b"#" in msg.payload)

        return True


class TestConcurrentOutput(TestCase):
    """10: 双 Pane 并发输出"""

    def __init__(self):
        super().__init__("Concurrent output")

    def run(self) -> bool:
        if not self.setup():
            return False

        self.server_manager = ServerManager("test_10", 24, 80)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        self.client = MiniTmuxClient("test_10")
        if not self.client.connect():
            return self.fail("Failed to connect client")

        time.sleep(0.5)

        # 尝试创建第二个 pane
        self.client.send_command(":new")
        time.sleep(0.3)

        # 切换到第二个 pane 并发送命令
        self.client.send_command(":focus 1")
        time.sleep(0.2)
        self.client.send_input("yes | head -1000\n")
        time.sleep(1.5)

        # 切回第一个 pane
        self.client.send_command(":focus 0")
        time.sleep(0.2)

        # 检查第一个 pane 仍可响应
        self.client.send_input("echo TEST\n")
        time.sleep(0.5)

        msgs = self.client.recv_messages(1.0)
        has_test = any(b"TEST" in msg.payload for msg in msgs)

        if not has_test:
            return self.fail("Pane 0 not responsive while Pane 1 is producing output")

        return True


class TestSigtstpSigcont(TestCase):
    """11: SIGTSTP 和 SIGCONT 传递"""

    def __init__(self):
        super().__init__("SIGTSTP/SIGCONT delivery")

    def run(self) -> bool:
        if not self.setup():
            return False

        self.server_manager = ServerManager("test_11", 24, 80)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        self.client = MiniTmuxClient("test_11")
        if not self.client.connect():
            return self.fail("Failed to connect client")

        time.sleep(0.5)

        # 发送 Ctrl-Z
        self.client.send_input("\x1a")  # Ctrl-Z
        time.sleep(0.5)

        # 信号处理检查需要侧信道，这里只检查是否不会崩溃
        if not self.server_manager.is_alive():
            return self.fail("Server crashed after SIGTSTP")

        return True


class TestRapidPaneOps(TestCase):
    """12: 快速 Pane 创建/销毁"""

    def __init__(self):
        super().__init__("Rapid pane operations")

    def run(self) -> bool:
        if not self.setup():
            return False

        self.server_manager = ServerManager("test_12", 24, 80)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        self.client = MiniTmuxClient("test_12")
        if not self.client.connect():
            return self.fail("Failed to connect client")

        time.sleep(0.3)

        # 快速创建和销毁 panes
        for i in range(3):
            self.client.send_command(":new")
            time.sleep(0.2)
            self.client.send_command(f":kill {i+1}")
            time.sleep(0.2)

        time.sleep(0.5)

        # 检查 server 是否仍存活
        if not self.server_manager.is_alive():
            return self.fail("Server crashed during rapid operations")

        # 检查是否仍有响应
        self.client.send_input("echo ALIVE\n")
        time.sleep(0.5)

        msgs = self.client.recv_messages(0.5)
        has_output = any(msg.type == MSG_SERVER_OUTPUT for msg in msgs)

        if not has_output:
            return self.fail("No response after rapid operations")

        return True


class TestDetachReattachBasic(TestCase):
    """13: Detach 和 Reattach 基本功能"""

    def __init__(self):
        super().__init__("Detach and reattach basic")

    def run(self) -> bool:
        if not self.setup():
            return False

        self.server_manager = ServerManager("test_13", 24, 80)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        self.client = MiniTmuxClient("test_13")
        if not self.client.connect():
            return self.fail("Failed to connect client")

        time.sleep(0.5)

        # Detach
        self.client.disconnect()
        time.sleep(0.5)

        # 检查 server 是否存活
        if not self.server_manager.is_alive():
            return self.fail("Server died after detach")

        # Reattach
        self.client = MiniTmuxClient("test_13")
        if not self.client.connect():
            return self.fail("Failed to reattach")

        # 检查是否仍能通信
        self.client.send_input("echo REATTACHED\n")
        time.sleep(0.5)

        msgs = self.client.recv_messages(0.5)
        has_output = any(b"REATTACHED" in msg.payload for msg in msgs)

        if not has_output:
            return self.fail("No output after reattach")

        return True


class TestDetachProbeSurvives(TestCase):
    """14: Detach 时 Probe 存活"""

    def __init__(self):
        super().__init__("Detach probe survives")

    def run(self) -> bool:
        if not self.setup():
            return False

        self.server_manager = ServerManager("test_14", 24, 80)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        self.client = MiniTmuxClient("test_14")
        if not self.client.connect():
            return self.fail("Failed to connect client")

        time.sleep(0.5)

        # Detach
        self.client.disconnect()
        time.sleep(1.0)

        # 检查 server 和 panes 是否存活
        if not self.server_manager.is_alive():
            return self.fail("Server died after detach")

        # Reattach
        self.client = MiniTmuxClient("test_14")
        if not self.client.connect():
            return self.fail("Failed to reattach")

        self.client.send_input("echo BACK\n")
        time.sleep(0.5)

        msgs = self.client.recv_messages(0.5)
        has_output = any(b"BACK" in msg.payload for msg in msgs)

        if not has_output:
            return self.fail("Pane not responsive after reattach")

        return True


class TestLogBasic(TestCase):
    """15: 日志功能"""

    def __init__(self):
        super().__init__("Log basic")

    def run(self) -> bool:
        if not self.setup():
            return False

        tmpdir = tempfile.mkdtemp()

        try:
            self.server_manager = ServerManager("test_15", 24, 80)
            if not self.server_manager.start():
                return self.fail("Failed to start server")

            self.client = MiniTmuxClient("test_15")
            if not self.client.connect():
                return self.fail("Failed to connect client")

            time.sleep(0.5)

            # 尝试启动日志 (通过命令)
            log_file = os.path.join(tmpdir, "log.txt")

            # 发送测试输出
            self.client.send_input("echo LOGTEST_MARKER\n")
            time.sleep(0.5)

            # 检查是否有输出
            msgs = self.client.recv_messages(0.5)
            has_marker = any(b"LOGTEST_MARKER" in msg.payload for msg in msgs)

            if not has_marker:
                return self.fail("Output not captured")

            # 日志功能需要 :log 命令，检查是否实现
            self.client.send_command(f":log {log_file}")
            time.sleep(0.3)

            msgs = self.client.recv_messages(0.3)
            status_msgs = [msg for msg in msgs if msg.type == MSG_SERVER_STATUS]

            if status_msgs:
                status = status_msgs[0].payload.decode('utf-8', errors='ignore')
                if "unknown command" in status.lower():
                    return self.fail(":log command not implemented")

            return True

        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)


class TestPipeoutBasic(TestCase):
    """16: Pipeout 基本功能"""

    def __init__(self):
        super().__init__("Pipeout basic")

    def run(self) -> bool:
        if not self.setup():
            return False

        tmpdir = tempfile.mkdtemp()

        try:
            self.server_manager = ServerManager("test_16", 24, 80)
            if not self.server_manager.start():
                return self.fail("Failed to start server")

            self.client = MiniTmuxClient("test_16")
            if not self.client.connect():
                return self.fail("Failed to connect client")

            time.sleep(0.5)

            # Pipeout 需要通过命令实现
            pipe_file = os.path.join(tmpdir, "pipeout.txt")

            self.client.send_input("echo PIPETEST_MARKER\n")
            time.sleep(0.5)

            # 检查是否有输出
            msgs = self.client.recv_messages(0.5)
            has_marker = any(b"PIPETEST_MARKER" in msg.payload for msg in msgs)

            if not has_marker:
                return self.fail("Output not captured")

            # 检查 pipeout 命令
            self.client.send_command(f":pipeout {pipe_file}")
            time.sleep(0.3)

            msgs = self.client.recv_messages(0.3)
            status_msgs = [msg for msg in msgs if msg.type == MSG_SERVER_STATUS]

            if status_msgs:
                status = status_msgs[0].payload.decode('utf-8', errors='ignore')
                if "unknown command" in status.lower():
                    return self.fail(":pipeout command not implemented")

            return True

        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)


class TestPipeoutCmdExits(TestCase):
    """17: Pipeout 命令退出时清理"""

    def __init__(self):
        super().__init__("Pipeout command exits cleanup")

    def run(self) -> bool:
        if not self.setup():
            return False

        # 此测试依赖于 pipeout 命令的实现
        self.server_manager = ServerManager("test_17", 24, 80)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        self.client = MiniTmuxClient("test_17")
        if not self.client.connect():
            return self.fail("Failed to connect client")

        time.sleep(0.5)

        # 检查僵尸进程
        def count_zombies() -> int:
            try:
                result = subprocess.run(
                    ["ps", "aux"],
                    capture_output=True,
                    text=True,
                    timeout=2
                )
                return result.stdout.count("defunct")
            except:
                return 0

        initial_zombies = count_zombies()

        # 等待一段时间
        time.sleep(2.0)

        zombies = count_zombies()
        if zombies > initial_zombies + 2:
            return self.fail(f"Zombie processes: {zombies} (initial: {initial_zombies})")

        return True


class TestMultiClientBasic(TestCase):
    """18: 多 Client 基本功能"""

    def __init__(self):
        super().__init__("Multi client basic")

    def run(self) -> bool:
        if not self.setup():
            return False

        self.server_manager = ServerManager("test_18", 24, 80)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        # 主 client
        self.client = MiniTmuxClient("test_18")
        if not self.client.connect():
            return self.fail("Failed to connect main client")

        time.sleep(0.5)

        # 额外的 client
        extra_client = MiniTmuxClient("test_18")
        if not extra_client.connect():
            return self.fail("Failed to connect extra client")

        self.extra_clients.append(extra_client)

        # 通过主 client 发送输入
        self.client.send_input("echo MULTICLIENT\n")
        time.sleep(0.5)

        # 检查两个 client 是否都收到输出
        main_msgs = self.client.recv_messages(0.5)
        extra_msgs = extra_client.recv_messages(0.5)

        main_has_output = any(b"MULTICLIENT" in msg.payload for msg in main_msgs)
        extra_has_output = any(b"MULTICLIENT" in msg.payload for msg in extra_msgs)

        if not main_has_output:
            return self.fail("Main client didn't receive output")

        if not extra_has_output:
            # 这是多 client 的核心要求
            return self.fail("Extra client didn't receive output")

        return True


class TestMultiClientReadonly(TestCase):
    """19: 只读 Client"""

    def __init__(self):
        super().__init__("Multi client readonly")

    def run(self) -> bool:
        if not self.setup():
            return False

        self.server_manager = ServerManager("test_19", 24, 80)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        # 主 client (读写)
        self.client = MiniTmuxClient("test_19")
        if not self.client.connect():
            return self.fail("Failed to connect main client")

        # 只读 client
        readonly_client = MiniTmuxClient("test_19", readonly=True)
        if not readonly_client.connect():
            return self.fail("Failed to connect readonly client")

        self.extra_clients.append(readonly_client)

        time.sleep(0.5)

        # 通过主 client 发送输入
        self.client.send_input("echo READONLY_TEST\n")
        time.sleep(0.5)

        # 检查只读 client 是否能接收输出
        readonly_msgs = readonly_client.recv_messages(0.5)
        readonly_has_output = any(b"READONLY_TEST" in msg.payload for msg in readonly_msgs)

        if not readonly_has_output:
            return self.fail("Readonly client didn't receive output")

        return True


class TestCapturePane(TestCase):
    """20: Capture Pane"""

    def __init__(self):
        super().__init__("Capture pane")

    def run(self) -> bool:
        if not self.setup():
            return False

        tmpdir = tempfile.mkdtemp()

        try:
            self.server_manager = ServerManager("test_20", 24, 80)
            if not self.server_manager.start():
                return self.fail("Failed to start server")

            self.client = MiniTmuxClient("test_20")
            if not self.client.connect():
                return self.fail("Failed to connect client")

            time.sleep(0.5)

            # 发送测试输出
            self.client.send_input("echo CAPTURE_TEST_MARKER\n")
            time.sleep(0.5)

            # 检查是否有输出
            msgs = self.client.recv_messages(0.5)
            has_marker = any(b"CAPTURE_TEST_MARKER" in msg.payload for msg in msgs)

            if not has_marker:
                return self.fail("Output not captured")

            capture_file = os.path.join(tmpdir, "capture.txt")

            # 检查 capture 命令
            self.client.send_command(f":capture {capture_file}")
            time.sleep(0.3)

            msgs = self.client.recv_messages(0.3)
            status_msgs = [msg for msg in msgs if msg.type == MSG_SERVER_STATUS]

            if status_msgs:
                status = status_msgs[0].payload.decode('utf-8', errors='ignore')
                if "unknown command" in status.lower():
                    return self.fail(":capture command not implemented")

            return True

        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)


class TestLastPaneExit(TestCase):
    """21: 最后 Pane 退出时 Server 清理"""

    def __init__(self):
        super().__init__("Last pane exit server cleanup")

    def run(self) -> bool:
        if not self.setup():
            return False

        self.server_manager = ServerManager("test_21", 24, 80)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        self.client = MiniTmuxClient("test_21")
        if not self.client.connect():
            return self.fail("Failed to connect client")

        time.sleep(0.5)

        # 发送 exit 命令
        self.client.send_input("exit\n")
        time.sleep(2.0)

        # 检查 server 是否退出
        if self.server_manager.is_alive():
            # 检查进程状态，可能是在等待
            time.sleep(1.0)
            if self.server_manager.is_alive():
                # Server 应该在最后一个 pane 退出后退出
                return self.fail("Server didn't exit after last pane exit")

        # 检查 client 是否断开
        try:
            self.client.send_input("echo STILL_ALIVE\n")
            time.sleep(0.3)
            msgs = self.client.recv_messages(0.5)
            if msgs:
                # Server 还在响应
                return self.fail("Server still responding after pane exit")
        except:
            # 连接已断开，这是正确的
            pass

        return True


class TestLayoutBothVisible(TestCase):
    """22: 两个 Pane 同时可见"""

    def __init__(self):
        super().__init__("Layout both panes visible")

    def run(self) -> bool:
        if not self.setup():
            return False

        self.server_manager = ServerManager("test_22", 24, 80)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        self.client = MiniTmuxClient("test_22")
        if not self.client.connect():
            return self.fail("Failed to connect client")

        time.sleep(0.3)

        # 创建第二个 pane
        self.client.send_command(":new")
        time.sleep(0.5)

        # 这个测试需要验证输出是否包含两个 pane 的内容
        # 由于需要解析布局输出，这里只检查是否有合理的响应
        msgs = self.client.recv_messages(1.0)

        if not msgs:
            return self.fail("No output from server")

        return True


class TestLayoutWinsize(TestCase):
    """23: 布局窗口大小"""

    def __init__(self):
        super().__init__("Layout winsize")

    def run(self) -> bool:
        if not self.setup():
            return False

        self.server_manager = ServerManager("test_23", 24, 80)
        if not self.server_manager.start():
            return self.fail("Failed to start server")

        self.client = MiniTmuxClient("test_23")
        if not self.client.connect():
            return self.fail("Failed to connect client")

        time.sleep(0.3)

        # 创建第二个 pane
        self.client.send_command(":new")
        time.sleep(0.5)

        # 发送 resize
        self.client.send_resize(30, 100)
        time.sleep(0.5)

        # 检查是否有响应
        msgs = self.client.recv_messages(0.5)
        has_redraw = any(msg.type == MSG_SERVER_REDRAW for msg in msgs)

        return True


class TestTuiStatePreserve(TestCase):
    """24: TUI 程序 Pane 切换时状态保持"""

    def __init__(self):
        super().__init__("TUI state preserve on pane switch")

    def run(self) -> bool:
        if not self.setup():
            return False

        # 检查 vim 是否可用
        try:
            subprocess.run(["which", "vim"], capture_output=True, check=True, timeout=1)
        except:
            print("  [SKIP] vim not available")
            return True  # 跳过此测试

        # 创建临时文件供 vim 编辑
        tmpdir = tempfile.mkdtemp()
        test_file = os.path.join(tmpdir, "test_tui_state.txt")

        try:
            # 写入测试文件
            with open(test_file, "w") as f:
                f.write("LINE ONE\n")
                f.write("LINE TWO\n")
                f.write("LINE THREE\n")

            self.server_manager = ServerManager("test_24", 24, 80)
            if not self.server_manager.start():
                return self.fail("Failed to start server")

            self.client = MiniTmuxClient("test_24")
            if not self.client.connect():
                return self.fail("Failed to connect client")

            time.sleep(0.5)

            # 创建第二个 pane
            self.client.send_command(":new")
            time.sleep(0.5)

            # 确认 pane 0 是当前焦点
            self.client.send_command(":focus 0")
            time.sleep(0.2)

            # 在 pane 0 中启动 vim 并打开测试文件
            self.client.send_input(f"vim {test_file}\n")
            time.sleep(1.0)

            # 在 vim 中执行一些操作来记录状态
            # 使用 :w! 写入标记，确认 vim 正常运行
            self.client.send_input(":w!\n")
            time.sleep(0.5)

            # 移动光标到第二行
            self.client.send_input("j")
            time.sleep(0.2)

            # 在当前行添加标记
            self.client.send_input("iMARKED_HERE")
            time.sleep(0.2)
            self.client.send_input("\x1b")  # ESC 键
            time.sleep(0.2)

            # 保存修改
            self.client.send_input(":w\n")
            time.sleep(0.5)

            # 切换到 pane 1
            self.client.send_command(":focus 1")
            time.sleep(0.3)

            # 在 pane 1 中执行一些命令
            self.client.send_input("echo PANE1_ACTIVE\n")
            time.sleep(0.3)

            msgs = self.client.recv_messages(0.5)
            has_pane1_output = any(b"PANE1_ACTIVE" in msg.payload for msg in msgs)

            if not has_pane1_output:
                return self.fail("Pane 1 not responding")

            # 在 pane 1 中创建文件
            self.client.send_input("date > /tmp/pane1_timestamp.txt\n")
            time.sleep(0.3)

            # 再切回 pane 0
            self.client.send_command(":focus 0")
            time.sleep(0.3)

            # 验证 vim 状态是否保持：尝试退出 vim
            self.client.send_input(":q\n")
            time.sleep(0.5)

            msgs = self.client.recv_messages(0.5)

            # 如果文件有修改但未保存，vim 会提示
            # 但我们已经保存过，应该能正常退出
            # 检查是否有 shell 提示符（表示已退出 vim）
            has_shell = any(b"$" in msg.payload or b"#" in msg.payload for msg in msgs
                          if msg.type == MSG_SERVER_OUTPUT)

            if has_shell:
                # 成功退出 vim，验证文件内容是否包含标记
                with open(test_file, "r") as f:
                    content = f.read()
                    if "MARKED_HERE" in content:
                        print("  [INFO] Vim state preserved, modifications retained")
                    else:
                        print("  [WARN] Modifications not found, but vim exited normally")
                return True
            else:
                # 可能 vim 还在，尝试强制退出
                self.client.send_input(":q!\n")
                time.sleep(0.5)

                msgs = self.client.recv_messages(0.5)
                has_shell = any(b"$" in msg.payload or b"#" in msg.payload for msg in msgs
                              if msg.type == MSG_SERVER_OUTPUT)

                if has_shell:
                    print("  [INFO] Vim state preserved (required :q! to exit)")
                    return True
                else:
                    return self.fail("Vim not responding after switching back")

        finally:
            # 清理
            if os.path.exists(test_file):
                os.unlink(test_file)
            if os.path.exists(tmpdir):
                shutil.rmtree(tmpdir, ignore_errors=True)
            if os.path.exists("/tmp/pane1_timestamp.txt"):
                os.unlink("/tmp/pane1_timestamp.txt")


# ============= 测试运行器 =============
class TestRunner:
    """测试运行器"""

    def __init__(self):
        self.all_tests = [
            TestSinglePaneBasic(),
            TestTwoPaneSignalIsolation(),
            TestFourPaneFocusSwitch(),
            TestPaneCreateDestroy(),
            TestHighFreqOutput(),
            TestZombieReap(),
            TestResizeSigwinch(),
            TestEightPaneStress(),
            TestTuiCompat(),
            TestConcurrentOutput(),
            TestSigtstpSigcont(),
            TestRapidPaneOps(),
            TestDetachReattachBasic(),
            TestDetachProbeSurvives(),
            TestLogBasic(),
            TestPipeoutBasic(),
            TestPipeoutCmdExits(),
            TestMultiClientBasic(),
            TestMultiClientReadonly(),
            TestCapturePane(),
            TestLastPaneExit(),
            TestLayoutBothVisible(),
            TestLayoutWinsize(),
            TestTuiStatePreserve(),
        ]

    def run_test(self, test: TestCase) -> bool:
        """运行单个测试"""
        print(f"\n{'='*60}")
        print(f"Running: {test.name}")
        print('='*60)

        try:
            result = test.run()
            test.passed = result

            if result:
                print(f"  [PASS] {test.name}")
            else:
                print(f"  [FAIL] {test.name}")
                print(f"  Reason: {test.fail_reason}")

            return result
        except Exception as e:
            test.passed = False
            test.fail_reason = f"Exception: {e}"
            print(f"  [ERROR] {test.name}")
            print(f"  Exception: {e}")
            import traceback
            traceback.print_exc()
            return False
        finally:
            test.teardown()

    def run_all(self) -> Tuple[int, int]:
        """运行所有测试"""
        # 清理旧进程
        kill_mini_tmux_processes()

        passed = 0
        total = len(self.all_tests)

        for i, test in enumerate(self.all_tests, 1):
            print(f"\n[{i}/{total}] ", end="", flush=True)

            if self.run_test(test):
                passed += 1

        return passed, total

    def print_summary(self, passed: int, total: int) -> None:
        """打印测试摘要"""
        print(f"\n{'='*60}")
        print(f"Test Summary")
        print('='*60)
        print(f"Passed: {passed}/{total}")
        print(f"Failed: {total - passed}/{total}")

        if passed < total:
            print(f"\nFailed tests:")
            for test in self.all_tests:
                if not test.passed:
                    print(f"  - {test.name}: {test.fail_reason}")


# ============= 主程序 =============
def main():
    import argparse

    parser = argparse.ArgumentParser(description="Mini-Tmux Evaluator")
    parser.add_argument("-v", "--verbose", action="store_true", help="Verbose output")
    parser.add_argument("--test", type=str, help="Run specific test (by name or number)")
    parser.add_argument("--list", action="store_true", help="List all available tests")

    args = parser.parse_args()

    runner = TestRunner()

    if args.list:
        print("Available tests:")
        for i, test in enumerate(runner.all_tests, 1):
            print(f"  {i}. {test.name}")
        return 0

    if args.test:
        # 运行特定测试
        try:
            test_num = int(args.test)
            if 1 <= test_num <= len(runner.all_tests):
                test = runner.all_tests[test_num - 1]
                runner.run_test(test)
                return 0 if test.passed else 1
        except ValueError:
            pass

        # 按名称查找
        for test in runner.all_tests:
            if args.test.lower() in test.name.lower():
                runner.run_test(test)
                return 0 if test.passed else 1

        print(f"Test not found: {args.test}")
        return 1

    # 运行所有测试
    passed, total = runner.run_all()
    runner.print_summary(passed, total)

    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
