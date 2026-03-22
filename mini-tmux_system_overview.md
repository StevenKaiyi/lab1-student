# mini-tmux 系统总览

这份文档不是逐条抄 handout，而是把这个实验压成一张“系统结构图 + 连接关系图 + 实现路线图”，方便你先建立整体认知，再进入编码。

## 1. 一句话先定性

`mini-tmux` 本质上是一个长期运行的后台会话管理器：

- 后台有一个常驻 `Server`
- 前台可以有一个或多个 `Client`
- 每个 `Pane` 本质上是一个运行在独立 `PTY + Session` 环境中的 shell 或 TUI 程序
- `Server` 负责在 `Client`、`Pane`、`Socket`、`Signal`、`Layout` 之间做统一调度

所以它不是“一个会分屏的终端界面”，而是一个：

`Client <-> Unix socket <-> Server <-> PTY master <-> PTY slave <-> shell/program`

---

## 2. 系统的整体框架

可以把系统分成 6 层：

### 2.1 进程角色层

- `Server`
  - 后台常驻
  - 管理所有 pane、client、socket、pty、布局、焦点
  - 即使所有 client 断开也继续运行
- `Client`
  - 前台交互进程
  - 负责接管用户真实终端、读取按键、显示 server 发来的内容
  - 可以反复 attach / detach
- `Pane Child Process`
  - 真实跑程序的子进程
  - 往往是 shell，也可能是 `vim`、`top`、`ping` 等
  - 每个 pane 都应处在自己的终端语义环境里

### 2.2 资源对象层

- `Unix domain socket`
  - `Server <-> Client` 的 IPC 通道
- `PTY`
  - `Server <-> Pane` 的终端语义通道
- `Signal`
  - 处理 `SIGINT`、`SIGTSTP`、`SIGWINCH`、`SIGCHLD`、`SIGPIPE`
- `termios/raw mode`
  - 让 client 能按字节接收按键，不被本地终端驱动抢先处理

### 2.3 状态管理层

Server 内部至少要维护这些状态：

- pane 列表
- 当前焦点 pane
- client 列表
- client 是否只读
- 布局信息
- 每个 pane 对应的 PTY master fd、child pid、winsize
- socket 路径和实例名
- pipeout/log/capture 相关状态

### 2.4 事件循环层

Server 核心是一个 event loop，监听：

- 监听 socket：接新 client
- client socket：收输入、命令、detach、resize
- pane PTY master：收 pane 输出
- 可能还有 pipe/log 的额外 fd

这层把整个系统从“很多独立对象”变成“一个统一调度系统”。

### 2.5 交互控制层

- 前缀键 `Ctrl+B`
- 命令模式 `:`
- pane 创建 / 销毁
- 焦点切换
- 布局更新
- resize 传播
- attach / detach
- 多 client 同步

### 2.6 工程稳定性层

- fd 生命周期管理
- 子进程回收
- socket 文件清理
- raw mode 恢复
- 断连处理
- EOF / EIO / SIGPIPE 等异常路径

---

## 3. 核心对象之间的连接关系

### 3.1 静态结构图

```text
真实终端
   ^
   | termios/raw mode
   v
Client
   ^
   | Unix domain socket
   v
Server
   ^
   | PTY master
   v
Pane child process
   ^
   | stdin/stdout/stderr 绑定到 PTY slave
   v
shell / vim / top / ping ...
```

### 3.2 多 client / 多 pane 图

```text
Client A ----\\
              \\
Client B ------> Unix socket ----> Server event loop
              /                         |        \\
Client C ----/                          |         \\
                                         |          \\
                                    PTY master1   PTY master2 ...
                                         |              |
                                      Pane 1         Pane 2
                                         |              |
                                      shell/vim       shell/top
```

### 3.3 最关键的依赖关系

