# Claude Review Notes

## 范围

这份文档只记录本轮 Claude Code 评审中“有价值、值得后续跟踪”的意见。
明显误判或当前阶段并不成立的问题，不在这里展开。

## 1. Server 父进程回收不完整

- 现象：`start_server_and_attach()` 中 fork 出 server 子进程后，父进程没有显式 `waitpid()` 回收它。
- 风险：如果 server 先退出、client 父进程仍短暂存活，理论上可能出现 zombie 进程窗口。
- 当前判断：这是一个真实的清理问题，但更偏工程完整性问题，不是当前阶段最致命的功能 bug。
- 后续建议：
  - 可以在父进程中增加 `SIGCHLD`/`waitpid()` 回收逻辑，或
  - 改成更明确的 daemon 化/双重 fork 方案。

## 2. Client 异常退出时终端恢复不足

- 现象：`TerminalModeGuard` 依赖正常控制流和析构来恢复 raw mode。
- 风险：如果 client 因 `SIGSEGV`、`SIGABRT` 等异常信号退出，析构不会执行，用户终端可能残留在 raw mode。
- 当前判断：这是一个真实问题，而且和当前计划中 `C1` 尚未完成的“异常退出路径恢复策略”一致。
- 后续建议：
  - 为 client 增加异常信号下的终端恢复机制；
  - 至少覆盖常见致命信号下的 termios 恢复。

## 结论

当前这两条意见值得保留并在后续迭代中处理：

1. server 子进程回收
2. client 异常退出时的终端恢复
