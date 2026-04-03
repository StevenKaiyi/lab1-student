# mini-tmux 调试记录

## 单 pane 基线验证

在 VSCode 的 WSL 终端中运行：

```bash
./mini-tmux
```

程序可以正常启动，并进入 mini-tmux 管理的单 pane shell。

对应的基础回归测试：

```bash
python3 test_single_pane.py
```

当前这组测试已经通过，说明单 pane 的 PTY、I/O 转发和退出清理链路基本成立。

## 已验证的行为

`test_single_pane.py` 当前覆盖并通过的检查包括：

- shell ready check
- pty path check
- isatty check
- winsize check
- io round-trip check
- top compatibility check
- vim compatibility check
- fork_exit/zombie reap check
- exit/cleanup check

## 关于 `./mini-tmux` 的 attach 语义

当前 `main()` 的行为是：

- 如果直接运行 `./mini-tmux`，会先检查默认 socket 是否已经存在
- 如果默认 session 已存在，则自动 attach 到已有 server
- 如果默认 session 不存在，则启动一个新的 server，并 attach 到它

因此，`./mini-tmux` 现在的语义不是“无条件新建 session”，而是“有 session 就 attach，没有就创建并 attach”。

这也解释了为什么在某些情况下再次运行 `./mini-tmux`，看到的是进入已有会话，而不是新开一个全新的 pane。

## 如何确认自己已经进入 mini-tmux

进入后，可以先看到本地提示：

- `mini-tmux: attached`
- 只读 attach 时为 `mini-tmux: attached in read-only mode`

退出时会看到：

- `mini-tmux: detached`

在 pane 内也可以继续做这些检查：

```bash
tty
echo $$
python3 -c "import os; print(os.isatty(0), os.isatty(1), os.isatty(2))"
```

如果 `tty` 指向的是一个 PTY，且三个标准流都是 TTY，说明当前 shell 确实跑在 mini-tmux 的 pane 里。

## 递归 attach 导致输出回环

现象：已经进入 `mini-tmux` 的 pane 后，又在同一个 pane 里执行 `./mini-tmux attach`，终端会反复打印同样的命令和 shell 错误输出，表现为刷屏且难以终止。

原因：第二个 `mini-tmux` client 的 `stdin/stdout` 绑定到了第一个 session 的 pane PTY。server 把 pane 输出发给 client，client 又把内容写回同一个终端，形成输出回环。

结论：`./mini-tmux attach` 必须从另一个普通终端启动，不能在已经 attach 的 pane 内再次启动。后续应补一层保护，检测当前终端是否已经是本 session 的 pane，避免递归 attach。

## Windows PowerShell 直接运行 ELF 的问题

如果在 Windows PowerShell 中，直接从 `\\wsl.localhost\...` 路径执行：

```powershell
./mini-tmux
```

Windows 不会把它当作 Linux 程序交给 WSL 执行，而是把它当成普通文件，从而弹出“选择用什么程序打开”的窗口。

这不是 `mini-tmux` 运行时逻辑的问题，而是 Windows 无法直接执行 Linux ELF 文件。

正确做法是先进入 WSL，再运行：

```powershell
wsl
cd /home/kaiyi/lab1-student
./mini-tmux
```

或者一条命令：

```powershell
wsl bash -lc "cd /home/kaiyi/lab1-student && ./mini-tmux"
```

## 当前阶段的结论

当前项目已经完成并验证了单 pane 的核心闭环：

- client raw mode
- PTY 启动 shell
- 输入透传
- 输出回传
- resize 基础处理
- 退出清理

接下来的主线仍然是：

1. 修稳 `Ctrl+B` 前缀状态机
2. 完成 `Ctrl+B :` 命令模式
3. 实现最小命令执行路径，例如 `:new` / `:focus` / `:kill`

## `./mini-tmux` 进入后立刻 detached

现象：在 WSL 终端里直接运行：

```bash
./mini-tmux
```

终端只短暂打印：

- `mini-tmux: attached`
- `mini-tmux: detached`

随后立刻返回 shell，看起来像“刚进入就退出了”。

原因：当前 `main()` 的默认语义不是“无条件新建 session”，而是：

- 先尝试连接默认 socket
- 如果默认 session 已存在，就直接 attach
- 如果默认 session 不存在，才启动新的 server 并 attach

