# mini-tmux 单个 Pane 设计说明

这份文档只讨论 `mini-tmux` 中“单个 pane”应该如何设计，不涉及多 pane 布局、多 client、detach/reattach 等更高层功能。

目标是回答 3 个问题：

- 单个 pane 到底是什么
- 单个 pane 最小要实现哪些功能
- 单个 pane 在程序结构上应该怎么组织

---

## 1. 先给定义：单个 Pane 是什么

在 `mini-tmux` 里，一个 pane 不是一个“屏幕区域”，而是一个完整的终端运行单元。

它至少包含：

- 一个正在运行的子进程
- 一对 PTY：`master/slave`
- 一组和该子进程绑定的终端语义
- 一份由 server 持有的状态记录

也就是说，单个 pane 的本质是：

`Server <-> PTY master <-> PTY slave <-> shell / TUI program`

其中：

- `Server` 通过 `master` 和 pane 通信
- pane 内的程序通过 `slave` 认为自己连着一个真实终端

所以单个 pane 的设计重点，不是“怎么画出一个框”，而是“怎么正确地启动并维护一个终端环境中的程序”。

---

## 2. 单个 Pane 必须实现的核心功能

如果只做最小可运行版本，单个 pane 至少要支持下面这些能力。

### 2.1 创建 pane

要做到：

- 创建一对 PTY
- fork 出一个子进程
- 让子进程进入新的 session
- 把 PTY slave 设为控制终端
- 将子进程的 `stdin/stdout/stderr` 绑定到 PTY slave
- exec 一个 shell

这一步完成后，pane 内程序才真正“活起来”。

### 2.2 接收用户输入

要做到：

- `Server` 能向 pane 写入字节流
- 这些字节通过 PTY master 进入 PTY slave
- pane 内程序能像从真实终端一样读到这些输入

最常见的例子就是：

- 输入命令
- 回车执行
- 发送 `Ctrl+C` / `Ctrl+Z` 这类控制字符

### 2.3 输出程序内容

要做到：

- pane 内程序向 `stdout/stderr` 输出
- 输出流经过 PTY slave 到达 PTY master
- `Server` 能从 PTY master 读到数据
- 上层 client 可以把这些数据展示出来

这是单 pane 的最基本输出通道。

### 2.4 维护终端属性

要做到：

- pane 有自己的 `winsize`
- 程序运行时 `isatty(0/1/2)` 为真
- `tty`、`stty size`、`vim`、`top` 等行为正常

这决定了 pane 里的程序到底是不是“真的在终端里”。

### 2.5 跟踪子进程生命周期

要做到：

- 知道 pane 的 child pid 是谁
- child 退出后能够检测到
- 正确回收 child，避免 zombie
- 更新 pane 状态为 exited / dead

否则 pane 会变成悬空资源。

### 2.6 正确关闭和清理

要做到：

- 关闭不再需要的 fd
- child 退出后释放 PTY 资源
- server 不再继续对已经失效的 pane 做 I/O

单 pane 的 bug 很多都不是出在“创建”，而是出在“退出和清理”。

---

## 3. 单个 Pane 的最小架构

可以把单个 pane 看成 4 个部分。

### 3.1 Pane State

这是 server 里保存的一份记录，至少应包含：

- `pane_id`
- `pty_master_fd`
- `child_pid`
- `alive/exited` 状态
- 当前 `rows/cols`
- 可选的退出码、退出信号

这是 pane 的“元数据层”。

### 3.2 Pane Runtime

这是实际运行中的那部分对象：

- PTY master
- PTY slave
- child process
- child process 所在的 session / process group / controlling terminal

这是 pane 的“真实运行实体”。

### 3.3 Pane I/O Interface

这是 pane 对 server 暴露的两个最核心接口：

- `write_input(data)`
- `read_output()`

本质上：

- `write_input` 是向 `pty_master_fd` 写
- `read_output` 是从 `pty_master_fd` 读

单个 pane 对外的接口其实非常小。

### 3.4 Pane Lifecycle

这是 pane 的状态迁移过程：

- created
- running
- exited
- reaped
- destroyed

如果没有生命周期概念，后面做 pane 销毁、最后一个 pane 退出、资源释放时会很乱。

---

## 4. 单个 Pane 的关键系统调用链

单 pane 的核心在这条链：

```text
openpty()
-> fork()
-> child: setsid()
-> child: ioctl(slave_fd, TIOCSCTTY, 0)
-> child: dup2(slave_fd, STDIN_FILENO)
-> child: dup2(slave_fd, STDOUT_FILENO)
-> child: dup2(slave_fd, STDERR_FILENO)
-> child: close(extra_fds)
-> child: exec(shell)
```

父进程这边要做：

```text
parent: close(slave_fd)
parent: keep(master_fd)
parent: remember(child_pid, master_fd)
```

这条链中每一步的作用分别是：

- `openpty()`
  - 创建伪终端对
- `fork()`
  - 分出 pane 子进程
- `setsid()`
  - 让子进程脱离原 session，成为新 session leader
- `TIOCSCTTY`
  - 把 PTY slave 变成该 session 的控制终端
- `dup2()`
  - 把标准输入输出错误都接到 PTY slave
- `exec()`
  - 启动 shell 或目标程序

如果这些顺序错了，pane 看起来可能“像是启动了”，但终端语义会是错的。

---

## 5. 单个 Pane 的数据流

### 5.1 输入流

```text
Server 收到用户输入
-> 写入 pane 的 PTY master
-> PTY slave 把输入交给 shell/TUI 程序
-> 程序读取 stdin
```

### 5.2 输出流

```text
shell/TUI 程序写 stdout/stderr
-> 数据进入 PTY slave
-> Server 从 PTY master 读出
-> 上层显示给用户
```

