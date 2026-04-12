# 报告内容规划

根据现在这份代码，我建议报告不要按“功能清单”写，而是围绕 2 个主概念写透，再用 1 个小案例做支撑。这样最贴合第 5 部分的评分标准，也最符合你这份实现的强项。

我刚在 WSL 里跑了公开评测，23 个 workload 过了 22 个，唯一失败是 `18_multi_client_basic`。所以这份报告最稳的写法是：把主轴放在 Unix 机制理解上，不把“多 client 同步”当成最主要亮点。

## 推荐主线

1. `PTY + 控制终端 + 进程组 + 信号`

这块最适合拿来冲“系统理解”分。你的代码里从 `openpty`、`setsid`、`TIOCSCTTY`、`dup2` 到 `tcsetpgrp` 是一条完整链路，[mini_tmux.cpp](/home/kaiyi/lab1-student/mini_tmux.cpp#L1720)；而真正把 `Ctrl+C`、`Ctrl+Z`、`SIGWINCH` 投递到前台进程组的是另一条链路，[mini_tmux.cpp](/home/kaiyi/lab1-student/mini_tmux.cpp#L1544) 和 [mini_tmux.cpp](/home/kaiyi/lab1-student/mini_tmux.cpp#L2361)。

这部分可以回答几个很强的问题：为什么 shell 必须挂在 PTY slave 上、为什么要建立新的 session、为什么信号不能随便发给整个 server、为什么 resize 不只是改个变量。

2. `poll 驱动的 server 事件循环 + Unix socket 消息协议`

这是整套系统的骨架。server 把监听 socket、`SIGCHLD` 的 self-pipe、所有 pane 的 master fd、所有 client fd 都挂进一个 `poll` 循环里，[mini_tmux.cpp](/home/kaiyi/lab1-student/mini_tmux.cpp#L2152)。

这块适合讲“我真正理解了什么是事件驱动系统”：信号处理函数里不能做复杂逻辑，所以先写一个 byte 到 pipe，再回到主循环里 `reap_children`；client 发来的 `hello`、`resize`、`input`、`command` 在同一个协议里分流处理。这个角度比单纯说“我用了 poll”要深得多。

3. `layout/redraw/output fan-out` 作为辅助案例

这块不一定当主角，但很适合当“代码里怎么落地”的证据。

布局和重绘在 [mini_tmux.cpp](/home/kaiyi/lab1-student/mini_tmux.cpp#L1282) 和 [mini_tmux.cpp](/home/kaiyi/lab1-student/mini_tmux.cpp#L1369)；pane 输出同时进入屏幕缓存、日志和 `pipeout` 在 [mini_tmux.cpp](/home/kaiyi/lab1-student/mini_tmux.cpp#L1242)；广播给同 session 的 client 在 [mini_tmux.cpp](/home/kaiyi/lab1-student/mini_tmux.cpp#L2055)。

这部分可以用来说明：你不是写了一堆互不相关的命令，而是把输出路径做成了统一的数据流。

## 不建议当主轴的内容

- `:log` / `:pipeout` / `:capture`

这些适合当“统一输出管线”的例子，不适合单独撑起报告中心。

- 多 client

你可以写，但更适合放在“诚实反思”里。因为从评测结果看，这部分总体有实现，但还有同步细节没完全稳住。更准确地说，我推测问题更像“新 attach 的额外 client 初始同步时序”而不是整个 resize/readonly 机制全坏。

## 我建议的报告结构

1. 开头 1 小节

你本来以为 `tmux`/`mini-tmux` 只是“转发输入输出”，后来发现核心其实是 PTY、进程组、socket、事件循环。

2. 主体 A

讲 PTY、控制终端、前台进程组和信号。这里最容易讲出“为什么必须这样做”。

3. 主体 B

讲 server 的 `poll` 循环和消息协议。这里最容易讲出“系统为什么能同时处理 pane、client、child exit”。

4. 辅助案例

用 layout/redraw 或 output fan-out 证明你的理解确实落到了代码上。

5. 反思

写你一开始没懂什么、AI 在哪种地方最容易给出“能编译但语义不对”的方案、你怎么靠 workload 和公开测试逼近问题。

这里可以写两个很真实的点：

- Windows Python 跑 `evaluator.py` 直接缺 `termios`，最后必须在 WSL 下测，这让你意识到这个实验依赖的是完整 Unix 终端语义。
- 多 client 相关功能大部分通了，但 `18_multi_client_basic` 还暴露了 attach/同步时序问题，所以你不打算在报告里把它包装成“完全解决”。

如果你认同这个方向，我下一步可以直接把这套规划细化成“报告提纲”，包括每一节要回答的具体问题和建议写哪些代码片段。
