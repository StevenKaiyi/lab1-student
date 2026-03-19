# mini-tmux 实验知识地图

这份文档的目标不是罗列零散概念，而是把 `mini-tmux` 涉及的知识整理成一个有依赖关系的学习地图，方便在正式实现前系统补课。

---

## 一、先给结论：这个实验本质上在做什么

`mini-tmux` 本质上是一个“终端复用器”的最小实现：

- 后台有一个常驻 `Server`
- 前台有一个或多个 `Client`
- 每个 `Pane` 本质上是一个运行在独立 PTY / Session 环境中的 shell 或程序
- `Server` 同时负责：
  - 创建和管理这些 pane
  - 在 `Client` 与 `Pane` 之间转发 I/O
  - 维护布局、焦点、信号投递、断开重连、多客户端同步
  - 回收子进程和清理资源

所以这个实验不是单一知识点题，而是一个“操作系统核心概念组装题”。

---

## 二、知识分块规则

为了系统学习，这里按“依赖关系 + 组装顺序”分块，而不是按教材章节分块。

分块规则如下：

1. 先学“对象是什么”
2. 再学“单个 pane 怎么活起来”
3. 再学“多个对象怎么被 Server 组织起来”
4. 最后学“高级功能、稳定性和评测”

按这个规则，整个实验可以拆成 6 个板块：

1. 终端与 Unix 进程模型
2. Pane 运行时：PTY、会话、前台进程组
3. Client/Server 通信与事件循环
4. 交互控制：raw mode、命令模式、布局与信号
5. 高级能力：detach/reattach、多 client、log/pipeout/capture
6. 工程质量：资源管理、鲁棒性、测试与调试

---

## 三、板块总览

| 板块 | 核心问题 | 直接产出 | 依赖 |
|---|---|---|---|
| A. 终端与 Unix 进程模型 | 终端、进程、会话、信号到底怎么关联 | 能正确理解“谁在控制谁” | 无 |
| B. Pane 运行时 | 一个 pane 为什么必须是 PTY + 独立 session | 能正确启动单 pane shell | A |
| C. Client/Server 通信与事件循环 | Server 如何同时管 socket、PTY、多个 client | 能实现单 server 多 fd 转发 | A, B |
| D. 交互控制层 | 用户按键、布局、焦点、resize、命令模式如何落地 | 能实现可交互的 mini-tmux | B, C |
| E. 高级能力层 | 为什么能 detach、多端 attach、输出导出 | 能实现课程要求中的扩展功能 | C, D |
| F. 工程质量层 | 怎样避免僵尸、fd 泄漏、残留 socket、错误退出 | 能通过评测并长期稳定运行 | 全部 |

---

## 四、板块 A：终端与 Unix 进程模型

这是所有后续知识的地基。没有这部分，后面很多行为只能“照着写”，但不知道为什么。

### A1. 终端、TTY、控制终端

要理解的点：

- 什么是终端设备
- 什么是 TTY / PTY
- 什么是 controlling terminal
- 为什么一个进程会“觉得自己连接在终端上”
- `isatty()` 为什么重要

和实验的关系：

- pane 里的 shell、`vim`、`top`、`cat` 等程序必须认为自己跑在“终端”里
- 如果只是普通 `pipe`，很多交互程序会退化，甚至行为完全变掉

你需要达到的理解：

- 明白“终端不是屏幕”，而是一套设备语义
- 明白 PTY 是“伪造终端语义”的关键设施

### A2. 进程、进程组、会话

要理解的点：

- `PID`、`PGID`、`SID` 分别是什么
- process group 与 session 的层级关系
- 前台进程组是什么
- 为什么 shell 启动的管道命令通常属于同一前台进程组

和实验的关系：

- 每个 pane 需要隔离信号
- `Ctrl+C` 只能打到当前 pane 的前台进程组
- 如果 session / process group 没分清，就会出现“一个 pane 的信号打死另一个 pane”的错误

### A3. 信号的真实来源与投递语义

要理解的点：

- `SIGINT`、`SIGTSTP`、`SIGCONT`、`SIGWINCH`、`SIGCHLD`、`SIGPIPE` 的语义
- `Ctrl+C` 到底是谁变成了 `SIGINT`
- 终端驱动、内核、进程组三者是什么关系
- 默认动作和自定义处理的区别

和实验的关系：

- `Ctrl+C` / `Ctrl+Z` 的行为不是简单“转发一个字节”就完了
- resize 需要触发 `SIGWINCH`
- pane 子进程退出后需要依赖 `SIGCHLD` / `waitpid()`
- client 断开后 socket 写失败可能触发 `SIGPIPE`

