# mini-tmux Implementation Plan

这份文档记录当前 `mini-tmux` 代码的真实进度，并把后续工作拆成可持续推进的模块。它不是最初设想版路线图，而是和当前实现对齐的执行看板。

## 状态约定

- `[x]` 已完成
- `[ ]` 未完成
- `[~]` 已开始但未完成

---

## 0. 当前基线

### 已有产物

- [x] 建立最小 `Server / Client / Pane / Session` 骨架
- [x] 使用 `PTY` 启动单个 pane 内的 shell
- [x] 建立最小 `Unix domain socket (SOCK_SEQPACKET)` 通信链路
- [x] 建立最小 `poll()` 事件循环
- [x] Client 进入 raw mode，并在正常退出时恢复
- [x] 单 pane 的输入输出双向转发
- [x] attach 时补发 backlog，减少首屏输出丢失
- [x] 支持 `./mini-tmux` 自动 attach 或首次启动
- [x] 支持 `./mini-tmux attach`
- [x] 支持 `./mini-tmux attach -r` 只读 attach
- [x] Client 侧实现 `Ctrl+B` 前缀状态机
- [x] 支持 `Ctrl+B :` 进入命令模式并发送命令到 server
- [x] Server 侧实现最小命令解析与状态反馈 `:new / :kill / :focus`
- [x] 单 pane 自测脚本 `test_single_pane.py`
- [x] 多 pane 命令生命周期回归脚本 `test_multi_pane_commands.py`

### 已验证行为

- [x] `tty` 检查
- [x] `isatty()` 检查
- [x] `stty size` 检查
- [x] I/O round-trip 检查
- [x] `top` 兼容性检查
- [x] `vim` 兼容性检查
- [x] `helpers/fork_exit` 僵尸回收检查
- [x] pane 退出后的基础清理检查
- [x] `:new / :focus / :kill` 状态变更检查
- [x] 只读 client 写命令拒绝检查（当前覆盖 `:new`）
- [x] 最后一个 pane 退出时 server/client 收尾检查

### 当前实现边界

- [x] 已支持多个 pane 在状态层和生命周期层存在
- [x] 已支持最小命令模式、pane 创建/销毁、焦点切换
- [x] 已支持 attach / reattach 基础链路，但尚无显式 `Ctrl+B d` detach 快捷键
- [x] 已支持多个 client 连接与只读 client 基础握手
- [x] 尚未实现多 pane 布局与结构化渲染
- [x] 尚未实现真正的多 pane 视图重绘与输入路由可视化
- [x] 尚未实现 `log / pipeout / capture`
- [x] 尚未实现递归 attach 保护

### 代码现状备注

- [x] `SessionState / ServerState / PaneState / ClientInputMode` 已成型
- [x] 协议已包含 `kClientCommand / kServerStatus / kServerRedraw` 等扩展消息类型
- [x] `kServerRedraw` 目前仅预留，尚未真正使用
- [x] `:new / :kill / :focus` 已落到最小 pane 生命周期和焦点状态变更
- [x] 当前 `./mini-tmux` 语义是“有 session 就 attach，没有就创建并 attach”
- [x] 仍缺少“禁止在 pane 内递归 attach 自己”的保护

---

## 1. 项目主线图

建议按下面顺序推进：

1. 稳定当前多 pane 生命周期与回归测试
2. 实现最小多 pane 布局和 resize 联动
3. 实现真实焦点切换、输入路由、信号隔离
4. 补齐 detach / reattach 语义
5. 稳定多 client / 只读 client 行为
6. 实现 `log / pipeout / capture`
7. 做异常路径、资源清理和回归测试

---

## 2. 模块 A：进程模型与系统骨架

目标：把角色边界固定住，避免后面继续堆零散状态。

### A1. 运行模式划分

- [x] 明确 `./mini-tmux` 是“首次启动或自动 attach”入口
- [x] 明确 `./mini-tmux attach` 是 attach 入口
- [x] 在同一个可执行文件内区分 server/client 行为
- [x] 明确只读 attach 的命令行入口 `./mini-tmux attach -r`

### A2. 关键对象建模

