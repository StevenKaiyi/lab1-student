# mini-tmux 学习路线

这份文档是在 `knowledge.md` 的基础上进一步细化出来的“执行版学习计划”。

目标不是把资料堆给你，而是让你按一个可落地的顺序学习，并且每学完一块都能用最小实验验证自己确实理解了。

---

## 一、学习总原则

这个实验最怕两种学习方式：

- 只背 API，不理解系统模型
- 还没搞懂单个 pane，就急着写完整 mini-tmux

更有效的方式是：

1. 先理解系统模型
2. 再做最小实验
3. 再把实验结果和 handout 要求对应起来
4. 最后才进入正式实现

建议始终围绕这 4 个问题学习：

- 这个机制在 Unix 里本来是干什么的？
- 它在 mini-tmux 里扮演什么角色？
- 如果它没接对，会出现什么现象？
- 我能用什么最小实验验证它？

---

## 二、整体路线

建议按下面顺序学：

1. 终端、PTY、TTY、控制终端
2. 进程、进程组、会话、前台进程组
3. 信号语义
4. 单 pane 启动链：`openpty + fork + setsid + TIOCSCTTY + dup2 + exec`
5. Unix domain socket
6. `poll()` 事件循环
7. `termios` 与 raw mode
8. 布局、winsize、`SIGWINCH`
9. detach / reattach / 多 client
10. `pipe()`、日志、pipeout、capture
11. `waitpid()`、fd 清理、异常路径

原因是：

- 前 4 步决定“pane 为什么能像终端一样工作”
- 5 到 7 步决定“server/client 为什么能交互”
- 8 到 10 步决定“它为什么像 tmux”
- 11 决定“它能不能稳定通过评测”

---

## 三、阶段 1：建立系统直觉

### 目标

- 先搞懂终端复用器在系统里到底由哪些对象构成
- 能把 `tmux` 的运行结构说清楚，而不是只知道它“能分屏”

### 必懂概念

- 终端、TTY、PTY、controlling terminal
- shell、前台进程组、会话
- client / server / pane 的结构关系

### 推荐阅读

- `handout.md` 的：
  - `1.1 什么是终端复用器`
  - `3.1 架构概述`
  - `2.1 PTY 伪终端`
  - `2.2 进程 进程组与会话`
- `man 7 pty`
- `man 2 setsid`
- `man 3 tcsetpgrp`

### 最小实验

1. 在真实 `tmux` 里做体验：
   - 启动 `tmux`
   - 分两个 pane
   - 一个 pane 跑 `top`
   - 另一个 pane 跑 `ls`
   - detach 再 attach

2. 观察进程结构：
   - 在 tmux 内外分别运行 `ps -o pid,pgid,sid,tty,comm`
   - 对比 server、client、shell 的 `PGID` 和 `SID`

3. 观察 socket：
   - 启动 tmux 后查看它的 socket 文件

### 学会的标志

- 你能口头解释 `Client <-> socket <-> Server <-> PTY master <-> PTY slave <-> shell`
- 你知道 server 和 client 为什么必须分成两个进程
- 你知道 pane 为什么本质上不是“UI 区块”，而是“一个 PTY + 一个子进程”

### 自测问题

1. 为什么终端复用器要设计成 server/client，而不是单进程程序？
2. 为什么 pane 里的程序必须连接 PTY，而不能只接普通 pipe？
3. detach 后为什么 pane 里的程序还能继续运行？

---

## 四、阶段 2：学会单个 pane 是怎么活起来的

这是整个实验最重要的一阶段。

### 目标

- 独立理解并验证单 pane shell 的启动流程

### 必懂概念

- `openpty()`
- `fork()`
- `setsid()`
- `ioctl(TIOCSCTTY)`
- `dup2()`
- `exec()`
- `isatty()`

### 推荐阅读

- `handout.md` 的：
  - `2.1 PTY 伪终端`
  - `2.2 进程 进程组与会话`
  - `2.7 文件描述符 dup2 与 pipe`
  - `3.3 单 Pane 基础`
- `man 3 openpty`
- `man 2 ioctl`
- `man 2 dup2`
- `man 3 isatty`

### 最小实验

写一个极小 demo：

1. `openpty()` 创建 master/slave
2. `fork()`
3. 子进程：
   - `setsid()`
   - `ioctl(slave_fd, TIOCSCTTY, 0)`
   - `dup2(slave_fd, 0/1/2)`
   - 关闭多余 fd
   - `exec("/bin/bash")`