- `Client` 不直接接触 pane 子进程，只和 `Server` 通信
- `Server` 不直接解释用户真实终端，而是通过 `Client` 获取输入
- `Pane` 不知道 client 是否存在，它只以为自己连着一个真实终端
- `Signal` 的正确性依赖于 `PTY + session + process group` 的接线是否正确
- `Layout` 不只是显示问题，还决定每个 pane 的 `winsize`

---

## 4. 两条主链路：数据流和控制流

### 4.1 数据流：用户输入如何进入 pane

```text
键盘输入
-> Client 从真实终端读到字节
-> Client 判断是否是前缀键/命令模式输入
-> 若是普通输入，则通过 socket 发给 Server
-> Server 根据当前焦点 pane 路由
-> Server 写入对应 pane 的 PTY master
-> PTY slave 对面的 shell/TUI 程序读到输入
```

重点：

- client 必须是 raw mode，否则 `Ctrl+B`、方向键、`Ctrl+C` 会先被本地终端处理
- server 必须知道“当前焦点 pane 是谁”
- 输入路由的目标不是 pane id 的“界面框”，而是它背后的 PTY

### 4.2 数据流：pane 输出如何显示到 client

```text
shell/TUI 输出
-> 写到 stdout/stderr
-> 进入 PTY slave
-> Server 从 PTY master 读出
-> Server 决定如何转发/缓存/广播
-> 通过 socket 发给一个或多个 Client
-> Client 显示到真实终端
```

重点：

- 这是多 client 同步的基础
- log / capture / pipeout 都是从这条输出链上分流出来的
- 如果某个 client 很慢，server 的输出策略会影响系统稳定性

### 4.3 控制流：pane 创建

pane 创建是整个系统最关键的一条系统调用链：

```text
Server
-> openpty()
-> fork()
-> 子进程 setsid()
-> 子进程 ioctl(TIOCSCTTY)
-> 子进程 dup2(slave_fd -> stdin/stdout/stderr)
-> 子进程 exec(shell)
-> 父进程保留 master，关闭不需要的 slave
```

这条链的意义：

- `openpty()` 提供“伪终端”
- `setsid()` 让 pane 脱离旧会话
- `TIOCSCTTY` 把 PTY slave 变成该 pane 会话的控制终端
- `dup2()` 让程序的标准输入输出真正接到终端上
- `exec()` 后，shell 会“认为自己在一个真实终端里”

### 4.4 控制流：resize 传播

```text
真实终端窗口尺寸变化
-> Client 感知变化
-> 发消息给 Server
-> Server 重新计算 layout
-> 为每个 pane 计算新的行列数
-> ioctl(TIOCSWINSZ) 更新对应 PTY
-> 向 pane 前台进程组触发/传播 SIGWINCH
```

这条链把“界面布局”和“终端程序行为”连接起来了。

---

## 5. 为什么必须是这些组件，不能随便替换

### 5.1 为什么 pane 必须用 PTY，不能只用 pipe

因为 shell、vim、top 等程序需要终端语义：

- `isatty()` 要为真
- 行编辑、光标控制、颜色、全屏刷新都依赖终端
- `Ctrl+C`、`Ctrl+Z`、`SIGWINCH` 等机制都和终端驱动有关

如果改成普通 `pipe`：

- 程序会认为自己不是交互式终端
- 很多 TUI 行为会退化甚至失效

### 5.2 为什么要 Client/Server 分离

因为实验要支持：

- detach 后 server 继续活着
- 后续 reattach
- 多 client 同时连接
- 只读 client

如果只有单进程前台程序，这些能力很难自然成立。

### 5.3 为什么要 event loop

因为 server 要同时盯住很多 fd：

- 新连接
- 多个 client
- 多个 pane 输出
- 额外的 pipe/log

没有 event loop，就没有统一调度中心。

### 5.4 为什么焦点不仅是 UI 概念

焦点决定：

- 普通输入送到哪个 pane
- `Ctrl+C` / `Ctrl+Z` 应影响谁
- pane 创建/销毁后谁接管输入

