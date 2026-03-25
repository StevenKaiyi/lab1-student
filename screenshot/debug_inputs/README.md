# Debug Input Flows

These JSON files are sample input flows for `screenshot_debug.py`.

Each file contains:

- `rows` / `cols`: PTY size used for the client
- `settle_ms`: default quiet window after each key
- `steps`: a sequence of actions

Supported step types:

- `{"type": "keys", "keys": "echo hi<Enter>"}`
- `{"type": "wait", "ms": 500}`
- `{"type": "snapshot", "label": "optional manual checkpoint"}`

Inside `keys`, angle-bracket tokens are expanded. Supported tokens include `Enter`, `Esc`, `Tab`, `Backspace`, `Space`, and control keys like `<C-b>`.
