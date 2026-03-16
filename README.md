# Lab 1: mini-tmux

从零实现一个终端复用器。完整需求见 [handout.md](handout.md)。

## 快速开始

```bash
# 编译（需要 C++17）
make

# 运行
./mini-tmux
```

## 文件结构

```
├── handout.md                  # 完整的实验需求文档（从这里开始读）
├── Makefile                 # 编译规则：mini_tmux.cpp → mini-tmux
├── helpers/
│   ├── probe                # 评测探针，用于验证 PTY 环境和 I/O 管道（详见 handout4.1 节）
│   └── fork_exit            # 评测辅助程序，用于僵尸进程回收测试（详见 handout4.1 节）
├── workloads/
│   └── public/              # 公开测试用例（YAML 格式，可阅读了解评测内容）
└── .github/
    └── workflows/
        └── classroom.yml    # GitHub Classroom 自动评分 CI
```

你需要创建 `mini_tmux.cpp`（或修改 Makefile 以适配你的源文件组织方式），从零实现 mini-tmux。

## 提交

推送到 GitHub Classroom 分配的仓库。确保 `make` 能在干净的 Linux 环境中编译成功。

AI 使用记录的要求见 handout.md 4.7 节。