### A4. 文件描述符是所有 I/O 对象的统一接口

要理解的点：

- fd 是什么
- 为什么文件、socket、pipe、PTY 都能被统一处理
- `read` / `write` / `close` / `dup2` 为什么是整个系统的通用语言

和实验的关系：

- 整个 mini-tmux 实际上是在管理很多 fd 的生命周期
- 真正难的地方往往不是“功能”，而是“这个 fd 该由谁持有、什么时候关闭”

### A 板块的学习结果

学完 A，你应该能回答：

- 为什么 pane 不能直接用普通 pipe 代替 PTY
- 为什么要有 session / process group
- 为什么 `Ctrl+C` 会只影响一个 pane
- 为什么这个实验本质上是在管理一堆 fd 和进程关系

---

## 五、板块 B：Pane 运行时

这部分解决“一个 pane 怎么被真正启动起来”。

### B1. PTY 的创建与接线

要理解的点：

- `openpty()` 在做什么
- PTY master / slave 各自是谁在用
- 谁从 master 读，谁往 master 写
- slave 为什么要交给子进程

和实验的关系：

- `Server` 通过 PTY master 与 pane 内程序通信
- pane 子进程通过 PTY slave 感受到“自己就在一个终端里”

### B2. `fork -> setsid -> TIOCSCTTY -> dup2 -> exec` 这条链

这是整个实验最关键的一条系统调用链。

要理解的点：

- 为什么先 `fork()`
- 为什么子进程里要先 `setsid()`
- 为什么之后要 `ioctl(TIOCSCTTY)`
- 为什么再 `dup2(slave_fd, 0/1/2)`
- 为什么最后才 `exec()`

它们的职责：

- `fork()`：分出 pane 子进程
- `setsid()`：让子进程脱离原会话，成为新会话首进程
- `TIOCSCTTY`：把 PTY slave 设成这个新会话的控制终端
- `dup2()`：把该终端接到标准输入输出错误
- `exec()`：真正启动 shell 或目标程序

顺序不能乱的原因：

- 不先 `setsid()`，通常无法正确拿到新的 controlling terminal
- 不先挂上 controlling terminal，终端信号、作业控制相关语义会不完整
- 不正确关闭多余 fd，会导致 EOF 不出现、资源不释放、行为诡异

### B3. 前台进程组与信号隔离

要理解的点：

- 一个 pane 内前台进程组是谁
- `tcsetpgrp()` 的角色是什么
- shell 再启动子进程、管道、全屏程序时，前台进程组会怎么变化

和实验的关系：

- 实验要求 `SIGINT`、`SIGTSTP` 只打到焦点 pane
- 这不是“按 pane id 发信号”这么简单，而是终端语义、前台进程组、session 一起配合

### B4. winsize 与终端程序兼容性

要理解的点：

- `stty size` 为什么能读出行列数
- `ioctl(TIOCSWINSZ)` 在改什么
- 为什么 `vim`、`top` 这类 TUI 程序对 winsize 很敏感

和实验的关系：

- pane 布局变化或 client resize 后，PTY 的 winsize 必须同步更新
- 否则全屏程序、换行、光标、重绘都会出问题

### B 板块的学习结果

学完 B，你应该能独立解释：

- 一个 pane shell 如何被正确启动
- 为什么单 pane 基础 I/O 是实现里第一个真正的里程碑
- 为什么 `stty size`、`isatty()`、`vim` 能直接暴露 PTY 接线是否正确

---

## 六、板块 C：Client/Server 通信与事件循环

这部分解决“单 pane 跑起来以后，整个系统怎么组织”。

### C1. Unix domain socket 作为本地 IPC

要理解的点：

- `socket / bind / listen / accept / connect` 的基本流程
- Unix domain socket 和 TCP socket 共享哪些接口
- socket 文件路径的生命周期和清理问题

和实验的关系：

- client 和 server 的通信通道就是 Unix domain socket
- server 崩溃后 socket 文件可能残留，下一次 `bind()` 会失败

### C2. 通信协议设计

要理解的点：

- 字节流协议和消息协议的区别
- 输入事件、命令事件、布局刷新、attach/detach 事件如何编码
- “原始字节直通”与“结构化渲染协议”各自的优缺点

和实验的关系：

- handout 明确要求你自己设计 client/server 协议
- 这个决策会影响后续代码复杂度、调试难度、capture 实现方式和多 client 同步方式

