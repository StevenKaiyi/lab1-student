# Claude Review Notes

## 范围

这份文档只记录本轮 Claude Code 评审中“有价值、值得后续跟踪”的意见。
明显误判或当前阶段并不成立的问题，不在这里展开。

## 1. Server 父进程回收不完整

- 现象：`start_server_and_attach()` 中 fork 出 server 子进程后，父进程没有显式 `waitpid()` 回收它。
- 风险：如果 server 先退出、client 父进程仍短暂存活，理论上可能出现 zombie 进程窗口。
- 当前判断：这是一个真实的清理问题，但更偏工程完整性问题，不是当前阶段最致命的功能 bug。
- 后续建议：
  - 可以在父进程中增加 `SIGCHLD`/`waitpid()` 回收逻辑，或
  - 改成更明确的 daemon 化/双重 fork 方案。

## 2. Client 异常退出时终端恢复不足

- 现象：`TerminalModeGuard` 依赖正常控制流和析构来恢复 raw mode。
- 风险：如果 client 因 `SIGSEGV`、`SIGABRT` 等异常信号退出，析构不会执行，用户终端可能残留在 raw mode。
- 当前判断：这是一个真实问题，而且和当前计划中 `C1` 尚未完成的“异常退出路径恢复策略”一致。
- 后续建议：
  - 为 client 增加异常信号下的终端恢复机制；
  - 至少覆盖常见致命信号下的 termios 恢复。

## 结论

当前这两条意见值得保留并在后续迭代中处理：

1. server 子进程回收
2. client 异常退出时的终端恢复

## 3. 多 Client 尺寸策略缺失

- 现象：server 收到任意一个 client 的 `kClientResize` 后，会直接覆盖 `session.size`，再对全部 pane 重新分发布局尺寸。
- 风险：多个终端以不同窗口大小同时 attach 时，谁最后发 resize，谁就改写整个 session 的全局尺寸；行为不稳定，也不适合作为明确语义。
- 当前判断：这是一个真实的功能缺口，而且已经在 `plan.md` 的 `K3` 中列为未完成项。
- 后续建议：
  - 明确尺寸仲裁策略，例如“仅主 client 生效”“取最小公共尺寸”或“首个读写 client 优先”；
  - 把 attach/detach 后的尺寸重算也纳入同一套规则。

## 4. 前台进程组 / 信号隔离仍未完成

- 现象：pane 子进程已经 `setsid()` 并绑定控制终端，但当前实现仍没有补齐多 pane 下的前台进程组管理策略。
- 风险：`Ctrl+C`、`Ctrl+Z`、`SIGCONT` 这类交互信号的目标语义还不够明确，后续如果要做到“只影响焦点 pane”，当前实现还不够。
- 当前判断：这是一个真实缺口，和 `plan.md` 中 `I1-I3` 的未完成项一致；它不是当前最小多 pane 功能的阻塞点，但会影响后续评测和交互正确性。
- 后续建议：
  - 明确 server/client 各自负责的信号转发边界；
  - 增加针对焦点 pane 的前台进程组管理与回归测试。

## 5. 递归 Attach 保护仍然缺失

- 现象：目前仍缺少“在已经 attach 的 pane 内再次 attach 同一 session”的防护。
- 风险：这会导致已知的输出回环问题，用户在 pane 内递归 attach 自己时可能出现刷屏和难以交互的情况。
- 当前判断：这是一个真实问题，`debug.md` 和 `plan.md` 都已经记录过，不是误报。
- 后续建议：
  - 在 attach 前检测当前终端是否已经属于本 session；
  - 命中时直接拒绝 attach，并给出明确提示。

## 6. `:log / :pipeout / :capture` 仍是硬缺口

- 现象：这三组命令当前都还没有实现。
- 风险：如果后续评测或验收覆盖这些 handout 要求，当前版本会直接缺功能失败。
- 当前判断：这不是“隐藏 bug”，但确实是当前版本与完整目标之间最明显的差距，值得在 review 文档里保留，方便排优先级。
- 后续建议：
  - 优先补 `:log`，因为它的状态和资源模型最简单；
  - 再处理 `:pipeout` 的子进程生命周期；
  - `:capture` 最后做，因为它依赖更明确的屏幕表示。

## 不采纳的点

以下几类意见这次不写成正式 review 结论：

- “server.clients 退出清理循环有未定义行为”：当前证据不足，不是一个成立的高价值问题。
- “忽略 SIGPIPE 本身是 bug”：这更像权衡，不足以单独列为问题。
- “backlog 1MB 一定过小”或“必须立刻做 layout 缓存”：属于优化建议，不是当前阶段最有价值的缺陷。
- “结构化渲染完全未实现”：当前说法不准确，已有简化版 redraw/render 链路，只是终端语义还不完整。

## 7. 多 Pane 渲染问题：tmux 方案适用性分析

### 问题概述

当前多 pane 渲染存在以下 bug：

