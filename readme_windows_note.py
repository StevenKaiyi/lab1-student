from pathlib import Path
path = Path('/home/kaiyi/lab1-student/docs/README.md')
text = path.read_text(encoding='utf-8')
needle = """```bash
# 编译（需要 C++17）
make

# 运行
./mini-tmux
```
"""
insert = needle + """
如果你是在 Windows PowerShell 里从 `\\wsl$` 或 `\\wsl.localhost` 路径打开这个仓库，不要直接运行 `./mini-tmux`。这个文件是 Linux ELF，可执行权属于 WSL，PowerShell 会把它当成需要选择打开程序的文件。

请改用：

```powershell
./mini-tmux.ps1
```

attach 也一样：

```powershell
./mini-tmux.ps1 attach
./mini-tmux.ps1 attach -r
```
"""
if needle not in text:
    raise SystemExit('README anchor not found')
text = text.replace(needle, insert, 1)
path.write_text(text, encoding='utf-8')