### C3. I/O 多路复用与事件循环

要理解的点：

- 为什么 server 必须同时监听很多 fd
- `poll()` 的工作方式
- `epoll` 与 `poll()` 的区别
- event loop 的基本结构：收集就绪事件 -> 分发处理 -> 更新状态

server 需要监听的对象：

- 监听 socket：接新 client
- 已连接 client socket：收输入、命令、detach
- 每个 pane 的 PTY master：收输出
- 可能还有 pipeout/log 对应的资源

和实验的关系：

- 这是整个 server 的核心框架
- 没有事件循环，就不可能把多个 pane、多个 client、多个 PTY 同时管起来

### C4. 背压、广播与一致性

要理解的点：

- 一个 pane 输出要广播给多个 client 时怎么做
- 某个 client 慢时会不会拖慢整个系统
- 多个输入源同时到达时如何定义处理顺序

这部分不是 handout 中最底层的系统调用知识，但它会直接决定程序行为是否稳定。

### C 板块的学习结果

学完 C，你应该能画出：

- server 主循环结构
- client attach 的完整时序
- 单个 pane 的输出从 PTY 到多个 client 的流向

---

## 七、板块 D：交互控制层

这部分解决“系统不只是能跑，还要能像 tmux 一样交互”。

### D1. Client raw mode 与 `termios`

要理解的点：

- canonical mode 和 raw mode 的区别
- `cfmakeraw()` 改了哪些行为
- 为什么 `Ctrl+B`、方向键、退格、回车在 raw mode 下和普通终端模式下不一样

和实验的关系：

- client 必须把自己的终端切到 raw mode
- 否则：
  - `Ctrl+B` 不会被 client 当作前缀键
  - `Ctrl+C` 可能直接杀掉 client 自己
  - 方向键等特殊输入无法按预期处理

### D2. 按键解析与命令模式

要理解的点：

- 如何区分普通输入和前缀命令
- 如何实现 `Ctrl+B` 前缀
- 如何实现 `:` 命令模式
- 如何解析 `split / kill / focus / log / pipeout / capture`

和实验的关系：

- 命令模式是 pane 管理和高级功能入口
- 它是“tmux 操作语义”进入系统状态机的那一层

### D3. 布局系统

要理解的点：

- 为什么布局不仅是“显示问题”，还是“winsize 分配问题”
- 分隔线如何占用行数
- pane 可见区域如何计算
- 多 pane 时怎样保证每个 pane 都能同时可见

和实验的关系：

- handout 要求多 pane 布局和 winsize 联动
- 这意味着布局计算必须和 PTY winsize 更新同步

### D4. 焦点、输入路由与信号路由

要理解的点：

- 当前焦点 pane 是谁
- 普通键盘输入应该送到哪个 PTY
- `Ctrl+C` / `Ctrl+Z` 的“逻辑焦点”与“终端前台进程组”如何对应

和实验的关系：

- “焦点切换”不是 UI 小功能，而是输入和信号投递的中心
- 没有清晰的焦点模型，就实现不好 signal isolation

### D5. resize 传播链

要理解的点：

- client 终端变大或变小时，事件是如何从 client 到 server，再到各 pane
- 为什么最后要更新 winsize 并发送 `SIGWINCH`

传播链可以写成：

`真实终端 resize -> client 感知 -> 发给 server -> server 重算布局 -> 更新各 pane winsize -> 向 pane 发送 SIGWINCH`

### D 板块的学习结果

学完 D，你应该能解释：

- 为什么 raw mode 是 client 必修课
- 为什么布局、焦点、resize、signal 其实是一组耦合功能
- 为什么多 pane 并不是简单地把文本拼起来显示

---

## 八、板块 E：高级能力层

这部分对应 handout 的“tmux 像 tmux”的地方。

### E1. Detach / Reattach

要理解的点：

- 为什么 server 必须独立于 client 存活
- client 退出与 pane 子进程退出为什么不是一回事
- reattach 时为什么还能看到旧状态

和实验的关系：

- 这是终端复用器最核心的价值之一
- 如果 server 跟 client 强绑定，就不存在 detach/reattach

### E2. 多 Client 语义

要理解的点：

- 多个 client 同时 attach 时，输出为什么要广播
- 只读 client 为什么只能看不能写
- 多 client 下 winsize 为什么通常取最小值

和实验的关系：

- 这要求 server 把“pane 运行状态”和“client 展示终端”解耦
- 也要求协议和事件循环支持“一对多”

