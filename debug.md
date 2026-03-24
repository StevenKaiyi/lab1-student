# mini-tmux 调试记录

## 单 pane 基线验证

在 VSCode 的 WSL 终端中运行：

```bash
./mini-tmux
```

程序可以正常启动，并进入 mini-tmux 管理的单 pane shell。

对应的基础回归测试：

```bash
python3 test_single_pane.py
```

当前这组测试已经通过，说明单 pane 的 PTY、I/O 转发和退出清理链路基本成立。

## 已验证的行为

`test_single_pane.py` 当前覆盖并通过的检查包括：

- shell ready check
- pty path check
- isatty check
- winsize check
- io round-trip check
- top compatibility check
- vim compatibility check
- fork_exit/zombie reap check
- exit/cleanup check

## 关于 `./mini-tmux` 的 attach 语义

当前 `main()` 的行为是：

- 如果直接运行 `./mini-tmux`，会先检查默认 socket 是否已经存在
- 如果默认 session 已存在，则自动 attach 到已有 server
- 如果默认 session 不存在，则启动一个新的 server，并 attach 到它

因此，`./mini-tmux` 现在的语义不是“无条件新建 session”，而是“有 session 就 attach，没有就创建并 attach”。

这也解释了为什么在某些情况下再次运行 `./mini-tmux`，看到的是进入已有会话，而不是新开一个全新的 pane。

## 如何确认自己已经进入 mini-tmux

进入后，可以先看到本地提示：

- `mini-tmux: attached`
- 只读 attach 时为 `mini-tmux: attached in read-only mode`

退出时会看到：

- `mini-tmux: detached`

在 pane 内也可以继续做这些检查：

```bash
tty
echo $$
python3 -c "import os; print(os.isatty(0), os.isatty(1), os.isatty(2))"
```

如果 `tty` 指向的是一个 PTY，且三个标准流都是 TTY，说明当前 shell 确实跑在 mini-tmux 的 pane 里。

## 递归 attach 导致输出回环

现象：已经进入 `mini-tmux` 的 pane 后，又在同一个 pane 里执行 `./mini-tmux attach`，终端会反复打印同样的命令和 shell 错误输出，表现为刷屏且难以终止。

原因：第二个 `mini-tmux` client 的 `stdin/stdout` 绑定到了第一个 session 的 pane PTY。server 把 pane 输出发给 client，client 又把内容写回同一个终端，形成输出回环。

结论：`./mini-tmux attach` 必须从另一个普通终端启动，不能在已经 attach 的 pane 内再次启动。后续应补一层保护，检测当前终端是否已经是本 session 的 pane，避免递归 attach。

## Windows PowerShell 直接运行 ELF 的问题

如果在 Windows PowerShell 中，直接从 `\\wsl.localhost\...` 路径执行：

```powershell
./mini-tmux
```

Windows 不会把它当作 Linux 程序交给 WSL 执行，而是把它当成普通文件，从而弹出“选择用什么程序打开”的窗口。

这不是 `mini-tmux` 运行时逻辑的问题，而是 Windows 无法直接执行 Linux ELF 文件。

正确做法是先进入 WSL，再运行：

```powershell
wsl
cd /home/kaiyi/lab1-student
./mini-tmux
```

或者一条命令：

```powershell
wsl bash -lc "cd /home/kaiyi/lab1-student && ./mini-tmux"
```

## 当前阶段的结论

当前项目已经完成并验证了单 pane 的核心闭环：

- client raw mode
- PTY 启动 shell
- 输入透传
- 输出回传
- resize 基础处理
- 退出清理

接下来的主线仍然是：

1. 修稳 `Ctrl+B` 前缀状态机
2. 完成 `Ctrl+B :` 命令模式
3. 实现最小命令执行路径，例如 `:new` / `:focus` / `:kill`

## `./mini-tmux` 进入后立刻 detached

现象：在 WSL 终端里直接运行：

```bash
./mini-tmux
```

终端只短暂打印：

- `mini-tmux: attached`
- `mini-tmux: detached`

随后立刻返回 shell，看起来像“刚进入就退出了”。

原因：当前 `main()` 的默认语义不是“无条件新建 session”，而是：

- 先尝试连接默认 socket
- 如果默认 session 已存在，就直接 attach
- 如果默认 session 不存在，才启动新的 server 并 attach

因此，当 `/tmp/mini-tmux-<uid>-default.sock` 仍然存在、且有旧的默认 session/server 残留时，`./mini-tmux` 会优先 attach 到这个已有 session，而不是新建一个干净会话。

这次排查时，WSL 中可以看到：

- 默认 socket 仍然存在：`/tmp/mini-tmux-1000-default.sock`
- 还有多个历史 `mini-tmux` 进程残留

在这种情况下，如果 attach 到的是一个状态异常、空壳、或即将关闭连接的旧 session，client 就会先打印 `attached`，随后因为连接关闭或 session 结束而立即打印 `detached`。