4. 父进程：
   - 关闭 slave
   - 从 master 读输出
   - 往 master 写输入

然后做这些验证：

- 在子 shell 里运行 `tty`
- 运行 `python3 -c "import os; print(os.isatty(0), os.isatty(1), os.isatty(2))"`
- 输入 `echo hello`
- 输入 `stty size`

### 学会的标志

- 你的 demo 能和 shell 正常交互
- `isatty()` 返回值正确
- shell 表现像终端，而不是像批处理进程

### 自测问题

1. 为什么 `setsid()` 必须在子进程里做？
2. 为什么 `TIOCSCTTY` 必须发生在正确的时机？
3. 如果父进程忘记关闭 slave，会出现什么现象？
4. 如果子进程忘记关闭 master，会留下什么问题？

---

## 五、阶段 3：搞懂信号和作业控制

很多 bug 最后都会落回这一层。

### 目标

- 搞懂为什么 `Ctrl+C` 只该影响当前 pane
- 搞懂 `SIGCHLD`、`SIGPIPE`、`SIGWINCH` 为什么都和这个实验强相关

### 必懂概念

- `SIGINT`
- `SIGTSTP`
- `SIGCONT`
- `SIGWINCH`
- `SIGCHLD`
- `SIGPIPE`
- 前台进程组
- 作业控制

### 推荐阅读

- `handout.md` 的：
  - `2.3 信号语义`
  - `3.6 焦点切换与信号投递`
  - `3.10 进程管理与资源清理`
- `man 7 signal`
- `man 2 waitpid`

### 最小实验

1. 在真实 tmux 里分两个 pane：
   - 左边运行 `cat`
   - 右边运行 `sleep 1000`
   - 在右边按 `Ctrl+C`
   - 观察左边是否受影响

2. 在普通 shell 里观察管道的进程组：
   - 启动 `cat file | grep x | wc -l`
   - 用 `ps -o pid,pgid,sid,tty,comm`

3. 写一个小程序反复 `fork()` 并快速退出子进程，观察如果不 `waitpid()` 会不会留下 zombie

### 学会的标志

- 你能解释 `Ctrl+C` 为什么是信号而不是普通字符
- 你能解释为什么不同 pane 必须属于不同 session
- 你知道 `SIGCHLD` 和 `waitpid()` 是如何配合的

### 自测问题

1. `Ctrl+C` 是谁发出的？
2. raw mode 下，`Ctrl+C` 还是不是自动变成 `SIGINT`？
3. 为什么 client 断开时 server 可能撞上 `SIGPIPE`？
4. 为什么“没 `waitpid()`”会在公开测试里直接暴露？

---

## 六、阶段 4：学会本地进程间通信

### 目标

- 搞懂 client 和 server 为什么要通过 Unix domain socket 通信

### 必懂概念

- `socket()`
- `bind()`
- `listen()`
- `accept()`
- `connect()`
- socket 文件路径
- 本地 IPC 与 TCP 的差别

### 推荐阅读

- `handout.md` 的：
  - `2.4 Unix domain socket`
  - `3.1 架构概述`
  - `3.2 命令行接口`
- `man 7 unix`

### 最小实验

写一个极小 echo server：

1. server 监听一个 Unix socket 路径
2. client 连接后发送一行文本
3. server 原样回显
4. 再扩展成同时处理多个 client

再补两个验证：

- server 退出后 socket 文件是否被清理
- 异常退出后下次 `bind()` 是否会失败

### 学会的标志

- 你能独立写出一个本地 echo server/client
- 你知道 socket 路径残留为什么会造成第二次启动失败

### 自测问题

1. 为什么这个实验更适合 Unix domain socket，而不是 TCP？
2. 如果 server 崩溃后 socket 文件没删，下次启动会怎样？
3. attach 本质上对应 socket 语义中的哪一步？

---

## 七、阶段 5：掌握事件循环

### 目标

- 理解 server 为什么必须围绕 `poll()` 或 `epoll` 组织

### 必懂概念

- I/O 多路复用
- `poll()`
- 就绪事件
- 事件循环
- 广播与输入分发

### 推荐阅读

- `handout.md` 的：
  - `2.5 I/O 多路复用`
  - `3.1 架构概述`
- `man 2 poll`
- `man 7 epoll`