因此，当 `/tmp/mini-tmux-<uid>-default.sock` 仍然存在、且有旧的默认 session/server 残留时，`./mini-tmux` 会优先 attach 到这个已有 session，而不是新建一个干净会话。

这次排查时，WSL 中可以看到：

- 默认 socket 仍然存在：`/tmp/mini-tmux-1000-default.sock`
- 还有多个历史 `mini-tmux` 进程残留

在这种情况下，如果 attach 到的是一个状态异常、空壳、或即将关闭连接的旧 session，client 就会先打印 `attached`，随后因为连接关闭或 session 结束而立即打印 `detached`。

结论：这类“进入就立刻 detached”的现象，不一定是新启动的 session 立即崩掉，更常见的是误 attach 到旧的默认 session。

临时规避方式：

```bash
MINI_TMUX_SERVER=fresh ./mini-tmux
```

这样会使用新的实例名，避免误连到旧的默认 session。

后续建议：

- 增加对默认 session 健康状态的判断
- 或提供更明确的“强制新建 session”入口
- 或在调试/测试前先清理残留 socket 和旧 server 进程
## 单 pane 交互渲染异常

现象：进入 `./mini-tmux` 后，单 pane shell 交互存在几类明显异常：

- prompt 前会直接漏出类似 `[?2004h]0;kaiyi@...` 的控制序列
- 光标停在窗口最底部，而不是当前 shell 输入位置
- 未进入 `Ctrl+B` 前缀时，backspace 等编辑键表现不正常
- 例如输入 `tty` 后按 backspace，屏幕上会出现类似 `tty[K` 的脏输出

原因：早期 client 对所有 pane 输出都走“简化结构化渲染”。这套渲染只会粗略过滤部分控制字符，但不会正确解析真实 shell/PTTY 输出里的终端控制序列，例如：

- bracketed paste：`\x1b[?2004h` / `\x1b[?2004l`
- OSC 窗口标题：`\x1b]0;...\x07`
- 颜色控制序列：`\x1b[01;32m`
- 行编辑相关序列：`\b\x1b[K`

这些字节本来应该由真实终端解释，但被 mini-tmux client 当成普通文本或半截文本处理后，就会直接显示到屏幕上，进而导致：

- 控制序列外漏
- 光标位置计算错误
- shell 自己的行编辑效果被破坏

修复：将 client 的渲染策略拆成两种模式：

- 单 pane：直接透传 PTY 原始输出到当前终端
- 多 pane：继续使用结构化重绘

这样做的原因是：

- 单 pane 时，最重要的是保持“像真实终端一样”的 shell 交互语义
- 多 pane 时，必须由 client 自己负责布局和重绘，因此仍需要结构化渲染

另外，`Ctrl+B :` 命令模式在单 pane 下不再强制整屏重绘，而是单独绘制和清除底部命令提示，避免破坏当前 shell 光标与屏幕状态。

验证结果：

- `python3 test_interactive_render.py` 已通过
- `python3 test_single_pane.py` 已通过
- `python3 test_focus_prefix.py` 已通过
- `python3 test_detach_reattach.py` 已通过
- `python3 test_multi_pane_layout.py` 已通过

结论：这组修复不违背 handout。handout 要求的是：

- Client 必须进入 raw mode，以便拦截 `Ctrl+B`
- Client 退出时必须恢复终端

handout 并没有要求把 shell 的终端控制序列显示给用户看。相反，单 pane 直通 PTY 输出更符合真实终端语义，也更符合交互预期。
## 多 pane 重绘污染 scrollback

现象：进入 `mini-tmux` 后，只要新建 pane、切换焦点或触发重绘，终端窗口往上翻就能看到大量重复的历史“画面快照”，例如：

- 一长条分隔线 `-----...`
- `Pane 1 [active]`
- `Pane 0`
- pane 内当时的 shell prompt

这些内容会在 scrollback 里累积很多屏，看起来像每次重绘都被当成了新的终端输出追加上去。

原因：多 pane 模式下，client 需要自己负责整屏布局和重绘；之前的实现虽然每次都用 `\x1b[H\x1b[2J` 回到左上角并清屏，但仍然是在终端的主屏缓冲区里做整屏刷新。对于普通终端来说，这些整屏内容依然属于“正常输出历史”，因此会被保存进 scrollback。

所以问题不在于分隔线本身，也不在于 pane 内容重复发送，而在于：

- 多 pane 重绘没有切到 alternate screen
- 导致每一帧布局快照都污染了主终端的滚动历史