- [x] 定义 pane 的基本状态：`master_fd / child_pid / winsize / exit status`
- [x] 定义 client 连接对象
- [x] 定义 `SessionState`
- [x] 定义 `ServerState`
- [x] 把核心全局状态收敛为结构化对象
- [x] 为后续多 pane 扩展把 `SessionState` 升级为“pane 容器 + 焦点 + backlog”
- [ ] 继续扩展到“pane 容器 + 焦点 + 布局”

### A3. 协议与事件框架

- [x] 定义最小 client/server 消息类型：输入、resize、输出、退出
- [x] 扩展消息类型以支持命令模式、状态反馈和后续重绘
- [x] 协议编码/解码逻辑已经集中封装
- [ ] 在协议层补齐 pane 管理、重绘、布局同步等真正需要的消息

---

## 3. 模块 B：单 Pane 运行时

目标：把单 pane 这一层作为稳定地基保住。

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
- [x] 明确 pane state 枚举：`created / running / exited / reaped / destroyed`

### B4. 单 pane 验证

- [x] `tty` 检查
- [x] `isatty()` 检查
- [x] `stty size` 检查
- [ ] `helpers/probe` 接入验证
- [x] `helpers/fork_exit` 验证僵尸回收
- [x] `vim/top` 兼容性验证
- [ ] 单独补一条 resize 后 `SIGWINCH` 验证

---

## 4. 模块 C：Client 终端控制

目标：让 client 成为稳定、可扩展的前台交互层。

### C1. raw mode 管理

- [x] Client 启动时进入 raw mode
- [x] Client 退出时恢复终端属性
- [ ] 补充异常退出路径的恢复策略

### C2. 输入处理

- [x] 普通按键透传到 pane
- [x] 区分普通输入与前缀命令输入
- [x] 支持 `Ctrl+B` 前缀键状态机
- [x] 支持命令模式输入缓冲
- [ ] 支持更多 prefix 命令，例如 `Ctrl+B d / n / p`

### C3. 终端尺寸处理

- [x] 捕获 `SIGWINCH`
- [x] 将窗口大小通过 socket 发送给 server
- [x] 对异常 winsize 做兜底
- [ ] 多 client 场景下统一 winsize 策略

---

## 5. 模块 D：命令模式

目标：把“能输入命令”推进到“命令真的生效”。

### D1. 进入与退出命令模式

- [x] 支持 `Ctrl+B` 后按 `:` 进入命令模式
- [x] 命令模式下暂停向 pane 透传普通输入
- [x] 回车执行命令
- [x] `Esc` 或 `Ctrl+C` 取消命令模式

### D2. 命令解析器

- [x] 解析 `:new`
- [x] 解析 `:kill <pane_id>`
- [x] 解析 `:focus <pane_id>`
- [x] 让 `:new / :kill / :focus` 真正改动 session 状态
- [ ] 为后续 `:log` / `:pipeout` / `:capture` 预留更完整的解析接口

### D3. 错误反馈

- [x] 对非法命令返回明确错误信息
- [x] 对缺失参数返回明确错误信息
- [x] 统一命令执行结果的反馈格式

---

## 6. 模块 E：多 Pane 数据结构

目标：让“多个 pane”先在状态层成立，再谈显示层。

### E1. Pane 容器

- [x] 支持创建 Pane 0 之外的新 pane
- [x] 维护 pane 列表或映射表
- [x] 分配稳定的 pane id
- [x] 支持根据 pane id 查找 pane

### E2. Pane 生命周期管理

- [x] `:new` 创建新 pane
- [x] `:kill` 销毁指定 pane
- [x] child 退出后自动标记 pane 不可用
- [x] 最后一个 pane 退出时定义系统行为

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
- [x] 当前会把 client winsize 广播到所有 pane 的 PTY
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

- [x] 保存当前焦点 pane id
- [x] 新 pane 创建后的焦点策略：默认切到新 pane
- [x] pane 销毁后的焦点迁移策略：选择下一个可用 pane

### H2. 焦点切换接口