所以单 pane 其实就是一个“终端 I/O 适配器”。

---

## 6. 单个 Pane 的状态机

建议至少有下面几个状态：

### 6.1 Created

含义：

- pane 元数据已分配
- 但可能还没成功完成 child 启动

### 6.2 Running

含义：

- child 已 exec 成功或至少已进入运行阶段
- PTY master 可读可写
- pane 可接收输入并产生输出

### 6.3 Exited

含义：

- child 已经退出
- 但 server 可能还没彻底清理完资源

### 6.4 Reaped

含义：

- `waitpid()` 已经回收 child
- 已有退出状态
- 后续只剩资源释放

### 6.5 Destroyed

含义：

- master fd 已关闭
- server 中的 pane 记录将被移除或已移除

这个状态机有助于你把“子进程结束”和“资源彻底释放”区分开。

---

## 7. 单个 Pane 应暴露哪些接口

如果你后面要写 C++，一个 pane 设计上至少应有这些接口语义：

### 7.1 创建类接口

- `spawn_shell(...)`
- `spawn_program(...)`

职责：

- 建立 PTY
- fork child
- 完成终端绑定
- 返回 pane 对象或 pane 句柄

### 7.2 I/O 类接口

- `write_to_pane(bytes)`
- `read_from_pane(buffer)`

职责：

- 向 pane 输入
- 从 pane 收输出

### 7.3 状态查询接口

- `pid()`
- `master_fd()`
- `is_alive()`
- `exit_status()`
- `winsize()`

### 7.4 控制类接口

- `resize(rows, cols)`
- `close_input()` 或 `terminate()`
- `destroy()`

### 7.5 回收类接口

- `handle_child_exit(...)`
- `reap_if_needed()`

这里不一定非要按面向对象写，但功能边界最好清楚。

---

## 8. 单个 Pane 的内部字段建议

如果用结构体描述，建议最少包括：

```text
struct Pane {
    int pane_id;
    int master_fd;
    pid_t child_pid;
    bool alive;
    bool exited;
    int exit_code;
    int exit_signal;
    int rows;
    int cols;
};
```

如果后面还要扩展，可继续加：

- 输出缓冲
- 日志文件句柄
- pipeout 相关 fd
- pane 标题
- 最近一次活动时间

但在单 pane 阶段，不要过早扩展太多。

---

## 9. 单个 Pane 和信号的关系

虽然现在只讨论单 pane，但信号语义已经必须正确。

### 9.1 为什么单 pane 也必须关心 session

因为即使只有一个 pane：

- `Ctrl+C` 也应该打到 pane 内前台进程组
- `Ctrl+Z` 也应该只影响 pane 内程序
- resize 后程序应该收到 `SIGWINCH`

这说明单 pane 阶段就不能把“终端语义”简化掉。

### 9.2 单 pane 需要特别关注哪些信号

- `SIGCHLD`
  - 用来知道 child 退出了
- `SIGWINCH`
  - pane 尺寸变化时需要传播
- `SIGPIPE`
  - 更偏 client/socket 层，但以后一定会遇到

在纯单 pane 最小版里，最关键的是先把 `SIGCHLD + waitpid()` 这一条搞对。

---

## 10. 单个 Pane 的正确性检查标准

如果单 pane 做对了，至少应满足这些现象：

- 在 pane 中运行 `tty`，能看到 `/dev/pts/X`
- `python3 -c "import os; print(os.isatty(0), os.isatty(1), os.isatty(2))"` 输出全是 `True`
- `stty size` 输出与你分配给 pane 的行列数一致
- 可以正常运行交互式 shell
- 可以输入命令并获得输出
- child 退出后不会留下 zombie
- 关闭 pane 后不会因为 fd 泄漏导致 EOF 异常

这些检查比“看起来像能跑”更可靠。

---

## 11. 单个 Pane 最常见的错误

### 11.1 忘记在父进程关闭 slave fd

后果：

- pane 退出时 PTY 行为异常
- master 侧可能读不到预期 EOF

### 11.2 没有 `setsid()`

后果：

- child 拿不到独立控制终端
- 信号语义和终端语义会错

### 11.3 `dup2()` 后没关闭原始 fd

后果：

- fd 泄漏
- 生命周期难以判断

### 11.4 没有处理 child 退出

后果：

- zombie 进程堆积
- pane 状态和真实运行状态不一致

### 11.5 只验证普通输出，不验证 TTY 语义

后果：

- `echo hello` 看起来正常
- 但 `vim`、`top`、`stty size`、`isatty()` 全部有问题

单 pane 最大的陷阱，就是“看起来会回显”不等于“终端接线正确”。

---

## 12. 推荐的实现边界

如果你现在要开始写代码，单 pane 阶段建议只做这些事：

### 必做

- spawn 一个 shell
- 转发输入
- 读取输出
- 支持 resize
- 处理 child 退出
- 正确清理 fd

### 暂时不要做

- 多 pane 布局
- 焦点切换
- 多 client
- detach/reattach
- capture/log/pipeout

理由很简单：

单 pane 是整个 mini-tmux 的地基。地基不稳，后面所有高级功能都会把错误放大。

---

## 13. 结论

单个 pane 的设计可以压缩成一句话：

**一个 pane，就是 server 管理下的一个“带 PTY 终端语义的子进程运行单元”。**

它最核心的 5 件事是：

- 正确创建 PTY
- 正确启动 child
- 正确做输入输出转发
- 正确维护 winsize 和终端语义
- 正确处理退出与资源回收

如果单 pane 这一层设计清楚了，后面的多 pane、layout、focus、signal isolation，本质上都只是“在多个 pane 之间做调度”。