修复：为结构化多 pane 渲染增加 alternate screen 管理：

- 进入多 pane 结构化渲染时，client 发送 `\x1b[?1049h`
- 退出结构化渲染、回到单 pane 直通模式或 client 退出时，发送 `\x1b[?1049l`
- 之后所有多 pane redraw 都在 alternate screen 内原位刷新，而不是堆进主屏 scrollback

实现后的语义是：

- 单 pane：保持 PTY 直通，行为尽量接近真实 shell
- 多 pane：进入类似全屏 TUI 的独立渲染表面，由 client 自己控制布局

已重新验证的回归：

- `python3 test_focus_prefix.py`
- `python3 test_detach_reattach.py`
- `python3 test_multi_pane_layout.py`
- `python3 test_interactive_render.py`

结论：这个问题本质上是”渲染表面选错了”，不是 pane 生命周期错误。多 pane 结构化重绘必须使用 alternate screen，否则 scrollback 被历史帧淹没几乎是必然结果。

## 多 Pane 渲染问题分析

本节汇总多 pane 模式下渲染出现的主要 bug 及其根本原因。

### 症状概述

在真实终端运行 mini-tmux 时，多 pane 模式可能出现以下问题：

- 光标位置不对
- 输出串行/画面撕裂
- 高频输出时卡死
- 画面闪烁

---

### 问题 1: ANSI 转义序列解析不完整

**位置**: `handle_csi_final()` (mini_tmux.cpp:513-527)

```cpp
void handle_csi_final(ClientPaneBuffer &buffer, char final_byte) {
    if (final_byte == 'K') {        // 清除行尾
        if (buffer.cursor_col < buffer.current_line.size()) {
            buffer.current_line.erase(static_cast<std::string::size_type>(buffer.cursor_col));
        }
    } else if (final_byte == 'D') { // 光标左移
        if (buffer.cursor_col > 0) {
            --buffer.cursor_col;
        }
    } else if (final_byte == 'C') { // 光标右移
        if (buffer.cursor_col < buffer.current_line.size()) {
            ++buffer.cursor_col;
        }
    }
}
```

**问题**:

- CSI 序列支持参数，如 `\x1b[10C` 表示右移 10 个字符，但代码**没有解析参数**
- 缺少常用的光标移动序列：`H`(定位)、`A`(上移)、`B`(下移)、`J`(清除屏幕)
- 不支持颜色/样式序列（影响 `ls --color`、`grep --color` 等）

**影响**: 导致光标位置计算错误、带颜色输出显示异常

---

### 问题 2: 频繁完整重绘导致性能问题

**位置**: 事件循环 (mini_tmux.cpp:1737-1741)

```cpp
} else if (use_structured_client_render(view_state)) {
    if (!ensure_render_surface() ||
        !render_client_view(view_state, command_buffer, input_mode)) {
        done = true;
    }
}
```

**问题**:

- **每次收到任何输出都完整重绘整个屏幕**
- 涉及大量字符串拼接和 `write_all()` 调用
- 多 pane 高频输出时（如 `yes` 命令）会导致严重卡顿

**影响**: 高频输出场景下 CPU 占用高、响应延迟

---

### 问题 3: 光标位置计算存在边界问题

**位置**: `render_client_view()` (mini_tmux.cpp:680-699)

```cpp
if (pane.focused) {
    cursor_positioned = true;
    if (body_rows > 0) {
        const int current_line_index = std::max<int>(0, static_cast<int>(visible_lines.size()) - 1);
        int cursor_offset = current_line_index - start;
        if (cursor_offset < 0) {
            cursor_offset = 0;  // ← 问题：可能越界
        }
        if (cursor_offset >= body_rows) {
            cursor_offset = body_rows - 1;
        }
        cursor_row = header_row + 1 + cursor_offset;
```

**问题**:

- 当 `visible_lines` 为空时，`current_line_index = 0`，但实际应该反映真实状态
- `visible_lines` 是向量拷贝，如果 `buffer.lines` 很大，拷贝开销大

**影响**: 光标可能出现在错误位置

---

### 问题 4: 多 pane 并发输出导致渲染混乱

**位置**: 没有输出合并机制

**问题**:

- Pane1 输出 → 重绘 → Pane2 输出 → 重绘 → Pane1 输出 → 重绘
- 没有在事件循环中批处理多个消息
- `write_stdout()` 不是原子操作，可能导致画面撕裂