- 光标位置错误（ANSI 序列解析不完整）
- 高频输出时卡顿（每次完整重绘）
- 多 pane 并发输出导致画面撕裂
- 带颜色输出显示异常

### tmux 渲染方案在 mini-tmux 的适用性

| 方案 | 适用性 | 推荐度 | 理由 |
|------|--------|--------|------|
| 服务端虚拟屏幕 | ❌ 不适合 | 0/5 | 复杂度过高，与 lab “从零实现” 的初衷不符 |
| 差分渲染 | ✅ 适合 | 4/5 | 可以显著改善性能，实现难度中等 |
| 服务端 ANSI 解析 | ⚠️ 部分适合 | 2/5 | 只需补全常用序列，不必实现完整终端模拟器 |
| 服务端计算 layout | ✅ 已实现 | 5/5 | 已经在做，这是正确的方向 |
| 简化客户端职责 | ⚠️ 需配合 | 3/5 | 需要服务端做更多工作 |

### 推荐的改进方案（按优先级）

#### 阶段 1：快速修复（低成本，2-4 小时）

| 修改 | 预期解决 | 代码量 |
|------|---------|--------|
| 添加 CSI 参数解析 | 光标位置错误 | ~50 行 |
| 补全常用序列（A/B/H/J） | TUI 程序兼容性 | ~80 行 |
| 过滤 SGR（颜色序列） | 颜色异常显示 | ~20 行 |
| 输出节流（合并多次输出后渲染） | 高频输出卡顿 | ~30 行 |

#### 阶段 2：性能优化（中成本，6-10 小时）

| 修改 | 预期解决 | 代码量 |
|------|---------|--------|
| 差分渲染（记录上一帧） | 画面撕裂、CPU 占用 | ~200 行 |
| 智能刷新（只在有变化时渲染） | 减少无效重绘 | ~50 行 |
| 批量消息处理 | 多 pane 并发输出混乱 | ~100 行 |

#### 阶段 3：架构改进（高成本，可选，10-15 小时）

| 修改 | 预期解决 | 代码量 |
|------|---------|--------|
| 服务端输出缓冲 | detach/reattach 状态一致性 | ~150 行 |
| 客户端状态简化 | 减少客户端复杂度 | 重构客户端 |

### 具体实现建议

#### 1. 补全 ANSI CSI 序列（必须）

在 `handle_csi_final()` 中添加参数解析：

```cpp
void handle_csi_final(ClientPaneBuffer &buffer, char final_byte,
                   const std::string &params) {
    int n = 1;  // 默认参数
    if (!params.empty()) {
        n = std::stoi(params);
        if (n < 1) n = 1;
    }

    switch (final_byte) {
        case 'A': buffer->cursor_col -= n; break;  // 上移
        case 'B': buffer->cursor_col += n; break;  // 下移
        case 'C': buffer->cursor_col += n; break;  // 右移（已有，需改）
        case 'D': buffer->cursor_col -= n; break;  // 左移（已有，需改）
        case 'H': /* 光标定位 <row>;<col>H */ break;
        case 'J': /* 清屏 */ break;
        case 'm': /* SGR，至少过滤掉 */ break;
    }
}
```

#### 2. 输出节流（应该做）

在事件循环中添加消息队列，合并多个输出消息后统一渲染：

```cpp
// 在 ClientViewState 中添加
std::vector<Message> pending_outputs;

// 收到输出时先入队，暂不渲染
if (message.type == MessageType::kServerOutput) {
    view_state.pending_outputs.push_back(message);
}

// 定时或在有其他事件时批量处理并渲染
void flush_pending_outputs() {
    for (auto &msg : pending_outputs) {
        append_client_output(view_state, msg.arg0, ...);
    }
    pending_outputs.clear();
    render_client_view(view_state, command_buffer, input_mode);
}
```

#### 3. 差分渲染（可以做）

记录上一帧，只在有变化时渲染：

```cpp
struct ClientViewState {
    // ... 现有字段 ...
    std::string last_rendered_frame;
};

bool render_client_view_diff(const ClientViewState &view, ...) {
    std::string new_frame = build_full_frame(view, ...);

    if (new_frame != view.last_rendered_frame) {
        write_stdout(new_frame);
        view.last_rendered_frame = new_frame;
    }
}
```

### 不推荐的方向

| 方案 | 不推荐原因 |
|------|-----------|
| 完整终端模拟器 | 复杂度过高，超出 lab 范围 |
| ncurses/libtickit | lab 要求不得使用第三方库 |
| 完全重写渲染层 | 风险太大，可能破坏已通过的功能 |

### 核心原则

在 lab 范围内，用最小的改动解决最明显的问题。差分渲染 + ANSI 补全足以覆盖 80% 的渲染问题。

### 验证策略

```bash
# 修改后验证
python3 test_single_pane.py       # 基础 I/O
python3 test_interactive_render.py # 交互渲染
python3 test_multi_pane_layout.py  # 多 pane 布局
python3 test_focus_prefix.py       # 焦点切换
python3 test_detach_reattach.py    # 会话管理
```