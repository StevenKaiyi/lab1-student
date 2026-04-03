# mini-tmux 实验 Tips

这份笔记根据 `screenshots/` 中的 6 张截图整理，保留了图片里的文字信息，也把图示中表达的结构关系补成了可执行的提示。

## 1. 先理解 tmux 是什么

- `tmux` 的核心链路是：`Client <-> Unix socket <-> Server <-> PTY master <-> PTY slave <-> shell`
- `Server` 常驻后台，`Client` 可来可走，所以断开连接后任务仍能继续跑。
- `Server` 的 event loop 需要同时监听两类输入源：
  - client socket 上来的控制命令和按键
  - PTY master 上来的 pane 输出
- 截图里的结构图强调的是共享会话模型：多个 client 可以连到同一个 server / session。

## 2. 从最小骨架开始，不要一口气写完

推荐顺序：

`单 Pane I/O -> 命令模式 -> 多 Pane 布局 -> 信号隔离 -> detach/attach -> 高级命令`

补充原则：

- 每一步都应该有可验证的里程碑。
- 不要一开始就试图实现完整版 mini-tmux。
- 先把两个最小 demo 跑通：
  - 一个约 20 行的 PTY demo：`openpty + fork + exec bash`
  - 一个约 30 行的 echo server：`Unix socket + poll`
- 这两个都能稳定工作后，再把它们组装成 mini-tmux 的基本骨架。

## 3. 关键实现点：PTY / session / controlling terminal

截图里特别强调了提问粒度要足够细。比起“帮我实现 mini-tmux”，更有效的问题是：

`PTY 创建后，子进程 setsid -> TIOCSCTTY -> dup2 的正确顺序是什么？为什么？`

这说明实现时要把问题拆到系统调用级别。

对 pane 子进程来说，常见正确思路是：

1. `fork()`
2. 子进程里 `setsid()`，先脱离原来的会话
3. 把 PTY slave 设为 controlling terminal：`ioctl(slave_fd, TIOCSCTTY, ...)`
4. 再用 `dup2()` 把 slave 接到 `stdin/stdout/stderr`
5. `exec()` 启动 shell 或目标程序

原因是：

- 没有先 `setsid()`，子进程通常不能正确拿到新的 controlling terminal。
- pane 需要有独立的 session / process group，这样 `Ctrl+C`、`Ctrl+Z` 之类的终端信号才不会串到别的 pane。

## 4. 自建反馈闭环

截图给出的建议很直接：

- 写一个小脚本：启动 `mini-tmux` -> 发送按键 -> 检查输出
- 让 AI 在“生成 -> 运行 -> 看报错 -> 修”这个循环里自主迭代
- 你当监工，不当搬砖工

对应到实验里，可以理解为：

- 不要只靠人工点按键试。
- 尽早做出可重复运行的最小验证脚本。
- 每实现一个阶段，就立刻跑回归验证，而不是积到最后一起查错。

## 5. 常见陷阱

截图明确列了 4 个高频坑：

- `fork` 后忘记关闭多余 `fd`
  - 后果：pane 退出时，PTY master 读不到 EOF
- 没有 `setsid`
  - 后果：信号投递到错误的进程组
- 没处理 `SIGPIPE`
  - 后果：client 断开时 server 可能直接崩溃
- 没有 `waitpid`
  - 后果：僵尸进程堆积

可以把它们当作调试 checklist：

- PTY EOF 不到？先查两端还有谁持有相关 fd。
- `Ctrl+C` 行为异常？先查 session / process group / controlling terminal。
- client 退出后 server 跟着死？先查 `SIGPIPE`。
- 长时间运行后进程越来越多？先查 `SIGCHLD` 和 `waitpid`。

## 6. 提问和拆解方式会直接影响 AI 产出质量

截图第一页的核心结论：

- 拆问题的粒度决定 AI 输出质量
- 差的提问：`帮我实现 mini-tmux`
- 更好的提问：`PTY 创建后，子进程 setsid -> TIOCSCTTY -> dup2 的正确顺序是什么？为什么？`
- 拆得好，前提是你对系统整体结构有全局认知

对这个实验尤其适用，因为它把这些知识点缠在一起了：

- PTY
- session / process group
- Unix domain socket
- poll / event loop
- 信号处理
- detach / attach

## 7. 可以直接照着执行的实验策略

- 先独立写 PTY demo，确认 shell 能正常交互。
- 再独立写 Unix socket + `poll()` 的 server/client demo。
- 然后只做一个 pane，把 socket 输入转发到 PTY，把 PTY 输出回传给 client。
- 单 pane 跑稳后，再加命令模式和 pane 管理。
- 多 pane 稳定后，再做信号隔离、detach/attach、多 client。
- 每一步都配一个自动化小脚本做回归。

## 8. 一句话总结

这个实验最重要的不是“尽快写很多代码”，而是先把系统拆成若干可验证的小问题，然后围绕 `PTY + socket + event loop + signal/session` 逐步拼起来。
