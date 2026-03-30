 # mini-tmux Implementation Plan Fixed

  本计划以 `handout_fixed.md` 为准；如果与 `handout.md` 冲突，以 `handout_fixed.md` 为准。

  这份文档只围绕 `handout_fixed` 的主线目标组织：

  1. 单 Session / 单 PTY shell 跑通
  2. 拆成 Client / Server
  3. 稳定 Client 的 raw mode、输入、resize
  4. 稳定 Server 的事件循环、子进程和 fd 生命周期
  5. 做对 Detach / Reattach
  6. 做对多 Session
  7. 做对多 Client 和只读 Client

  不再把多 pane/layout/log/pipeout/capture 作为主线目标；这些如果代码里已有残留结构，可以保留，但不应继续主导实现方向。

  ---

  ## 0. 当前基线

  ### 已完成

  - [x] 单一可执行文件 `./mini-tmux`
  - [x] Unix domain socket 基础通信
  - [x] `poll()` 事件循环骨架
  - [x] PTY + shell 启动链路
  - [x] Client raw mode 基础链路
  - [x] 单 session 基础 IO
  - [x] `attach`
  - [x] `attach -r`
  - [x] 基础多 session 状态管理
  - [x] 基础多 client 连接

  ### 当前公开评测结果

  - [x] Single pane basic IO
  - [x] Session create and destroy
  - [x] High frequency output stress
  - [x] TUI program compatibility
  - [x] Concurrent output from 2 sessions
  - [x] Readonly client cannot send input
  - [x] Server exits cleanly when last pane's shell exits
  - [ ] Signal Ctrl+C isolation with 2 sessions
  - [ ] 4 sessions with independent IO and signal isolation
  - [ ] Zombie process reaping
  - [ ] 8 session creation stress
  - [ ] SIGTSTP and SIGCONT delivery
  - [ ] Rapid session create/destroy cycle
  - [ ] Detach and reattach basic
  - [ ] Probe survives detach and IO works after reattach
  - [ ] Two clients see same output

  ### 当前主判断

  - [x] 主路径已经接近 `handout_fixed` 要求
  - [ ] 还没有把“会话隔离、僵尸回收、detach 后 server 存活、多 client 输入语义”做稳
  - [ ] 当前代码仍保留不少来自旧 `pane/layout` 方向的复杂度，后续要避免继续沿那个方向扩展

  ---

  ## 1. 阶段 A：单 Session / 单 PTY Shell

  目标：先把 “Server 创建一个 PTY，里面跑 bash，Client 能正常交互” 这条链彻底做稳。

  ### A1. PTY 启动链

  - [x] `openpty()`
  - [x] `fork()`
  - [x] 子进程 `setsid()`
  - [x] 子进程 `ioctl(TIOCSCTTY)`
  - [x] 子进程 `dup2()` 到 `stdin/stdout/stderr`
  - [x] `exec()` 默认 shell

  ### A2. 基础自测

  - [x] `tty`
  - [x] `isatty()`
  - [x] `stty size`
  - [x] 普通命令 round-trip
  - [x] `vim` / `top` 可运行
  - [ ] 用 `helpers/probe` 长时间复核单 session 环境稳定性

  ### A3. 退出与清理

  - [x] shell 退出后主进程能感知
  - [x] 最后一个 session 退出后 server 收尾
  - [ ] 确认不存在额外 zombie
  - [ ] 确认没有 socket 残留

  ---

  ## 2. 阶段 B：拆成 Client / Server

  目标：让 PTY 和 shell 始终归 Server 管，Client 只做前台交互。

  ### B1. Server 职责

  - [x] 持有监听 socket
  - [x] 持有各 session 的 PTY master
  - [x] 接受 client 连接
  - [x] 转发 client 输入到目标 session
  - [x] 转发 session 输出到 attached client

  ### B2. Client 职责

  - [x] 从键盘读取输入
  - [x] 通过 socket 发给 server
  - [x] 从 socket 收输出
  - [x] 显示到本地终端

  ### B3. 结构约束

  - [x] 一个 server 管多个 session
  - [x] client 不直接管理 PTY
  - [ ] 删除或降级与 `handout_fixed` 主线无关的复杂渲染依赖

  ---

  ## 3. 阶段 C：Client 交互稳定性

  目标：把 client 变成一个稳定的 raw mode 双向搬运工。

  ### C1. raw mode

  - [x] 进入 raw mode
  - [x] 正常退出恢复 termios
  - [ ] 异常退出恢复策略补强

  ### C2. 输入

  - [x] 普通输入透传
  - [x] `Ctrl+B` 前缀状态机
  - [x] `Ctrl+B d`
  - [x] `Ctrl+B n/p`
  - [ ] 检查前缀态是否会吞掉或误转发 `Ctrl+C` / `Ctrl+Z`

  ### C3. resize

  - [x] Client 捕获 `SIGWINCH`
  - [x] 发送新窗口大小给 server
  - [ ] 验证 attach / detach / 多 client 下 resize 语义一致

  ---

  ## 4. 阶段 D：Server 事件循环与生命周期

  目标：把最容易丢分的系统行为做对。

  ### D1. poll 事件循环

  - [x] 监听 listen socket
  - [x] 监听 client socket
  - [x] 监听 session PTY master
  - [x] 监听 `SIGCHLD` 自唤醒 pipe

  ### D2. 子进程回收

  - [x] `SIGCHLD` handler
  - [x] `waitpid(-1, &status, WNOHANG)` 基础链路
  - [ ] 确认每轮循环都能把所有已退出子进程回收干净
  - [ ] 确认 session 高频创建/销毁时不会残留 zombie

  ### D3. fd 生命周期

  - [x] client 断开时关闭 socket
  - [x] pane 结束时关闭 master fd
  - [ ] 子进程里关闭继承来的多余 fd
  - [ ] server 退出时清理 socket 文件

  ---

  ## 5. 阶段 E：Detach / Reattach

  目标：达到 `handout_fixed` 的核心收益点。

  ### E1. detach

  - [x] `Ctrl+B d` 触发本地 detach
  - [ ] detach 后只移除 client，不影响 server 和 session
  - [ ] detach 后 probe / shell 继续运行

  ### E2. reattach

  - [x] `./mini-tmux attach`
  - [ ] 新 client attach 后 server 仍存活
  - [ ] reattach 后 probe 继续存在
  - [ ] reattach 后 IO 仍然连通

  ### E3. 无 client 状态

  - [ ] 所有 client 断开后 server 继续存活
  - [ ] 新 client 之后仍可 attach 回原 session

  ---

  ## 6. 阶段 F：多 Session

  目标：把 `handout_fixed` 主线里的 session 级隔离做对。

  ### F1. session 管理

  - [x] 创建新 session
  - [x] 列出 session
  - [x] attach 到指定 session
  - [x] session shell 退出时销毁该 session

  ### F2. IO 独立性

  - [x] 多 session 并发输出基本可用
  - [ ] 每个 session 的 probe 都能稳定 ready
  - [ ] 8 session 压力下仍能稳定 ready

  ### F3. 信号隔离

  - [ ] `Ctrl+C` 只作用于目标 session
  - [ ] `Ctrl+Z` 只作用于目标 session
  - [ ] 必要时补齐前台进程组控制与验证

  ---

  ## 7. 阶段 G：多 Client

  目标：满足 `handout_fixed` 的多 client 语义。

  ### G1. 输出广播

  - [x] 多 client 可同时 attach
  - [x] attached client 都能收到输出
  - [ ] 额外 client detach 后主 client 输入仍然正常

  ### G2. 输入语义

  - [x] 只读 client 不能发输入
  - [ ] 普通多 client 情况下输入路由仍然稳定
  - [ ] client attach / detach 后不会把 session 尺寸或输入状态搞坏

  ### G3. 尺寸语义

  - [x] 已有最小 winsize 聚合逻辑
  - [ ] 需要验证多 client attach / detach 后 winsize 更新不会影响 probe

  ---

  ## 8. 失败项对照清单

  ### P0：必须先修

  - [ ] `verify_server_alive`
  - [ ] `verify_signal`
  - [ ] `verify_zombie_count`

  ### P1：随后修

  - [ ] `wait_probe_ready`
  - [ ] `verify_input_token`

  ---

  ## 9. 下一步顺序

  1. 修 `detach` 语义，确保 server 在无 client 时不退出
  2. 修 `SIGCHLD / waitpid` 回收，压掉 zombie
  3. 修 session 级信号隔离，保证 `Ctrl+C` / `Ctrl+Z` 精确投递
  4. 修 8 session 压力下的 probe ready 稳定性
  5. 修多 client 在 attach / detach 之后的输入语义