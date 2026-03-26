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

Inside `keys`, angle-bracket tokens are expanded. Supported tokens include `Enter`, `Esc`, `Tab`, `Backspace`, `Space`, `LT`, `GT`, and control keys like `<C-b>`.

Optional `keys` fields:

- `per_key`: when `true` (default), the harness sends and snapshots one token at a time
- `settle_ms`: override the default quiet window for one step
- `max_wait_ms`: override the maximum wait for output after one step

`LT` is useful for mouse debugging because SGR mouse reports contain a literal `<`. For example, a wheel-up packet can be written as `"<Esc>[<LT>64;40;12M"` with `"per_key": false`.