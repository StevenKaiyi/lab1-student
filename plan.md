# mini-tmux Implementation Plan

这份文档把整个 `mini-tmux` 的实现拆成可逐步推进的小模块。后续每完成一项，直接在对应条目前打勾即可。

## 状态约定

- `[x]` 已完成
- `[ ]` 未完成
- `[~]` 已开始但未完成

---

## 0. 当前基线

### 已有产物

- [x] 建立最小 `Server / Client / Pane` 骨架
- [x] 使用 `PTY` 启动单个 pane 内的 shell
- [x] 建立最小 `Unix domain socket` 通信链路
- [x] 建立最小 `poll()` 事件循环
- [x] Client 进入 raw mode，并在退出时恢复
- [x] 单 pane 的输入输出双向转发
- [x] 单 pane 的 `isatty()` / `tty` / `stty size` 基础自测
- [x] 编写单 pane 自测脚本 `test_single_pane.py`

### 当前实现边界

- [x] 仅支持单个 pane
- [x] 尚未实现命令模式
- [x] 尚未实现多 pane 布局
- [x] 尚未实现 detach / reattach
- [x] 尚未实现多 client
- [x] 尚未实现 log / pipeout / capture

---

## 1. 项目主线图

建议按下面顺序推进：

1. 单 pane 基础闭环
2. 命令模式与 pane 管理
3. 多 pane 数据结构
4. 布局与渲染
5. 焦点与输入路由
6. 信号隔离
7. detach / reattach
8. 多 client / 只读 client
9. log / pipeout / capture
10. 清理、鲁棒性、回归测试

---

## 2. 模块 A：进程模型与系统骨架

目标：把整个程序的角色边界固定住，避免后面越写越乱。

### A1. 运行模式划分

- [x] 明确 `./mini-tmux` 是“首次启动或自动 attach”入口
- [x] 明确 `./mini-tmux attach` 是 attach 入口
- [x] 在同一个可执行文件内区分 server/client 行为
- [ ] 明确只读 attach 的命令行入口 `./mini-tmux attach -r`

### A2. 关键对象建模

- [x] 定义 pane 的基本状态：`master_fd / child_pid / winsize / exit status`
- [x] 定义 client 连接对象
- [ ] 定义全局 session / server 状态对象
- [ ] 把全局状态从“零散变量”收敛成结构化对象

### A3. 协议与事件框架

- [x] 定义最小 client/server 消息类型：输入、resize、输出、退出
- [ ] 扩展消息类型以支持命令模式、pane 管理、重绘、attach 语义
- [ ] 整理协议编码/解码逻辑，避免后面散落在各处

---

## 3. 模块 B：单 Pane 运行时

目标：把单 pane 这一层做成稳定地基。

### B1. Pane 启动链

- [x] `openpty()` 创建 PTY
- [x] `fork()` 创建 pane 子进程
- [x] 子进程 `setsid()`
- [x] 子进程 `ioctl(TIOCSCTTY)` 绑定控制终端
- [x] 子进程 `dup2()` 到 `stdin/stdout/stderr`
- [x] `exec()` 启动 shell

### B2. Pane I/O

- [x] Client 输入经 socket 送入 PTY master
- [x] PTY master 输出经 server 广播给 client
- [x] attach 时补发 backlog，避免首屏输出丢失

### B3. Pane 生命周期

- [x] 记录 child pid
- [x] 监听 `SIGCHLD`
- [x] 用 `waitpid()` 回收子进程
- [x] child 退出后关闭 PTY 并通知 client
- [ ] 明确 pane state 枚举：created/running/exited/reaped/destroyed

### B4. 单 pane 进一步验证

- [x] `tty` 检查
- [x] `isatty()` 检查
- [x] `stty size` 检查
- [ ] `helpers/probe` 接入验证
- [ ] `helpers/fork_exit` 验证僵尸回收
- [ ] `vim/top` 兼容性验证

---

## 4. 模块 C：Client 终端控制

目标：让 client 成为稳定、可扩展的前台交互层。

### C1. raw mode 管理

- [x] Client 启动时进入 raw mode
- [x] Client 退出时恢复终端属性
- [ ] 补充异常退出路径的恢复策略

### C2. 输入处理

- [x] 普通按键透传到 pane
- [ ] 区分普通输入与前缀命令输入
- [ ] 支持 `Ctrl+B` 前缀键状态机
- [ ] 支持命令模式输入缓冲

### C3. 终端尺寸处理

- [x] 捕获 `SIGWINCH`
- [x] 将窗口大小通过 socket 发送给 server
- [x] 对异常 winsize 做兜底
- [ ] 多 client 场景下统一 winsize 策略