所以焦点本质上是“输入路由中心”。

---

## 6. 你可以怎样理解每个子系统

### 6.1 Pane 子系统

职责：

- 创建 pane
- 销毁 pane
- 保存 pid / pty fd / 状态
- 更新 winsize
- 检测退出并回收

依赖：

- PTY
- fork/exec
- session/process group
- SIGCHLD

### 6.2 Client 通信子系统

职责：

- attach 到 server
- 发送按键和控制命令
- 接收 server 输出和状态更新
- 支持只读模式

依赖：

- Unix domain socket
- 协议设计
- raw mode

### 6.3 Server 调度子系统

职责：

- 统一事件循环
- 转发 I/O
- 管理 client 和 pane 的生命周期
- 维护全局状态

依赖：

- poll/epoll
- fd 管理
- 状态机设计

### 6.4 交互语义子系统

职责：

- 前缀键处理
- 命令模式解析
- 焦点切换
- 布局变更
- detach / attach 语义

依赖：

- client 输入处理
- server 状态更新
- pane 管理

### 6.5 可观测性子系统

职责：

- `log`
- `capture-pane`
- `pipeout`

本质：

- 都是从“pane 输出流”上做额外分支

---

## 7. 这个实验里最容易混淆的几个关系

### 7.1 Client 和 Pane 不是一一对应

- 一个 client 可以看到多个 pane
- 多个 client 也可以同时看同一组 pane
- pane 由 server 管，不属于某个特定 client

### 7.2 布局和 pane 不是同一个概念

- pane 是“运行实体”
- layout 是“显示和尺寸分配规则”

布局变了，不代表 pane 重建了；但 pane 的 `winsize` 往往要跟着变。

### 7.3 `Ctrl+C` 不是 client 直接发信号给自己

在正确设计里：

- client 处于 raw mode，先读到字节
- server 根据焦点 pane 处理
- 最终应由 pane 所处的终端/前台进程组语义去保证信号只影响该 pane

这里如果 `session / controlling terminal / process group` 没搭好，就会出现串 pane 的 bug。

### 7.4 detach 不是“暂停 pane”

detach 只是：

- client 断开
- server 继续跑
- pane 子进程继续跑

这正是 tmux 的核心价值。

---

## 8. 从实现顺序看，系统应如何逐层长出来

推荐按下面顺序理解和实现：

### 阶段 1：单 pane 最小闭环

目标：

- 启动后看到 shell
- 能输入命令
- 能看到输出

你在验证：

- PTY 接线是否正确
- `fork -> setsid -> TIOCSCTTY -> dup2 -> exec` 是否正确
- server/client 最小转发链是否成立

### 阶段 2：命令模式和 pane 生命周期

目标：

- 创建 pane
- 销毁 pane
- 切换焦点

你在验证：

- server 状态管理是否成立
- client 输入解析是否成立
- pane 列表和焦点切换是否稳定

### 阶段 3：布局和 winsize

目标：

- 多 pane 同时可见
- resize 后尺寸正确

你在验证：

- layout 计算是否正确
- layout 和 PTY winsize 是否联动

### 阶段 4：信号隔离

目标：

- `Ctrl+C`、`Ctrl+Z` 只影响焦点 pane

你在验证：

- session / process group / controlling terminal 是否真正理解并接对

### 阶段 5：detach / reattach / 多 client

目标：

- client 来去自由
- server 和 pane 长期存活
- 多 client 同步显示

你在验证：

- client/server 架构是否真的独立
- 广播和一致性策略是否可靠

### 阶段 6：log / capture / pipeout 和稳定性

目标：

- 输出可记录、可抓取、可分流
- 长时间运行不泄漏资源

你在验证：

- 输出链路抽象是否足够干净
- fd 和子进程回收是否完整

---

## 9. workload 名称基本对应的系统能力

从 `workloads/public` 的文件名可以反推出评测关注点：