**影响**: 多 pane 并发输出时画面闪烁、内容错乱

---

### 问题 5: `\r\n` 处理不一致

**位置**: `append_client_output()` (mini_tmux.cpp:539-546)

```cpp
if (ch == '\r') {
    buffer->current_line.clear();
    buffer->cursor_col = 0;
    continue;
}
if (ch == '\n') {
    handle_newline(*buffer);
    continue;
}
```

**问题**:

- Windows 换行 `\r\n` 会被处理为清空行+换行
- 但某些程序（如 `cat` 文件）可能输出单独的 `\r` 或 `\n`
- 没有跟踪 `\r` 后是否需要保留原行内容

**影响**: 某些程序的输出显示异常

---

### 问题 6: 备用屏幕进入/退出时序问题

**位置**: `ensure_render_surface()` (mini_tmux.cpp:1546-1563)

```cpp
auto ensure_render_surface = [&]() -> bool {
    if (!use_structured_client_render(view_state)) {
        if (alternate_screen_active) {
            if (!leave_alternate_screen()) {
                return false;
            }
            alternate_screen_active = false;
        }
        return true;
    }
    if (!alternate_screen_active) {
        if (!enter_alternate_screen()) {
            return false;
        }
        alternate_screen_active = true;
    }
    return true;
};
```

**问题**:

- `use_structured_client_render()` 基于 `layout.size() > 1`
- 但在单 pane 时如果收到输出，可能延迟进入结构化渲染
- 这会导致单 pane 输出先显示原始内容，多 pane 创建后突然切换渲染模式

**影响**: 单 pane 到多 pane 切换时可能短暂出现原始控制序列

---

### 问题 7: write_stdout 部分失败未正确处理

**位置**: `render_client_view()` (mini_tmux.cpp:724)

```cpp
frame += “\x1b[?25h”;
return write_stdout(frame);
```

**问题**:

- `write_stdout()` 可能只写入部分数据就返回错误
- 但没有检测是否完整写入整个 `frame`
- ANSI 序列被截断会导致终端进入异常状态

**影响**: 渲染失败后终端状态异常

---

### 问题总结表

| 症状 | 根本原因 | 影响代码位置 |
|------|---------|-------------|
| 光标位置不对 | ANSI 序列解析不完整，无法追踪正确光标位置 | `handle_csi_final()` |
| 输出串行/撕裂 | 高频输出时每次都完整重绘，无输出合并机制 | 事件循环 1737-1741 |
| 卡死 | 高频输出触发频繁重绘，字符串拼接+write 阻塞主循环 | `render_client_view()` |
| 画面闪烁 | 备用屏幕切换时序问题，清屏后重绘 | `ensure_render_surface()` |
| 带颜色输出异常 | 不支持颜色控制序列 | `handle_csi_final()` |

---

### 建议修复方向

1. **实现完整 ANSI 解析器** - 支持 CSI 参数和常用序列
2. **输出合并/节流** - 在事件循环中批量处理多个输出消息
3. **差分渲染** - 只重绘变化的部分，而非整个屏幕
4. **使用终端库** - 如 ncurses、libtickit 等处理复杂情况
5. **缓冲区优化** - 避免大量向量拷贝

---

## ???????????

?????????????????????????????????????????? `mini-tmux` ???? screenshot ??????????

- `screenshot/screenshot_debug.py`
- `screenshot/debug_inputs/`
- `screenshot/screenshot-debug/`

### ??

???????????????

- ?????????????????
- `Ctrl+B` ????????? pane??????????
- ? `mini-tmux` ??????????????????
- ???? bug ????? ANSI ????????????

???? `session.raw`???????????

- ??????????
- ??????????????
- ???????????????
- ?????????????????????

????????????????? I/O ???

### ??????

`screenshot/screenshot_debug.py` ?????

1. ? PTY ??????? `mini-tmux` client
2. ? JSON ??????????
3. ? key token ??????????????`<Enter>`?`<C-b>`
4. ???????????? quiet window
5. ???????????? `delta.raw`
6. ????????????????????????? `screen.txt`
7. ?????? `meta.json`

??????? run ????????

```bash
screenshot/screenshot-debug/20260325-171454-multi_pane_command_mode/
```

????????

```bash
step-0000/
step-0001/
...
```

?????

- `delta.raw`?????????
- `screen.txt`???????????
- `meta.json`??????????????????delta ????