---

## 5. 模块 D：命令模式

目标：建立 tmux 风格的控制入口。

### D1. 进入与退出命令模式

- [ ] 支持 `Ctrl+B` 后按 `:` 进入命令模式
- [ ] 命令模式下暂停向 pane 透传普通输入
- [ ] 回车执行命令
- [ ] `Esc` 取消命令模式

### D2. 命令解析器

- [ ] 解析 `:new`
- [ ] 解析 `:kill <pane_id>`
- [ ] 解析 `:focus <pane_id>`
- [ ] 为后续 `:log` / `:pipeout` / `:capture` 预留解析接口

### D3. 错误反馈

- [ ] 对非法命令返回明确错误信息
- [ ] 对缺失参数返回明确错误信息
- [ ] 统一命令执行结果的反馈格式

---

## 6. 模块 E：多 Pane 数据结构

目标：让“多个 pane”先在状态层成立，再谈显示层。

### E1. Pane 容器

- [ ] 支持创建 Pane 0 之外的新 pane
- [ ] 维护 pane 列表或映射表
- [ ] 分配稳定的 pane id
- [ ] 支持根据 pane id 查找 pane

### E2. Pane 生命周期管理

- [ ] `:new` 创建新 pane
- [ ] `:kill` 销毁指定 pane
- [ ] child 退出后自动标记 pane 不可用
- [ ] 最后一个 pane 退出时定义系统行为

### E3. 状态同步

- [ ] pane 创建/删除后通知 client 重绘
- [ ] pane 焦点变化后通知 client 更新视图

---

## 7. 模块 F：布局系统

目标：解决“多个 pane 怎么同时显示”和“每个 pane 的 winsize 怎么计算”。

### F1. 布局模型

- [ ] 定义垂直等分布局
- [ ] 定义 pane 可见区域
- [ ] 定义分隔行渲染规则
- [ ] 标记当前焦点 pane

### F2. winsize 联动

- [ ] 根据布局为每个 pane 计算 `rows/cols`
- [ ] 调用 `ioctl(TIOCSWINSZ)` 更新每个 pane 的 PTY
- [ ] 在布局变化后为相关 pane 传播 `SIGWINCH`

### F3. 布局验证

- [ ] 两个 pane 同时可见
- [ ] 多个 pane 分配行数合理
- [ ] resize 后布局与 `stty size` 一致

---

## 8. 模块 G：渲染与重绘

目标：让 client 看到的是“多 pane 视图”，而不是原始字节混流。

### G1. 渲染策略选型

- [ ] 明确是“server 做结构化渲染”还是“client 自己渲染”
- [ ] 固定当前阶段的渲染责任边界

### G2. 最小渲染实现

- [ ] 支持把多个 pane 内容同时画到当前终端
- [ ] 支持分隔行
- [ ] 支持焦点标记

### G3. 重绘触发点

- [ ] pane 输出变化时重绘
- [ ] pane 创建/销毁时重绘
- [ ] 焦点切换时重绘
- [ ] resize 时重绘

---

## 9. 模块 H：焦点与输入路由

目标：让输入、命令、信号都能落到正确 pane。

### H1. 焦点管理

- [ ] 保存当前焦点 pane id
- [ ] 新 pane 创建后的焦点策略
- [ ] pane 销毁后的焦点迁移策略

### H2. 焦点切换接口

- [ ] `:focus <pane_id>`
- [ ] `Ctrl+B n` 切到下一个 pane
- [ ] `Ctrl+B p` 切到上一个 pane

### H3. 输入路由

- [ ] 普通输入只发给焦点 pane
- [ ] 命令输入只进入命令模式缓冲
- [ ] 只读 client 禁止发送输入

---

## 10. 模块 I：信号隔离与作业控制

目标：让 `Ctrl+C` / `Ctrl+Z` / resize 的语义真的像 tmux。

### I1. 每个 pane 的会话隔离

- [x] 每个 pane 子进程独立 `setsid()`
- [ ] 明确前台进程组的管理策略
- [ ] 在多 pane 下验证不同 pane 的会话/进程组隔离

### I2. 交互信号行为

- [ ] `Ctrl+C` 只影响焦点 pane
- [ ] `Ctrl+Z` 只影响焦点 pane
- [ ] `SIGCONT` 行为正确

### I3. resize 信号

- [x] 基础 `SIGWINCH` 通路已经存在
- [ ] 多 pane resize 时对所有 pane 正确传播

---

## 11. 模块 J：Detach / Reattach