### E3. 输出导出：log / pipeout / capture

这三个功能看起来是附加项，实际上分别覆盖了 3 种非常不同的数据路径能力。

#### `:log`

本质：

- 把 pane 输出复制一份，追加写入文件

对应知识：

- 文件 I/O
- 追加写
- 生命周期管理

#### `:pipeout`

本质：

- 把 pane 输出复制一份，实时喂给外部命令的标准输入

对应知识：

- `pipe()`
- `fork()/exec()`
- 子进程回收
- 命令提前退出时的自动清理

#### `:capture`

本质：

- 导出 pane 当前“屏幕内容”

对应知识：

- 你是否保留了足够的 pane 状态
- “当前屏幕”与“原始输出字节流”不是同一个概念

这也是协议设计会反过来影响功能实现复杂度的典型例子。

### E4. 最后一个 Pane 退出时的系统收束

要理解的点：

- pane 都退出后，server 是否还应该继续活着
- attached client 此时怎么办
- socket 文件、fd、子进程如何最终清理

和实验的关系：

- handout 明确要求最后一个 pane 退出时 server 干净退出

### E 板块的学习结果

学完 E，你应该能理解：

- `mini-tmux` 为什么不是“一个全屏 shell”，而是一个长期运行的会话管理器
- 高级功能本质上都是“在已有数据流和生命周期模型上加分支”

---

## 九、板块 F：工程质量层

这部分决定你是“写出能跑的代码”，还是“写出能过测试、能长时间稳定运行的系统代码”。

### F1. fd 生命周期管理

要理解的点：

- 每次 `fork()` 后父子进程应该各自关闭什么
- pane 销毁时哪些 fd 必须关
- client detach 时 socket 如何回收
- log / pipeout 停止时文件、管道如何关闭

典型错误：

- 父进程忘关 PTY slave
- 子进程忘关 PTY master
- 旧 client socket 没关
- pipe 写端残留导致读端永远等不到 EOF

### F2. 僵尸进程回收

要理解的点：

- 什么是 zombie process
- `SIGCHLD` 什么时候来
- 为什么通常要循环 `waitpid(-1, &status, WNOHANG)`

和实验的关系：

- `fork_exit` 测试和 zombie 检查会直接卡这里

### F3. 异常路径与鲁棒性

要理解的点：

- client 崩溃后 server 是否还能活
- server 崩溃后 socket 文件残留如何处理
- raw mode 没恢复时终端会怎样
- `SIGPIPE` 默认动作为什么危险

### F4. 性能与压力场景

要理解的点：

- 高频输出下是否会卡顿
- 多 pane 并发输出时是否串流
- 快速创建销毁 pane 是否泄漏资源

这对应公开 workload 中的：

- 高频输出
- 并发输出
- 多 pane 压力
- 快速 pane 操作

### F5. 调试与验证方法

你至少应该掌握这些验证思路：

- 用 `isatty()` 验证 PTY 是否接对
- 用 `stty size` 验证 winsize 是否正确
- 用 `ps -o pid,pgid,sid,tty,comm` 验证 session / process group
- 用 `/proc/<pid>/fd` 或等价方法检查 fd 是否泄漏
- 用僵尸检查验证 `waitpid()`
- 用脚本驱动“启动 -> 发按键 -> 检查输出”形成闭环

### F 板块的学习结果

学完 F，你应该有能力：

- 把“偶发 bug”缩小到具体的 fd、进程、信号、事件顺序
- 针对每个功能写出最小可重复验证
- 对评测失败给出有方向的定位，而不是盲改

---

## 十、板块之间的关系

可以把这些板块理解成一个依赖图：

```text
A. 终端与 Unix 进程模型
        |
        v
B. Pane 运行时：PTY / session / controlling terminal
        |
        v
C. Client/Server 通信与事件循环
        |
        v
D. 交互控制层：raw mode / 命令模式 / 布局 / 焦点 / resize
        |
        v
E. 高级能力层：detach / multi-client / log / pipeout / capture
        |
        v
F. 工程质量层：资源管理 / 清理 / 鲁棒性 / 测试
```

但真实关系不是单链，而是下面这种“主链 + 横向耦合”：

```text
A -> B -> C -> D -> E
\         \    \    \
 \         \    \    -> F
  \         \    -> F
   \         -> F
    -> F
```

含义是：

- A 是所有板块的基础
- B 决定单 pane 是否正确
- C 决定整个系统是否能组织起来
- D 决定交互语义是否像 tmux
- E 是在 D 的基础上扩展能力
- F 贯穿全部实现阶段