?? run ????????

- `session.raw`?????????
- `scenario.json`??????????

### ????????????

??????????????

- `screenshot/debug_inputs/basic_single_pane.json`
- `screenshot/debug_inputs/multi_pane_command_mode.json`

?????

```bash
python3 screenshot/screenshot_debug.py screenshot/debug_inputs/basic_single_pane.json
python3 screenshot/screenshot_debug.py screenshot/debug_inputs/multi_pane_command_mode.json
```

### ??? screenshot ?????? 3 ?????

#### ?? 1?? pane ???????????

????? pane ????? `Ctrl+B :new` ????????????? `:new`??????

- `:`
- `:n`
- `:ne`
- `:new`

????????????

??????? `multi_pane_command_mode` run ???????

- `step-0002/screen.txt`
- `step-0003/screen.txt`
- `step-0004/screen.txt`
- `step-0005/screen.txt`

???? pane ? `redraw_command_prompt()` ????? `
:`?????????????????????????

???

- ????????????? `begin_command_prompt()`
- ?????????????? `:` ??

#### ?? 2?? pane ?????????

?????? pane ?????? pane header ?????????????? pane ? header ?????

?????

- ? screenshot ????? `Pane 1` header ?????? header ???
- ??? `session.raw`????? `Pane 0` ????? `mini-tmux` ?????

??????? server ????? client ???????????

???`render_client_view()` ??? `pane.rows` ?? header + body??????????????????????????? pane ???

???

- ? body ????????????? pane ????
- ?????? pane ???`reserved_rows = 1(header) + 1(separator)`

???????????? layout ??????

#### ?? 3?`kServerStatus` ?? pane ??

????? pane ????? `:focus 1` ?????????

```text
kaiyi@...$ ok: focused pane 1
```

?????server ??????????? pane ? shell ???

????????

- `step-0049/screen.txt`
- `step-0071/screen.txt`

???client ?? `kServerStatus` ?? `kServerOutput` ?????? `append_client_output()` ???????????? pane ???????

???

- `kServerStatus` ???? pane buffer
- ???? client ??? `local_status`
- ????????????? active pane ? header ???

???????????????? pane ???

### ? screenshot ??????????

??????????? bug??????????

1. `mini-tmux` ???????
2. `screenshot_debug.py` ????????????????

? `screen.txt` ? `session.raw` ???????????? `session.raw`???? screenshot ????????? ANSI ???

????? pane header ??????????

- `screen.txt` ???????? header ???
- ? `session.raw` ?????? `Pane 0` ? frame ??

????????????????????????????? ground truth?

### ?????????

????????????????????????

1. ?????? `debug_inputs/*.json` ??
2. ? `python3 screenshot/screenshot_debug.py <scenario>`
3. ??????? `screen.txt`
4. ???? screenshot ???????????? `session.raw`
5. ???????? `mini_tmux.cpp` ???

??????????????????????????

## `tty -> :new -> tty -> :focus 0 -> tty` 后光标掉到分割线附近

### 复现

新增了一个精确场景：

- `screenshot/debug_inputs/tty_new_focus_tty.json`

步骤和手工复现一致：

1. pane 0 里执行 `tty`
2. `Ctrl+B :new`
3. pane 1 里执行 `tty`
4. `Ctrl+B :focus 0`
5. pane 0 再执行 `tty`

初次复现产物：

- `screenshot/screenshot-debug/20260326-102357-tty_new_focus_tty/`

### 现象

在 `:focus 0` 之后，active pane 的光标没有回到 pane 0 的 prompt，而是跳到了分割线附近。随后输入 `tty` 时，字符会出现在 pane 0 中部，看起来像“光标掉到 divider 上方一行”。

关键证据：

- `step-0027/meta.json` 里光标在第 11 行，而不是 pane 0 prompt 所在的第 4 行
- `step-0031/screen.txt` 里，`tty` 被画在 pane 0 中部
- `step-0027/delta.raw` 里可以看到 `:focus 0` 之后的重绘把光标放到了 `CSI 11;34H`

### 根因

`TextScreenBuffer` 之前只有：

- `lines`
- `current_line`
- `cursor_col`

这个模型默认“当前输入行永远是最后一行”。但 shell/readline 在 `tty`、resize、focus 切换之后会发带光标移动的 CSI 序列；旧实现没有跟踪 `cursor_row`，也没有保存 CSI 参数，因此：