### 最小实验

在上一阶段的 echo server 基础上升级：

1. 监听 socket fd
2. 接受多个 client
3. 用 `poll()` 同时收多个连接的数据
4. 谁发来的数据就回给谁

再想一步：

- 如果把“client socket”换成“PTY master fd”，这个结构是不是已经很接近 mini-tmux server？

### 学会的标志

- 你能写出最基础的 event loop 框架
- 你知道 server 主循环为什么不能简单阻塞在单个 fd 上

### 自测问题

1. `poll()` 解决的本质问题是什么？
2. server 至少要同时监听哪几类 fd？
3. 如果某个事件处理逻辑太慢，会对其他 fd 产生什么影响？

---

## 八、阶段 6：掌握 client 侧 raw mode

### 目标

- 理解为什么 client 必须接管自己的终端输入

### 必懂概念

- canonical mode
- raw mode
- `termios`
- `cfmakeraw()`
- 恢复终端状态

### 推荐阅读

- `handout.md` 的：
  - `2.6 终端 raw mode 与 termios`
  - `3.3 单 Pane 基础`
  - `3.4 命令模式`
- `man 3 termios`
- `man 3 cfmakeraw`

### 最小实验

写一个小程序：

1. 保存当前终端属性
2. 切 raw mode
3. 逐字节读取键盘输入并打印十六进制值
4. 退出前恢复终端属性

测试这些输入：

- 普通字符
- 回车
- 退格
- `Ctrl+B`
- `Ctrl+C`
- 方向键

### 学会的标志

- 你知道为什么 `Ctrl+B` 在 raw mode 下才能作为前缀键被程序自己处理
- 你知道为什么异常退出时必须恢复终端设置

### 自测问题

1. 为什么不切 raw mode，client 很难实现 tmux 风格前缀键？
2. raw mode 下 `Ctrl+C` 还是不是自动杀 client？
3. 如果程序崩溃时没恢复 termios，终端会表现成什么样？

---

## 九、阶段 7：布局、winsize、focus

### 目标

- 理解 pane 不只是多个子进程，还必须有布局和输入焦点

### 必懂概念

- pane 可视区域
- 分隔线
- focus
- `TIOCSWINSZ`
- `SIGWINCH`

### 推荐阅读

- `handout.md` 的：
  - `3.5 屏幕布局`
  - `3.6 焦点切换与信号投递`
- workload：
  - `22_layout_both_visible.yaml`
  - `23_layout_winsize.yaml`
  - `07_resize_sigwinch.yaml`

### 最小实验

可以先不写完整 mini-tmux，只做纸面推导或小程序推导：

1. 给定总行数 `H`，两个 pane 如何分配行数
2. 分隔线是否占行
3. 每个 pane 的 winsize 应该是多少
4. resize 后如何重算

然后在真实 tmux 里验证：

- 两个 pane 分别执行 `stty size`
- 拖动终端大小
- 观察数值变化

### 学会的标志

- 你能解释为什么布局计算和 PTY winsize 更新必须同步
- 你知道 focus 既影响普通输入，也影响信号投递

### 自测问题

1. 为什么布局不仅是显示问题，还是终端语义问题？
2. 为什么 resize 后不仅要重绘，还要更新各 pane 的 winsize？
3. 为什么焦点切换会影响 `Ctrl+C` 的投递目标？

---

## 十、阶段 8：高级功能

### 目标

- 在已有模型上理解 detach、多 client、log、pipeout、capture

### 推荐阅读

- `handout.md` 的：
  - `3.7 Detach 与 Reattach`
  - `3.8 多 Client 支持`
  - `3.9 高级命令`
- workload：
  - `13_detach_reattach_basic.yaml`
  - `14_detach_probe_survives.yaml`
  - `18_multi_client_basic.yaml`
  - `19_multi_client_readonly.yaml`
  - `15_log_basic.yaml`
  - `16_pipeout_basic.yaml`
  - `17_pipeout_cmd_exits.yaml`
  - `20_capture_pane.yaml`

### 建议理解方式

#### detach / reattach

先问自己：

- 如果 client 只是“显示器”，那它走了为什么 server 不能死？
- 如果 server 不死，reattach 时为什么能接回原来的 pane？

#### 多 client

先问自己：

- pane 输出为什么必须广播而不是单播？
- 两个 client 终端大小不一样时，pane winsize 应该取谁？

#### log