---

## 十一、把知识映射回实验功能

下面把知识板块和 handout 功能直接对应起来。

| 实验功能 | 关键知识板块 | 说明 |
|---|---|---|
| 单 Pane 基础 I/O | A, B, C | PTY 接线正确，client/server 能转发字节流 |
| 命令模式 | D | 前缀键、命令解析、控制平面 |
| 多 Pane 布局 | D | 布局计算、可见性、winsize 联动 |
| 焦点切换 | D | 输入和信号都依赖焦点模型 |
| `Ctrl+C` / `Ctrl+Z` 精确投递 | A, B, D | session / 前台进程组 / 焦点一起生效 |
| resize / `SIGWINCH` | B, D | winsize 更新和信号传播 |
| Detach / Reattach | C, E | server/client 解耦、状态延续 |
| 多 Client | C, E | 广播、只读、最小 winsize |
| log | E, F | 输出复制、文件生命周期 |
| pipeout | A, E, F | pipe、子进程、SIGCHLD、清理 |
| capture | C, D, E | 输出模型和屏幕状态表示 |
| 僵尸回收 | A, F | `SIGCHLD` + `waitpid()` |
| fd 清理 | A, B, C, E, F | 每条数据路径都涉及句柄管理 |

---

## 十二、建议学习顺序

如果要在正式动手前较系统地补课，建议按下面顺序学，而不是平均用力。

### 第一阶段：建立系统直觉

目标：

- 搞懂 tmux 到底由哪些对象组成
- 搞懂 pane 为什么必须是 PTY + 独立 session

建议学习内容：

1. 终端、TTY、PTY
2. 进程组、会话、控制终端
3. `SIGINT` / `SIGTSTP` / `SIGWINCH` / `SIGCHLD`

### 第二阶段：把单 pane 跑通

目标：

- 能独立解释单 pane shell 为什么能工作

建议学习内容：

1. `openpty()`
2. `fork / setsid / TIOCSCTTY / dup2 / exec`
3. `ioctl(TIOCSWINSZ)`
4. `isatty()`、`stty size` 的验证方法

### 第三阶段：把系统串起来

目标：

- 能设计 server 主循环和 client/server 协议

建议学习内容：

1. Unix domain socket
2. `poll()` / `epoll`
3. 事件循环
4. 广播与多输入源处理

### 第四阶段：补交互和高级特性

目标：

- 从“能跑”升级到“像 tmux”

建议学习内容：

1. `termios` / raw mode
2. 命令模式和按键状态机
3. 多 pane 布局
4. detach/reattach
5. 多 client
6. log / pipeout / capture

### 第五阶段：补工程质量

目标：

- 降低调试成本并通过评测

建议学习内容：

1. `waitpid()` 与僵尸回收
2. fd 泄漏排查
3. `SIGPIPE` 处理
4. 异常路径清理
5. 自动化回归脚本

---

## 十三、如果时间有限，最不能跳过的知识

如果你没有足够时间全面展开，最优先的是这 8 个点：

1. PTY 的 master/slave 语义
2. `fork -> setsid -> TIOCSCTTY -> dup2 -> exec`
3. process group / session / controlling terminal
4. `SIGINT`、`SIGTSTP`、`SIGWINCH`、`SIGCHLD`、`SIGPIPE`
5. Unix domain socket
6. `poll()` 事件循环
7. `termios` raw mode
8. `waitpid()` 与 fd 清理

这 8 个点几乎决定了这个实验 80% 的成败。

---

## 十四、最终总结

这个实验涉及的知识不是平铺展开的，而是一个清晰的依赖网络：

- 最底层是终端、进程、信号、fd 这些 Unix 基础对象
- 中间层是单 pane 运行时，也就是 PTY + session + controlling terminal
- 再往上是 server/client 架构和事件循环
- 再上层才是布局、焦点、命令模式、detach、多 client 等 tmux 语义
- 全程都被资源管理、异常路径、测试验证这条工程主线贯穿

换句话说，这个实验最适合的学习方式不是“按功能点背 API”，而是：

`先理解系统模型 -> 再理解单 pane -> 再理解 server 组装 -> 再补高级功能 -> 全程做验证`

如果后续你愿意，我可以继续在这份知识地图的基础上，再帮你生成一份“学习计划版”文档，把每个板块细化成：

- 必懂概念
- 推荐阅读
- 最小实验
- 判断自己是否学会的检查题