- [x] `:focus <pane_id>`
- [ ] `Ctrl+B n` 切到下一个 pane
- [ ] `Ctrl+B p` 切到上一个 pane

### H3. 输入路由

- [x] 普通输入只发给焦点 pane
- [x] 命令输入只进入命令模式缓冲
- [x] 只读 client 禁止发送普通输入
- [~] 只读 client 已禁止 `:new / :kill`，其余写命令待补齐

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
- [x] client 退出或断开后 server 不会因无 client 立即退出
- [x] pane 子进程继续运行

### J2. reattach 语义

- [x] `./mini-tmux attach` 连接已有 server
- [~] attach 后恢复当前 pane 输出视图（当前依赖 backlog，未达到真正屏幕重建）
- [x] attach 后继续交互

### J3. 无 client 状态

- [x] 所有 client 断开后 server 继续运行
- [x] 新 client 随后可重新 attach
- [ ] 防止在已有 pane 内递归 attach 自己导致输出回环

---

## 12. 模块 K：多 Client

目标：支持多个终端同时连到同一个 session。

### K1. 多 client 管理

- [x] 支持多个 client 同时 attach
- [x] 新 client attach 时同步 backlog 输出
- [x] 一个 client 断开不影响其他 client
- [ ] 新 client attach 时同步“当前完整视图”而不是仅 backlog

### K2. 输入权限

- [x] 默认读写 client 可输入
- [x] `attach -r` 只读 client 仅接收输出
- [x] server 忽略只读 client 的普通输入
- [x] server 拒绝只读 client 的 `:new / :kill`
- [ ] server 拒绝所有只读 client 的状态修改命令集合

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
- [x] client 断开时释放 socket

### M2. 进程回收

- [x] 已有基础 `SIGCHLD + waitpid()`
- [ ] 扩展到 pane 子进程之外的 `pipeout/log` 子进程
- [x] 当前单 pane + `fork_exit` 场景已验证无 zombie 堆积
- [ ] 补齐 server 子进程自身的回收策略

### M3. 异常路径

- [x] 已忽略 `SIGPIPE`
- [ ] client 异常退出路径补齐
- [ ] server 启动失败路径进一步补齐
- [ ] socket 残留文件恢复策略补齐

---

## 15. 模块 N：测试与回归

目标：每做完一个模块，都有办法验证，而不是最后一起爆炸。

### N1. 当前已有测试

- [x] `test_single_pane.py`
- [x] `test_multi_pane_commands.py`

### N2. 下一步要补的测试

- [ ] `test_single_pane_probe.py`
- [ ] `test_single_pane_resize.py`
- [ ] `test_multi_pane_layout.py`
- [ ] `test_focus_and_signal.py`
- [ ] `test_detach_reattach.py`
- [ ] `test_multi_client.py`
- [ ] `test_log_pipe_capture.py`
- [ ] `test_recursive_attach_guard.py`

### N3. 回归检查清单

- [x] 每次改协议后重跑基础单 pane 测试
- [x] 当前已补一条多 pane 命令生命周期测试
- [ ] 每次改布局后重跑 winsize 相关测试
- [ ] 每次改生命周期后重跑 zombie/fd 清理测试

---

## 16. 当前推荐的下一步

按现在这份代码的真实状态，最稳的顺序是：

- [x] 第一步：让 `:new / :kill / :focus` 真正修改 session 状态，而不是只做解析
- [x] 第二步：把 `SessionState` 从单 pane 扩成 pane 容器，并引入稳定 pane id
- [ ] 第三步：实现最简单的垂直等分布局和多 pane winsize 分发
- [ ] 第四步：实现真实焦点切换与输入路由可视化
- [ ] 第五步：补齐 `Ctrl+B d` detach、完整 reattach 视图恢复、递归 attach 保护
- [ ] 第六步：稳定多 client 尺寸协调与只读语义
- [ ] 第七步：实现 `log` / `pipeout` / `capture`
- [ ] 第八步：补齐异常退出、server 回收、socket 残留恢复与回归测试

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

这份文档应该持续和代码同步。凡是代码里已经有的框架、限制和测试结论，都应该及时体现在这里，而不是继续保留过时状态。