结论：这类“进入就立刻 detached”的现象，不一定是新启动的 session 立即崩掉，更常见的是误 attach 到旧的默认 session。

临时规避方式：

```bash
MINI_TMUX_SERVER=fresh ./mini-tmux
```

这样会使用新的实例名，避免误连到旧的默认 session。

后续建议：

- 增加对默认 session 健康状态的判断
- 或提供更明确的“强制新建 session”入口
- 或在调试/测试前先清理残留 socket 和旧 server 进程
## 单 pane 交互渲染异常

现象：进入 `./mini-tmux` 后，单 pane shell 交互存在几类明显异常：

- prompt 前会直接漏出类似 `[?2004h]0;kaiyi@...` 的控制序列
- 光标停在窗口最底部，而不是当前 shell 输入位置
- 未进入 `Ctrl+B` 前缀时，backspace 等编辑键表现不正常
- 例如输入 `tty` 后按 backspace，屏幕上会出现类似 `tty[K` 的脏输出

原因：早期 client 对所有 pane 输出都走“简化结构化渲染”。这套渲染只会粗略过滤部分控制字符，但不会正确解析真实 shell/PTTY 输出里的终端控制序列，例如：

- bracketed paste：`\x1b[?2004h` / `\x1b[?2004l`
- OSC 窗口标题：`\x1b]0;...\x07`
- 颜色控制序列：`\x1b[01;32m`
- 行编辑相关序列：`\b\x1b[K`

这些字节本来应该由真实终端解释，但被 mini-tmux client 当成普通文本或半截文本处理后，就会直接显示到屏幕上，进而导致：

- 控制序列外漏
- 光标位置计算错误
- shell 自己的行编辑效果被破坏

修复：将 client 的渲染策略拆成两种模式：

- 单 pane：直接透传 PTY 原始输出到当前终端
- 多 pane：继续使用结构化重绘

这样做的原因是：

- 单 pane 时，最重要的是保持“像真实终端一样”的 shell 交互语义
- 多 pane 时，必须由 client 自己负责布局和重绘，因此仍需要结构化渲染

另外，`Ctrl+B :` 命令模式在单 pane 下不再强制整屏重绘，而是单独绘制和清除底部命令提示，避免破坏当前 shell 光标与屏幕状态。

验证结果：

- `python3 test_interactive_render.py` 已通过
- `python3 test_single_pane.py` 已通过
- `python3 test_focus_prefix.py` 已通过
- `python3 test_detach_reattach.py` 已通过
- `python3 test_multi_pane_layout.py` 已通过

结论：这组修复不违背 handout。handout 要求的是：

- Client 必须进入 raw mode，以便拦截 `Ctrl+B`
- Client 退出时必须恢复终端

handout 并没有要求把 shell 的终端控制序列显示给用户看。相反，单 pane 直通 PTY 输出更符合真实终端语义，也更符合交互预期。
## 多 pane 重绘污染 scrollback

现象：进入 `mini-tmux` 后，只要新建 pane、切换焦点或触发重绘，终端窗口往上翻就能看到大量重复的历史“画面快照”，例如：

- 一长条分隔线 `-----...`
- `Pane 1 [active]`
- `Pane 0`
- pane 内当时的 shell prompt

这些内容会在 scrollback 里累积很多屏，看起来像每次重绘都被当成了新的终端输出追加上去。

原因：多 pane 模式下，client 需要自己负责整屏布局和重绘；之前的实现虽然每次都用 `\x1b[H\x1b[2J` 回到左上角并清屏，但仍然是在终端的主屏缓冲区里做整屏刷新。对于普通终端来说，这些整屏内容依然属于“正常输出历史”，因此会被保存进 scrollback。

所以问题不在于分隔线本身，也不在于 pane 内容重复发送，而在于：

- 多 pane 重绘没有切到 alternate screen
- 导致每一帧布局快照都污染了主终端的滚动历史

修复：为结构化多 pane 渲染增加 alternate screen 管理：

- 进入多 pane 结构化渲染时，client 发送 `\x1b[?1049h`
- 退出结构化渲染、回到单 pane 直通模式或 client 退出时，发送 `\x1b[?1049l`
- 之后所有多 pane redraw 都在 alternate screen 内原位刷新，而不是堆进主屏 scrollback

实现后的语义是：

- 单 pane：保持 PTY 直通，行为尽量接近真实 shell
- 多 pane：进入类似全屏 TUI 的独立渲染表面，由 client 自己控制布局

已重新验证的回归：

- `python3 test_focus_prefix.py`
- `python3 test_detach_reattach.py`
- `python3 test_multi_pane_layout.py`
- `python3 test_interactive_render.py`

结论：这个问题本质上是“渲染表面选错了”，不是 pane 生命周期错误。多 pane 结构化重绘必须使用 alternate screen，否则 scrollback 被历史帧淹没几乎是必然结果。