先问自己：

- “写到屏幕上”和“顺手复制到文件里”是不是一回事？

#### pipeout

先问自己：

- 为什么这其实是“把 pane 输出 tee 到另一个子进程的 stdin”

#### capture

先问自己：

- “当前屏幕内容”和“历史输出字节流”是不是同一件事？

### 最小实验

1. 在真实 tmux 中体验：
   - detach / attach
   - 多窗口 attach 同一个 session
   - `pipe-pane`
   - `capture-pane`

2. 写一个小实验：
   - 从一个 fd 读数据
   - 同时写到 stdout 和文件
   - 再改成同时写到 stdout 和另一个进程的 stdin

### 学会的标志

- 你能把这些高级功能都解释为“在已有数据流上加分支和生命周期管理”

### 自测问题

1. 为什么 detach 本质上要求 server 和 client 解耦？
2. 多 client 下为什么通常取最小 winsize？
3. `pipeout` 的外部命令自己退出后，为什么要自动清理？
4. capture 为什么比 log 更依赖你对“屏幕状态”的表示？

---

## 十一、阶段 9：工程质量与评测意识

### 目标

- 从“实现功能”升级到“写出能稳定通过 workload 的程序”

### 必懂概念

- `waitpid(-1, &status, WNOHANG)`
- 僵尸进程
- fd 泄漏
- 异常路径清理
- 自动化回归

### 推荐阅读

- `handout.md` 的：
  - `3.10 进程管理与资源清理`
  - `4.1 评测机制概述`
  - `4.2 公开测试用例`
  - `4.5 测试策略`
- 重点 workload：
  - `06_zombie_reap.yaml`
  - `12_rapid_pane_ops.yaml`
  - `08_eight_pane_stress.yaml`
  - `05_high_freq_output.yaml`
  - `10_concurrent_output.yaml`
  - `21_last_pane_exit.yaml`

### 最小实验

1. 写一个快速 fork/exit 的小程序，验证 zombie 回收
2. 统计某个进程打开的 fd 数量，反复创建/销毁资源，看数量是否回落
3. 模拟 client 异常断开，看 server 是否还能活
4. 模拟最后一个 pane 退出，看 server 是否能干净退出

### 学会的标志

- 你调试时不再只看“屏幕看起来对不对”，而会主动检查：
  - 进程关系
  - fd 数量
  - 僵尸状态
  - socket 文件残留

### 自测问题

1. 为什么一个程序“功能看起来对了”，仍然可能过不了评测？
2. 哪几类资源最容易泄漏？
3. 为什么“最后一个 pane 退出时系统收束”是一个单独的坑点？

---

## 十二、正式开工前的达标标准

如果你准备正式开始实现，建议至少满足下面这些条件：

### 必须达到

1. 你能解释 `Client <-> socket <-> Server <-> PTY master <-> PTY slave <-> shell`
2. 你能手写出单 pane 的启动链条
3. 你知道 `Ctrl+C`、`Ctrl+Z`、resize、client 断开、子进程退出分别对应哪些机制
4. 你能独立写一个 Unix socket + `poll()` 的最小 server
5. 你知道 raw mode 是 client 必须做的事
6. 你知道至少 4 个典型坑：
   - fd 没关
   - 没 `setsid()`
   - 没处理 `SIGPIPE`
   - 没 `waitpid()`

### 最好达到

1. 你已经做过一个 PTY demo
2. 你已经做过一个 socket + `poll()` demo
3. 你已经在真实 tmux 里观察过 detach、多 client、`pipe-pane`、`capture-pane`

---

## 十三、建议时间分配

如果你只打算花一个较集中的准备周期，可以参考这个比例：

- 阶段 1 到 3：40%
- 阶段 4 到 6：30%
- 阶段 7 到 8：20%
- 阶段 9：10%

原因是：

- 单 pane、信号、PTY、session 这些底层对象理解不到位，后面所有功能都会反复返工
- 高级功能虽然多，但大多是建立在前面模型正确的基础上

---

## 十四、最后的建议

对这个实验，最有效的学习方式不是“尽量多看资料”，而是每学一个机制就立刻做一个 20 到 50 行的最小实验。

你真正需要建立的是下面这条能力链：

`看到功能要求 -> 能映射到系统对象 -> 能猜到会踩什么坑 -> 能设计最小实验验证`

如果这条链建立起来，后面的正式实现会顺很多。