目标：让 server 和 pane 脱离 client 独立存活。

### J1. detach 语义

- [ ] `Ctrl+B d` 触发 detach
- [ ] client 退出但 server 不退出
- [ ] pane 子进程继续运行

### J2. reattach 语义

- [ ] `./mini-tmux attach` 连接已有 server
- [ ] attach 后恢复当前 pane 输出视图
- [ ] attach 后继续交互

### J3. 无 client 状态

- [ ] 所有 client 断开后 server 继续运行
- [ ] 新 client 随后可重新 attach

---

## 12. 模块 K：多 Client

目标：支持多个终端同时连到同一个 session。

### K1. 多 client 管理

- [ ] 支持多个 client 同时 attach
- [ ] 新 client attach 时同步当前视图
- [ ] 一个 client 断开不影响其他 client

### K2. 输入权限

- [ ] 默认读写 client 可输入
- [ ] `attach -r` 只读 client 仅接收输出
- [ ] server 忽略只读 client 的输入

### K3. 尺寸协调

- [ ] 多 client 的最终 winsize 策略
- [ ] attach/detach 后重新计算 pane winsize

---

## 13. 模块 L：输出扩展功能

目标：实现 handout 后半部分要求的观测与导出能力。

### L1. `:log`

- [ ] `:log <pane_id> <file_path>` 开始记录 pane 输出
- [ ] `:log-stop <pane_id>` 停止记录
- [ ] pane 结束时自动清理 log 句柄

### L2. `:pipeout`

- [ ] `:pipeout <pane_id> <cmd>` 启动输出管道
- [ ] `:pipeout-stop <pane_id>` 停止输出管道
- [ ] 外部命令退出后自动清理

### L3. `:capture`

- [ ] `:capture <pane_id> <file_path>` 导出 pane 当前屏幕内容
- [ ] 定义“当前屏幕内容”的内部表示方式

---

## 14. 模块 M：资源管理与鲁棒性

目标：避免实现后期最常见的“看起来能跑，但资源慢慢坏掉”。

### M1. fd 生命周期

- [x] 基础版本已经在关键路径关闭多余 fd
- [ ] 系统性梳理 server / client / pane 各自持有的 fd
- [ ] pane 销毁时保证全部相关 fd 被释放
- [ ] client 断开时保证 socket 被释放

### M2. 进程回收

- [x] 已有基础 `SIGCHLD + waitpid()`
- [ ] 扩展到 pane 子进程之外的 pipeout/log 子进程
- [ ] 压力场景下无 zombie 堆积

### M3. 异常路径

- [x] 已忽略 `SIGPIPE`
- [ ] client 异常退出路径补齐
- [ ] server 启动失败路径补齐
- [ ] socket 残留文件恢复策略补齐

---

## 15. 模块 N：测试与回归

目标：每做完一个模块，都有办法验证，而不是最后一起爆炸。

### N1. 当前已有测试

- [x] `test_single_pane.py`

### N2. 下一步要补的测试

- [ ] `test_single_pane_probe.py`
- [ ] `test_single_pane_resize.py`
- [ ] `test_multi_pane_layout.py`
- [ ] `test_focus_and_signal.py`
- [ ] `test_detach_reattach.py`
- [ ] `test_multi_client.py`
- [ ] `test_log_pipe_capture.py`

### N3. 回归检查清单

- [ ] 每次改协议后重跑基础单 pane 测试
- [ ] 每次改布局后重跑 winsize 相关测试
- [ ] 每次改生命周期后重跑 zombie/fd 清理测试

---

## 16. 当前推荐的下一步

如果按最稳的实现路线，建议后面按这个顺序继续：

- [ ] 第一步：实现命令模式最小闭环，只支持 `:new` / `:kill` / `:focus`
- [ ] 第二步：实现 pane 列表与多 pane 状态管理
- [ ] 第三步：实现最简单的垂直等分布局
- [ ] 第四步：实现焦点切换和输入路由
- [ ] 第五步：实现并验证多 pane 的信号隔离
- [ ] 第六步：实现 detach / reattach
- [ ] 第七步：实现多 client 和只读 attach
- [ ] 第八步：实现 `log` / `pipeout` / `capture`

---

## 17. 使用方式

后续每完成一项，直接把对应条目从：

```md
- [ ] 某项任务
```

改成：

```md
- [x] 某项任务
```

如果只是做到一半，可以改成：

```md
- [~] 某项任务
```

这份文档应该始终和代码状态保持同步，不要把它当成一次性计划，而要把它当成开发中的执行看板。
