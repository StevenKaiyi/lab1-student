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