- 旧 prompt 会保留在上面的历史行里
- client redraw 时又把“最后一行”当成当前输入行
- focused pane 的光标最终被错误地算到了 body 底部附近

### 第一轮修复

修复点：

- `mini_tmux.cpp`

改动：

- 给 `TextScreenBuffer` 增加 `cursor_row` 和 `csi_buffer`
- 把 pane capture 从“最后一行模型”改成“多行 + 行坐标模型”
- 解析并处理常见的 CSI 光标/清屏控制序列：`A/B/C/D/H/f/J/K`
- `build_redraw_payload()` 改为把每个 pane 的 `cursor_row` 和 `cursor_col` 一起发给 client
- `parse_redraw_buffers()` / `render_client_view()` 改为按真实 `cursor_row` 定位 focused pane 光标

### 第一轮验证

重新编译后运行：

```bash
python3 screenshot/screenshot_debug.py screenshot/debug_inputs/tty_new_focus_tty.json
```

验证产物：

- `screenshot/screenshot-debug/20260326-104729-tty_new_focus_tty/`

结果：

- `step-0027/meta.json` 里光标从原来的第 11 行回到了第 4 行
- `step-0031/meta.json` 里继续输入 `tty` 时光标仍在第 4 行

这说明“光标掉到 divider 附近”的主问题已经被修掉。

## 剩余问题：光标仍比预期低一行，`Pane 0` 头偶尔消失

### 现象

第一轮修完后还剩两个显示问题：

- 光标有时会出现在“应该在的位置的下一行”
- `Pane 0 [active]` 的 header 偶尔会被顶掉，只剩下 `Pane 1`

### 根因

问题不在 pane capture，而在 client 端的整屏重绘方式。

旧版 `render_client_view()` 是这样画帧的：

- 先 `CSI H` + `CSI 2J`
- 然后把每一行内容直接顺序拼到一个字符串里
- 行与行之间用 `\r\n`

这在 pane 宽度刚好等于终端宽度时会有副作用：

- header 或分隔线写满一整行以后，终端可能先触发自动换行
- 紧接着再收到 `\r\n`，就会多吃掉一行
- 后面的 pane header / body / cursor 定位都会整体下沉，表现成 header 消失或光标低一行

也就是说，第一轮修复解决了“cursor_row 算错”，第二轮修复解决的是“frame 写法本身会让终端多走一行”。

### 第二轮修复

仍然修改：

- `mini_tmux.cpp`

把 `render_client_view()` 改成：

- 帧开始时临时关闭自动换行：`CSI ?7l`
- 每一行都用绝对坐标写：`CSI <row>;1H`
- 写完本行内容后再用 `CSI K` 清尾
- 帧结束前恢复自动换行：`CSI ?7h`

这样每一行都固定落在预期的屏幕行号上，不再依赖“打印满宽文本 + CRLF”的副作用。

### 第二轮验证

注意：有一次验证把 `make` 和截图复现并行跑了，场景拿到了旧二进制，所以看到的还是旧 frame。后面改成串行验证。

最终验证命令：

```bash
make
python3 screenshot/screenshot_debug.py screenshot/debug_inputs/tty_new_focus_tty.json
```

最终验证产物：

- `screenshot/screenshot-debug/20260326-110053-tty_new_focus_tty/`

关键证据：

- `step-0031/delta.raw` 的十六进制开头已经变成：`ESC [?25l ESC [?7l ESC [H ESC [2J ESC [1;1H ...`
- 说明新 renderer 的“禁用自动换行 + 按绝对行号写”已经真正生效
- `step-0027/screen.txt` 里 `Pane 0 [active] | ok: focused pane 0` 仍然存在
- `step-0031/screen.txt` / `step-0034/screen.txt` 里 `Pane 0 [active]` 不再消失
- `step-0031/meta.json` 里光标在第 4 行第 37 列，和 pane 0 prompt 所在行一致

### 结论

这组问题实际上分成两层：

1. pane capture 没有跟踪 `cursor_row`，导致 focus 后把 prompt 错认成 pane 底部附近的一行
2. client redraw 用顺序文本 + CRLF 刷整屏，导致满宽行触发自动换行，进而把 header 和光标整体顶歪

两轮修完之后：

- focus 后光标不会再掉到 divider 附近
- `Pane 0` header 不会再被吃掉
- 光标也不会再稳定地下沉一行