- `01_single_pane_basic`
  - 单 pane I/O 基础闭环
- `02_two_pane_signal_isolation`
  - 多 pane 信号隔离
- `03_four_pane_focus_switch`
  - 焦点切换
- `04_pane_create_destroy`
  - pane 生命周期
- `05_high_freq_output`
  - 高频输出下的转发稳定性
- `06_zombie_reap`
  - 子进程回收
- `07_resize_sigwinch`
  - resize 与 `SIGWINCH`
- `08_eight_pane_stress`
  - 多 pane 压力场景
- `09_tui_compat`
  - TUI 程序兼容性，反向说明 PTY/winsize 必须正确
- `10_concurrent_output`
  - 多输出源并发
- `11_sigtstp_sigcont`
  - 停止/继续信号语义
- `12_rapid_pane_ops`
  - 快速创建销毁的鲁棒性
- `13_detach_reattach_basic`
  - detach / attach 基础
- `14_detach_probe_survives`
  - detach 后 pane 继续运行
- `15_log_basic`
  - 日志输出
- `16_pipeout_basic`
  - 输出管道
- `17_pipeout_cmd_exits`
  - pipeout 外部命令退出后的清理
- `18_multi_client_basic`
  - 多 client 同步
- `19_multi_client_readonly`
  - 只读 client 语义
- `20_capture_pane`
  - pane 输出抓取
- `21_last_pane_exit`
  - 最后一个 pane 退出后的系统行为
- `22_layout_both_visible`
  - 布局是否让 pane 同时可见
- `23_layout_winsize`
  - 布局与 winsize 一致性

这说明评测不是只看“能不能跑起来”，而是在检查整个系统的连接关系有没有闭合。

---

## 10. 真正的难点不在 API，而在“连接关系是否正确”

这个实验最容易出问题的地方，不是某个单独系统调用不会写，而是这些关系一旦接错，整个系统会表现得很怪：

- 忘关 fd
  - pane 退出了，但 PTY master 读不到 EOF
- 没有 `setsid`
  - 信号打错 pane，或者控制终端语义异常
- 没处理 `SIGCHLD`
  - 出现僵尸进程
- 没处理 `SIGPIPE`
  - client 断开时 server 跟着崩
- raw mode 恢复不完整
  - 用户终端退出后不能正常输入
- resize 只改了显示，没改 winsize
  - `vim` / `top` / `stty size` 行为异常

所以你应该始终用“链路视角”来排错，而不是只盯一个函数。

---

## 11. 建议你脑中固定的总图

如果只记一张图，建议记下面这张：

```text
用户键盘
  -> Client(raw mode)
  -> socket
  -> Server(event loop, state, routing)
  -> PTY master
  -> PTY slave
  -> pane shell / TUI
  -> PTY slave
  -> PTY master
  -> Server
  -> socket
  -> 一个或多个 Client
  -> 用户屏幕
```

同时补上一条控制链：

```text
focus/layout/resize/detach/attach/signal
都不是孤立功能，
而是 Server 对 pane/client/socket/pty/process-group 的统一调度结果。
```

---

## 12. 结论

你可以把 `mini-tmux` 理解成一个由四条主线交织起来的系统：

- `PTY` 解决“pane 里的程序为什么觉得自己在终端里”
- `Session / Process Group / Signal` 解决“交互信号为什么只影响正确的 pane”
- `Unix socket + Event Loop` 解决“server 如何统一管理 client 和 pane”
- `Layout + Focus + Command Mode` 解决“它为什么表现得像 tmux 而不是一个普通 shell”

如果你后面开始实现，最好的思路不是直接写完整版本，而是始终问自己：

- 这个功能属于哪一层？
- 它依赖哪些对象？
- 它改变了哪条数据流或控制流？
- 它会不会影响 fd、winsize、signal、focus、lifecycle 中的某一条连接关系？

只要这张系统图稳了，后面的编码会清楚很多。