## `Ctrl+B :...` 命令模式回显位置错误

### 初始现象

按 `Ctrl+B` 进入命令模式后，输入的命令（例如 `:new`）会回显到终端最底下一行，而不是当前 active pane 的正确位置。

最初使用的复现场景：

- `screenshot/debug_inputs/multi_pane_command_mode.json`

对应 run：

- `screenshot/screenshot-debug/20260326-110549-multi_pane_command_mode/`

关键证据：

- `step-0002/screen.txt` 里只在最后一行看到 `:`
- `step-0005/screen.txt` 里 `:new` 也仍然在最后一行
- 根因在 `render_client_view()` 命令模式分支里直接用了 `CSI 999;1H`

也就是 client 一旦进入 `ClientInputMode::kCommand`，就把命令缓冲强制画到底部，而没有考虑当前 pane 的真实光标位置。

### 第一次尝试：把命令回显放到 pane header

第一反应是：既然底行不对，那就把命令缓冲并到 active pane 的 header 上。

对应修改后，重新跑：

- `screenshot/screenshot-debug/20260326-111027-multi_pane_command_mode/`

结果：

- `step-0002/screen.txt` 变成 `Pane 0 [active] | :`
- `step-0005/screen.txt` 变成 `Pane 0 [active] | :new`

这说明“底行回显”问题被消掉了，但这个修法仍然不对，因为命令并不应该永远固定在 pane 第一行。

### 更精确的反例

用户给出了更准确的场景：

1. pane 0 里执行 `tty`
2. `Ctrl+B :new`
3. pane 1 里执行 `tty`
4. 在 pane 1 里按 `Ctrl+B :new`

这时 `:new` 应该出现在 pane 1 当前 prompt 那一行，而不是 pane 1 header。

为此新增了一个专门场景：

- `screenshot/debug_inputs/pane1_command_prompt_position.json`

第一次跑这个场景时，仍然能看到错误行为：

- `screenshot/screenshot-debug/20260326-111450-pane1_command_prompt_position/`
- `step-0019/screen.txt` 到 `step-0022/screen.txt` 里，`:`, `:n`, `:ne`, `:new` 都出现在 `Pane 1 [active]` 那一行

同时，进入命令模式之前 pane 1 的真实光标位置其实已经很明确：

- `step-0017/meta.json` 里光标在第 16 行第 34 列

也就是说，真正正确的行为应该是：命令缓冲跟随 active pane 当前的光标行，而不是固定在 header，也不是固定在底行。

### 最终修复

仍然修改：

- `mini_tmux.cpp`

做法：

1. 先像普通模式一样，根据 active pane 的 `cursor_row` / `cursor_col` 计算出真实光标位置
2. 如果 `input_mode == ClientInputMode::kCommand`：
   - 不再把命令写到底行
   - 也不再把命令拼到 header
   - 而是在 active pane 当前光标位置覆盖写入 `:` + `command_buffer`
3. 再把光标放到命令字符串末尾

这样命令模式就真正“贴着 pane 当前输入位置”显示，而不是依附于某个固定 UI 行。

### 最终验证

注意：中间又有一次把 `make` 和复现并行跑了，导致截图吃到旧二进制；后面改成串行验证。

最终串行验证：

```bash
make
python3 screenshot/screenshot_debug.py screenshot/debug_inputs/pane1_command_prompt_position.json
```

最终 run：

- `screenshot/screenshot-debug/20260326-111909-pane1_command_prompt_position/`

关键证据：

- `step-0019/screen.txt` 里，pane 1 的 prompt 行变成 `kaiyi@...$ :`
- `step-0022/screen.txt` 里，同一行变成 `kaiyi@...$ :new`
- `step-0022/meta.json` 里光标在第 16 行第 38 列
- 这和进入命令模式前 `step-0017/meta.json` 给出的 pane 1 prompt 所在行是一致的

### 结论

命令模式回显位置这个问题一共经历了三步定位：

1. 先确认旧逻辑是硬编码到底行（`CSI 999;1H`）
2. 再确认“改到 pane header”虽然消除了底行回显，但仍然不是正确语义
3. 最终确定正确行为应该是：命令缓冲覆盖在 active pane 当前真实光标位置

最终修完后：

- 命令模式不再回显到底行
- 也不会固定显示在 pane 第一行
- 而是跟随 active pane 当前 prompt 所在行显示
