#include <algorithm>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

constexpr int kDefaultRows = 24;
constexpr int kDefaultCols = 80;
constexpr size_t kMaxPayload = 8192;
constexpr size_t kMaxMessageSize = sizeof(uint32_t) * 2 + sizeof(int32_t) * 2 + kMaxPayload;

volatile sig_atomic_t g_server_signal_fd = -1;
volatile sig_atomic_t g_client_signal_fd = -1;

void perr(const std::string &message) {
    std::perror(message.c_str());
}

bool set_cloexec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool set_nonblock(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool write_all(int fd, const void *buf, size_t len) {
    const char *ptr = static_cast<const char *>(buf);
    size_t written = 0;
    while (written < len) {
        const ssize_t rc = write(fd, ptr + written, len - written);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<size_t>(rc);
    }
    return true;
}

void drain_fd(int fd) {
    char buffer[256];
    while (true) {
        const ssize_t rc = read(fd, buffer, sizeof(buffer));
        if (rc > 0) {
            continue;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

winsize normalize_winsize(winsize ws) {
    if (ws.ws_row == 0 || ws.ws_row > 1000) {
        ws.ws_row = kDefaultRows;
    }
    if (ws.ws_col == 0 || ws.ws_col > 1000) {
        ws.ws_col = kDefaultCols;
    }
    return ws;
}

winsize get_winsize_from_fd(int fd) {
    winsize ws{};
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0) {
        return normalize_winsize(ws);
    }
    ws.ws_row = kDefaultRows;
    ws.ws_col = kDefaultCols;
    return ws;
}

std::string get_instance_name() {
    const char *name = std::getenv("MINI_TMUX_SERVER");
    if (name == nullptr || *name == '\0') {
        return "default";
    }
    return name;
}

std::string sanitize_instance_name(std::string name) {
    for (char &ch : name) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        if (!ok) {
            ch = '_';
        }
    }
    return name;
}

std::string get_socket_path() {
    const uid_t uid = getuid();
    const std::string instance = sanitize_instance_name(get_instance_name());
    return "/tmp/mini-tmux-" + std::to_string(uid) + "-" + instance + ".sock";
}

void ignore_sigpipe() {
    struct sigaction sa {};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGPIPE, &sa, nullptr);
}

void signal_write_byte(int fd) {
    if (fd < 0) {
        return;
    }
    const uint8_t byte = 0;
    const ssize_t rc = write(fd, &byte, sizeof(byte));
    (void)rc;
}

void server_sigchld_handler(int) {
    signal_write_byte(static_cast<int>(g_server_signal_fd));
}

void client_sigwinch_handler(int) {
    signal_write_byte(static_cast<int>(g_client_signal_fd));
}

class TerminalModeGuard {
public:
    TerminalModeGuard() = default;

    bool enable_raw(int fd) {
        fd_ = fd;
        if (!isatty(fd_)) {
            return true;
        }
        if (tcgetattr(fd_, &original_) != 0) {
            return false;
        }
        termios raw = original_;
        cfmakeraw(&raw);
        if (tcsetattr(fd_, TCSANOW, &raw) != 0) {
            return false;
        }
        active_ = true;
        return true;
    }

    void restore() {
        if (active_) {
            tcsetattr(fd_, TCSANOW, &original_);
            active_ = false;
        }
    }

    ~TerminalModeGuard() {
        restore();
    }

    TerminalModeGuard(const TerminalModeGuard &) = delete;
    TerminalModeGuard &operator=(const TerminalModeGuard &) = delete;

private:
    int fd_ = -1;
    bool active_ = false;
    termios original_{};
};

struct MessageHeader {
    uint32_t type;
    uint32_t size;
    int32_t arg0;
    int32_t arg1;
};

static_assert(sizeof(MessageHeader) == 16, "unexpected message header size");

enum class MessageType : uint32_t {
    kClientHello = 1,
    kClientInput = 2,
    kClientResize = 3,
    kClientCommand = 4,
    kServerOutput = 5,
    kServerExit = 6,
    kServerRedraw = 7,
    kServerStatus = 8,
};

struct Message {
    MessageType type{};
    int32_t arg0 = 0;
    int32_t arg1 = 0;
    std::vector<char> payload;
};

bool send_message(int fd, MessageType type, int32_t arg0, int32_t arg1,
                  const char *payload, size_t size) {
    if (size > kMaxPayload) {
        errno = EMSGSIZE;
        return false;
    }

    MessageHeader header{};
    header.type = static_cast<uint32_t>(type);
    header.size = static_cast<uint32_t>(size);
    header.arg0 = arg0;
    header.arg1 = arg1;

    iovec iov[2]{};
    iov[0].iov_base = &header;
    iov[0].iov_len = sizeof(header);
    iov[1].iov_base = const_cast<char *>(payload);
    iov[1].iov_len = size;

    msghdr msg{};
    msg.msg_iov = iov;
    msg.msg_iovlen = size == 0 ? 1 : 2;

    while (true) {
        const ssize_t rc = sendmsg(fd, &msg, MSG_NOSIGNAL);
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        return rc >= 0;
    }
}

bool recv_message(int fd, Message &message) {
    std::vector<char> buffer(kMaxMessageSize);
    while (true) {
        const ssize_t rc = recv(fd, buffer.data(), buffer.size(), 0);
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc <= 0) {
            return false;
        }
        if (static_cast<size_t>(rc) < sizeof(MessageHeader)) {
            errno = EPROTO;
            return false;
        }
        MessageHeader header{};
        std::memcpy(&header, buffer.data(), sizeof(header));
        const size_t payload_size = static_cast<size_t>(header.size);
        if (payload_size > kMaxPayload) {
            errno = EMSGSIZE;
            return false;
        }
        if (sizeof(MessageHeader) + payload_size != static_cast<size_t>(rc)) {
            errno = EPROTO;
            return false;
        }
        message.type = static_cast<MessageType>(header.type);
        message.arg0 = header.arg0;
        message.arg1 = header.arg1;
        message.payload.assign(buffer.begin() + static_cast<std::ptrdiff_t>(sizeof(MessageHeader)),
                               buffer.begin() + static_cast<std::ptrdiff_t>(rc));
        return true;
    }
}

struct ClientConnection {
    int fd = -1;
    bool read_only = false;
};

struct OutputChunk {
    int pane_id = -1;
    std::vector<char> data;
};

struct OutputBacklog {
    std::vector<OutputChunk> chunks;
    size_t total_bytes = 0;

    void trim() {
        constexpr size_t kMaxBacklogBytes = 1 << 20;
        while (total_bytes > kMaxBacklogBytes && !chunks.empty()) {
            total_bytes -= chunks.front().data.size();
            chunks.erase(chunks.begin());
        }
    }

    void append(int pane_id, const char *chunk, size_t size) {
        if (size == 0) {
            return;
        }
        OutputChunk entry{};
        entry.pane_id = pane_id;
        entry.data.assign(chunk, chunk + size);
        total_bytes += size;
        chunks.push_back(std::move(entry));
        trim();
    }

    bool send_to_client(int fd) const {
        for (const OutputChunk &chunk : chunks) {
            size_t offset = 0;
            while (offset < chunk.data.size()) {
                const size_t part = std::min(kMaxPayload, chunk.data.size() - offset);
                if (!send_message(fd, MessageType::kServerOutput, chunk.pane_id, 0,
                                  chunk.data.data() + offset, part)) {
                    return false;
                }
                offset += part;
            }
        }
        return true;
    }
};

struct Pane {
    int master_fd = -1;
    pid_t child_pid = -1;
    bool master_closed = false;
    bool child_reaped = false;
    int exit_code = 0;
    int exit_signal = 0;
    winsize size{};
};

enum class PaneState : uint32_t {
    kCreated = 1,
    kRunning = 2,
    kExited = 3,
    kReaped = 4,
    kDestroyed = 5,
};

struct TextScreenBuffer {
    enum class EscapeState : uint32_t {
        kText = 1,
        kEscape = 2,
        kCsi = 3,
        kOsc = 4,
        kOscEscape = 5,
    };

    std::vector<std::string> lines;
    std::string current_line;
    size_t cursor_col = 0;
    EscapeState escape_state = EscapeState::kText;
};

struct PipeoutState {
    int write_fd = -1;
    pid_t child_pid = -1;
};

struct PaneOutputState {
    int log_fd = -1;
    PipeoutState pipeout{};
    TextScreenBuffer capture{};
};

struct PaneSlot {
    int pane_id = -1;
    Pane pane{};
    PaneState state = PaneState::kCreated;
    PaneOutputState output{};
};

struct PaneLayout {
    int pane_id = -1;
    unsigned short top = 0;
    unsigned short rows = 0;
    unsigned short cols = 0;
    bool focused = false;
};

struct SessionState {
    std::vector<PaneSlot> panes;
    int focused_pane_id = -1;
    int next_pane_id = 1;
    winsize size{};
    OutputBacklog backlog;
};

struct ServerState {
    std::string socket_path;
    int listen_fd = -1;
    int sigchld_read_fd = -1;
    int sigchld_write_fd = -1;
    bool should_exit = false;
    bool exit_notified = false;
    int final_exit_code = 0;
    int final_exit_signal = 0;
    SessionState session{};
    std::vector<ClientConnection> clients;
};

enum class ClientInputMode : uint32_t {
    kNormal = 1,
    kPrefix = 2,
    kCommand = 3,
};

struct ClientPaneBuffer {
    int pane_id = -1;
    TextScreenBuffer text{};
};

struct ClientViewState {
    std::vector<PaneLayout> layout;
    std::vector<ClientPaneBuffer> buffers;
    int focused_pane_id = -1;
    std::string local_status;
};

void close_fd_if_valid(int &fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

bool write_stdout(const std::string &text) {
    return write_all(STDOUT_FILENO, text.data(), text.size());
}

bool enter_alternate_screen() {
    return write_stdout("\x1b[?1049h\x1b[H");
}

bool leave_alternate_screen() {
    return write_stdout("\x1b[?1049l");
}

bool begin_command_prompt() {
    return write_stdout("\r\n:\x1b[K");
}

bool redraw_command_prompt(const std::string &buffer) {
    return write_stdout("\r:" + buffer + "\x1b[K");
}

bool clear_command_prompt(const std::string &) {
    return write_stdout("\r\x1b[K");
}

bool show_local_status(const std::string &text) {
    return write_stdout("\r\n" + text + "\r\n");
}

constexpr size_t kClientBufferedLines = 200;
constexpr size_t kCaptureBufferedLines = 1000;

ClientPaneBuffer *find_client_buffer(ClientViewState &view, int pane_id) {
    for (ClientPaneBuffer &buffer : view.buffers) {
        if (buffer.pane_id == pane_id) {
            return &buffer;
        }
    }
    view.buffers.push_back(ClientPaneBuffer{});
    view.buffers.back().pane_id = pane_id;
    return &view.buffers.back();
}

void trim_text_buffer_lines(TextScreenBuffer &buffer, size_t max_lines) {
    if (buffer.lines.size() > max_lines) {
        buffer.lines.erase(buffer.lines.begin(),
                           buffer.lines.begin() + static_cast<std::ptrdiff_t>(buffer.lines.size() - max_lines));
    }
}

void append_printable_char(TextScreenBuffer &buffer, char ch) {
    if (buffer.cursor_col < buffer.current_line.size()) {
        buffer.current_line[buffer.cursor_col] = ch;
    } else {
        while (buffer.current_line.size() < buffer.cursor_col) {
            buffer.current_line.push_back(' ');
        }
        buffer.current_line.push_back(ch);
    }
    ++buffer.cursor_col;
}

void append_spaces(TextScreenBuffer &buffer, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        append_printable_char(buffer, ' ');
    }
}

void handle_backspace(TextScreenBuffer &buffer) {
    if (buffer.cursor_col == 0 || buffer.current_line.empty()) {
        return;
    }
    const size_t erase_at = buffer.cursor_col - 1;
    if (erase_at < buffer.current_line.size()) {
        buffer.current_line.erase(buffer.current_line.begin() + static_cast<std::ptrdiff_t>(erase_at));
    }
    --buffer.cursor_col;
}

void handle_newline(TextScreenBuffer &buffer, size_t max_lines) {
    buffer.lines.push_back(buffer.current_line);
    buffer.current_line.clear();
    buffer.cursor_col = 0;
    trim_text_buffer_lines(buffer, max_lines);
}

void handle_csi_final(TextScreenBuffer &buffer, char final_byte) {
    if (final_byte == 'K') {
        if (buffer.cursor_col < buffer.current_line.size()) {
            buffer.current_line.erase(static_cast<std::string::size_type>(buffer.cursor_col));
        }
    } else if (final_byte == 'D') {
        if (buffer.cursor_col > 0) {
            --buffer.cursor_col;
        }
    } else if (final_byte == 'C') {
        ++buffer.cursor_col;
    }
}

void append_text_output(TextScreenBuffer &buffer, const std::string &chunk, size_t max_lines) {
    for (unsigned char uch : chunk) {
        const char ch = static_cast<char>(uch);
        switch (buffer.escape_state) {
            case TextScreenBuffer::EscapeState::kText:
                if (ch == '\x1b') {
                    buffer.escape_state = TextScreenBuffer::EscapeState::kEscape;
                    continue;
                }
                if (ch == '\r') {
                    buffer.cursor_col = 0;
                    continue;
                }
                if (ch == '\n') {
                    handle_newline(buffer, max_lines);
                    continue;
                }
                if (ch == '\b' || ch == 127) {
                    handle_backspace(buffer);
                    continue;
                }
                if (ch == '\t') {
                    append_spaces(buffer, 4);
                    continue;
                }
                if (uch < 32) {
                    continue;
                }
                append_printable_char(buffer, ch);
                continue;
            case TextScreenBuffer::EscapeState::kEscape:
                if (ch == '[') {
                    buffer.escape_state = TextScreenBuffer::EscapeState::kCsi;
                } else if (ch == ']') {
                    buffer.escape_state = TextScreenBuffer::EscapeState::kOsc;
                } else {
                    buffer.escape_state = TextScreenBuffer::EscapeState::kText;
                }
                continue;
            case TextScreenBuffer::EscapeState::kCsi:
                if (ch >= 0x40 && ch <= 0x7e) {
                    handle_csi_final(buffer, ch);
                    buffer.escape_state = TextScreenBuffer::EscapeState::kText;
                }
                continue;
            case TextScreenBuffer::EscapeState::kOsc:
                if (ch == '\a') {
                    buffer.escape_state = TextScreenBuffer::EscapeState::kText;
                } else if (ch == '\x1b') {
                    buffer.escape_state = TextScreenBuffer::EscapeState::kOscEscape;
                }
                continue;
            case TextScreenBuffer::EscapeState::kOscEscape:
                buffer.escape_state =
                    ch == '\\' ? TextScreenBuffer::EscapeState::kText : TextScreenBuffer::EscapeState::kOsc;
                continue;
        }
    }
    trim_text_buffer_lines(buffer, max_lines);
}

std::string render_text_snapshot(const TextScreenBuffer &buffer) {
    std::string snapshot;
    for (const std::string &line : buffer.lines) {
        snapshot += line;
        snapshot.push_back('\n');
    }
    snapshot += buffer.current_line;
    return snapshot;
}

void append_client_output(ClientViewState &view, int pane_id, const std::string &chunk) {
    ClientPaneBuffer *buffer = find_client_buffer(view, pane_id);
    append_text_output(buffer->text, chunk, kClientBufferedLines);
}

std::vector<PaneLayout> parse_redraw_layout(const std::string &payload, int focused_pane_id) {
    std::vector<PaneLayout> layout;
    const size_t marker = payload.find(" layout=");
    if (marker == std::string::npos) {
        return layout;
    }
    const std::string body = payload.substr(marker + 8);
    size_t pos = 0;
    while (pos < body.size()) {
        const size_t comma = body.find(',', pos);
        const std::string token = body.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!token.empty()) {
            const size_t open = token.find('[');
            const size_t close = token.find(']');
            const size_t x = token.find('x', open);
            const size_t at = token.find('@', x);
            const size_t star = token.find('*');
            if (open != std::string::npos && close != std::string::npos &&
                x != std::string::npos && at != std::string::npos) {
                PaneLayout pane{};
                const size_t id_end = star != std::string::npos && star < open ? star : open;
                pane.pane_id = std::stoi(token.substr(0, id_end));
                pane.focused = (star != std::string::npos && star < open) || pane.pane_id == focused_pane_id;
                pane.rows = static_cast<unsigned short>(std::stoi(token.substr(open + 1, x - open - 1)));
                pane.cols = static_cast<unsigned short>(std::stoi(token.substr(x + 1, at - x - 1)));
                pane.top = static_cast<unsigned short>(std::stoi(token.substr(at + 1, close - at - 1)));
                layout.push_back(pane);
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    return layout;
}

std::string fit_to_width(const std::string &input, unsigned short width) {
    if (width == 0) {
        return "";
    }
    if (input.size() >= width) {
        return input.substr(0, width);
    }
    return input + std::string(width - input.size(), ' ');
}

bool use_structured_client_render(const ClientViewState &view) {
    return view.layout.size() > 1;
}

bool render_client_view(const ClientViewState &view, const std::string &command_buffer,
                        ClientInputMode input_mode) {
    std::string frame = "\x1b[?25l\x1b[H\x1b[2J";
    int screen_row = 1;
    int cursor_row = 1;
    int cursor_col = 1;
    bool cursor_positioned = false;

    for (size_t i = 0; i < view.layout.size(); ++i) {
        const PaneLayout &pane = view.layout[i];
        const ClientPaneBuffer *buffer = nullptr;
        for (const ClientPaneBuffer &candidate : view.buffers) {
            if (candidate.pane_id == pane.pane_id) {
                buffer = &candidate;
                break;
            }
        }

        std::string header = "Pane " + std::to_string(pane.pane_id);
        if (pane.focused) {
            header += " [active]";
            if (!view.local_status.empty() && input_mode != ClientInputMode::kCommand) {
                header += " | " + view.local_status;
            }
        }
        frame += fit_to_width(header, pane.cols) + "\r\n";
        const int header_row = screen_row;
        ++screen_row;

        std::vector<std::string> visible_lines;
        if (buffer != nullptr) {
            visible_lines = buffer->text.lines;
            visible_lines.push_back(buffer->text.current_line);
        }
        const int reserved_rows = 1 + (i + 1 != view.layout.size() ? 1 : 0);
        const int body_rows = std::max<int>(0, static_cast<int>(pane.rows) - reserved_rows);
        int start = static_cast<int>(visible_lines.size()) - body_rows;
        if (start < 0) {
            start = 0;
        }

        if (pane.focused) {
            cursor_positioned = true;
            if (body_rows > 0) {
                const int current_line_index = std::max<int>(0, static_cast<int>(visible_lines.size()) - 1);
                int cursor_offset = current_line_index - start;
                if (cursor_offset < 0) {
                    cursor_offset = 0;
                }
                if (cursor_offset >= body_rows) {
                    cursor_offset = body_rows - 1;
                }
                cursor_row = header_row + 1 + cursor_offset;
                const size_t raw_col = buffer != nullptr ? buffer->text.cursor_col : 0;
                const unsigned short max_col = pane.cols == 0 ? 1 : pane.cols;
                cursor_col = 1 + static_cast<int>(std::min<size_t>(raw_col, max_col - 1));
            } else {
                cursor_row = header_row;
                cursor_col = 1;
            }
        }

        for (int row = 0; row < body_rows; ++row) {
            std::string line;
            const int source = start + row;
            if (source >= 0 && source < static_cast<int>(visible_lines.size())) {
                line = visible_lines[source];
            }
            frame += fit_to_width(line, pane.cols) + "\r\n";
            ++screen_row;
        }
        if (i + 1 != view.layout.size()) {
            frame += fit_to_width(std::string(pane.cols, '-'), pane.cols) + "\r\n";
            ++screen_row;
        }
    }

    if (input_mode == ClientInputMode::kCommand) {
        frame += "\x1b[999;1H:" + command_buffer;
    } else if (cursor_positioned) {
        frame += "\x1b[" + std::to_string(cursor_row) + ";" + std::to_string(cursor_col) + "H";
    } else {
        frame += "\x1b[H";
    }
    frame += "\x1b[?25h";
    return write_stdout(frame);
}

std::string trim_ascii_whitespace(const std::string &input) {
    size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start])) != 0) {
        ++start;
    }

    size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }
    return input.substr(start, end - start);
}

std::vector<std::string> split_ascii_whitespace(const std::string &input) {
    std::vector<std::string> tokens;
    size_t index = 0;
    while (index < input.size()) {
        while (index < input.size() && std::isspace(static_cast<unsigned char>(input[index])) != 0) {
            ++index;
        }
        if (index >= input.size()) {
            break;
        }
        size_t next = index;
        while (next < input.size() && std::isspace(static_cast<unsigned char>(input[next])) == 0) {
            ++next;
        }
        tokens.push_back(input.substr(index, next - index));
        index = next;
    }
    return tokens;
}

bool parse_non_negative_int(const std::string &text, int &value) {
    if (text.empty()) {
        return false;
    }

    char *end = nullptr;
    errno = 0;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || parsed < 0 || parsed > INT32_MAX) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

std::string format_usage(const std::string &usage) {
    return "usage: " + usage;
}

std::string format_error(const std::string &message) {
    return "error: " + message;
}

std::string format_ok(const std::string &message) {
    return "ok: " + message;
}

bool split_command_name_and_rest(const std::string &input, std::string &name, std::string &rest) {
    size_t pos = 0;
    while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos])) != 0) {
        ++pos;
    }
    if (pos >= input.size()) {
        return false;
    }

    const size_t name_start = pos;
    while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos])) == 0) {
        ++pos;
    }
    name = input.substr(name_start, pos - name_start);
    rest = trim_ascii_whitespace(input.substr(pos));
    return true;
}

bool parse_single_non_negative_int_argument(const std::string &input, int &value) {
    const std::vector<std::string> tokens = split_ascii_whitespace(input);
    return tokens.size() == 1 && parse_non_negative_int(tokens[0], value);
}

bool parse_pane_and_tail(const std::string &input, int &pane_id, std::string &tail) {
    size_t pos = 0;
    while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos])) != 0) {
        ++pos;
    }
    if (pos >= input.size()) {
        return false;
    }

    const size_t pane_start = pos;
    while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos])) == 0) {
        ++pos;
    }
    if (!parse_non_negative_int(input.substr(pane_start, pos - pane_start), pane_id)) {
        return false;
    }
    tail = trim_ascii_whitespace(input.substr(pos));
    return !tail.empty();
}

int open_output_file(const std::string &path, int flags) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | flags, 0644);
    if (fd < 0) {
        return -1;
    }
    set_cloexec(fd);
    return fd;
}

void stop_pane_log(PaneSlot &slot) {
    close_fd_if_valid(slot.output.log_fd);
}

bool wait_for_child_exit(pid_t pid) {
    if (pid <= 0) {
        return true;
    }
    while (true) {
        const pid_t rc = waitpid(pid, nullptr, 0);
        if (rc == pid) {
            return true;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        return rc < 0 && errno == ECHILD;
    }
}

void stop_pane_pipeout(PaneSlot &slot, bool terminate_process, bool wait_for_child) {
    close_fd_if_valid(slot.output.pipeout.write_fd);
    const pid_t pid = slot.output.pipeout.child_pid;
    if (pid > 0 && terminate_process) {
        if (kill(pid, SIGHUP) != 0 && errno != ESRCH) {
            // Best-effort cleanup; if signaling fails for another reason the next wait/reap path still clears it.
        }
    }
    if (pid > 0 && wait_for_child) {
        wait_for_child_exit(pid);
    }
    slot.output.pipeout.child_pid = -1;
}

void cleanup_pane_outputs(PaneSlot &slot, bool wait_for_pipeout_child) {
    stop_pane_log(slot);
    stop_pane_pipeout(slot, true, wait_for_pipeout_child);
}

bool start_pane_log(PaneSlot &slot, const std::string &path) {
    const int fd = open_output_file(path, O_APPEND);
    if (fd < 0) {
        return false;
    }
    stop_pane_log(slot);
    slot.output.log_fd = fd;
    return true;
}

void redirect_stdio_to_devnull();

bool start_pane_pipeout(PaneSlot &slot, const std::string &command) {
    stop_pane_pipeout(slot, false, true);

    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
        return false;
    }
    set_cloexec(pipe_fds[0]);
    set_cloexec(pipe_fds[1]);

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }
    if (pid == 0) {
        close(pipe_fds[1]);
        redirect_stdio_to_devnull();
        if (dup2(pipe_fds[0], STDIN_FILENO) < 0) {
            _exit(1);
        }
        if (pipe_fds[0] > STDERR_FILENO) {
            close(pipe_fds[0]);
        }
        execl("/bin/sh", "/bin/sh", "-c", command.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }

    close(pipe_fds[0]);
    slot.output.pipeout.write_fd = pipe_fds[1];
    slot.output.pipeout.child_pid = pid;
    return true;
}

bool write_capture_snapshot(const PaneSlot &slot, const std::string &path) {
    const int fd = open_output_file(path, O_TRUNC);
    if (fd < 0) {
        return false;
    }
    const std::string snapshot = render_text_snapshot(slot.output.capture);
    const bool ok = write_all(fd, snapshot.data(), snapshot.size());
    close(fd);
    return ok;
}

void record_pane_output(PaneSlot &slot, const char *data, size_t size) {
    if (size == 0) {
        return;
    }

    append_text_output(slot.output.capture, std::string(data, size), kCaptureBufferedLines);
    if (slot.output.log_fd >= 0 && !write_all(slot.output.log_fd, data, size)) {
        close_fd_if_valid(slot.output.log_fd);
    }
    if (slot.output.pipeout.write_fd >= 0 && !write_all(slot.output.pipeout.write_fd, data, size)) {
        close_fd_if_valid(slot.output.pipeout.write_fd);
    }
}

PaneSlot *find_pipeout_slot_by_pid(SessionState &session, pid_t pid) {
    if (pid <= 0) {
        return nullptr;
    }
    for (PaneSlot &slot : session.panes) {
        if (slot.output.pipeout.child_pid == pid) {
            return &slot;
        }
    }
    return nullptr;
}

void redirect_stdio_to_devnull();
bool apply_winsize(int master_fd, const winsize &ws);

std::vector<size_t> collect_live_pane_indices(const SessionState &session) {
    std::vector<size_t> indices;
    for (size_t i = 0; i < session.panes.size(); ++i) {
        if (session.panes[i].state != PaneState::kDestroyed) {
            indices.push_back(i);
        }
    }
    return indices;
}

std::vector<PaneLayout> compute_vertical_layout(const SessionState &session) {
    const std::vector<size_t> live_indices = collect_live_pane_indices(session);
    std::vector<PaneLayout> layout;
    if (live_indices.empty()) {
        return layout;
    }

    const unsigned short total_rows = session.size.ws_row == 0 ? kDefaultRows : session.size.ws_row;
    const unsigned short total_cols = session.size.ws_col == 0 ? kDefaultCols : session.size.ws_col;
    const size_t pane_count = live_indices.size();
    const unsigned short base_rows =
        static_cast<unsigned short>(std::max<size_t>(1, total_rows / pane_count));
    const unsigned short extra_rows = static_cast<unsigned short>(total_rows % pane_count);
    unsigned short top = 0;

    for (size_t order = 0; order < live_indices.size(); ++order) {
        const PaneSlot &slot = session.panes[live_indices[order]];
        unsigned short rows = base_rows;
        if (order < extra_rows) {
            rows = static_cast<unsigned short>(rows + 1);
        }
        if (order + 1 == live_indices.size()) {
            rows = static_cast<unsigned short>(std::max<int>(1, total_rows - top));
        }
        layout.push_back(PaneLayout{
            slot.pane_id,
            top,
            rows,
            total_cols,
            slot.pane_id == session.focused_pane_id,
        });
        top = static_cast<unsigned short>(top + rows);
    }
    return layout;
}

void apply_session_layout(SessionState &session) {
    const std::vector<PaneLayout> layout = compute_vertical_layout(session);
    for (const PaneLayout &entry : layout) {
        for (PaneSlot &slot : session.panes) {
            if (slot.pane_id != entry.pane_id || slot.state == PaneState::kDestroyed) {
                continue;
            }
            slot.pane.size.ws_row = entry.rows;
            slot.pane.size.ws_col = entry.cols;
            if (slot.pane.master_fd >= 0) {
                apply_winsize(slot.pane.master_fd, slot.pane.size);
            }
            break;
        }
    }
}

std::string build_redraw_summary(const SessionState &session) {
    std::string summary = "redraw: focus=" + std::to_string(session.focused_pane_id) + " layout=";
    const std::vector<PaneLayout> layout = compute_vertical_layout(session);
    bool first = true;
    for (const PaneLayout &entry : layout) {
        if (!first) {
            summary += ",";
        }
        first = false;
        summary += std::to_string(entry.pane_id);
        if (entry.focused) {
            summary += "*";
        }
        summary += "[" + std::to_string(entry.rows) + "x" + std::to_string(entry.cols) + "@" +
                   std::to_string(entry.top) + "]";
        for (const PaneSlot &slot : session.panes) {
            if (slot.pane_id == entry.pane_id &&
                (slot.state == PaneState::kExited || slot.state == PaneState::kReaped)) {
                summary += "(exiting)";
                break;
            }
        }
    }
    if (first) {
        summary += "none";
    }
    return summary;
}

PaneSlot *find_pane_slot(SessionState &session, int pane_id) {
    for (PaneSlot &slot : session.panes) {
        if (slot.pane_id == pane_id && slot.state != PaneState::kDestroyed) {
            return &slot;
        }
    }
    return nullptr;
}

int choose_focus_pane_id(const SessionState &session, int skip_pane_id) {
    for (const PaneSlot &slot : session.panes) {
        if (slot.state != PaneState::kDestroyed && slot.pane_id != skip_pane_id) {
            return slot.pane_id;
        }
    }
    return -1;
}

std::vector<int> collect_live_pane_ids(const SessionState &session) {
    std::vector<int> pane_ids;
    for (const PaneSlot &slot : session.panes) {
        if (slot.state != PaneState::kDestroyed) {
            pane_ids.push_back(slot.pane_id);
        }
    }
    return pane_ids;
}

int choose_adjacent_focus_pane_id(const SessionState &session, int direction) {
    const std::vector<int> pane_ids = collect_live_pane_ids(session);
    if (pane_ids.empty()) {
        return -1;
    }
    if (direction == 0) {
        return pane_ids.front();
    }

    size_t focused_index = 0;
    bool found = false;
    for (size_t i = 0; i < pane_ids.size(); ++i) {
        if (pane_ids[i] == session.focused_pane_id) {
            focused_index = i;
            found = true;
            break;
        }
    }
    if (!found) {
        return pane_ids.front();
    }

    const size_t pane_count = pane_ids.size();
    if (direction > 0) {
        return pane_ids[(focused_index + 1) % pane_count];
    }
    return pane_ids[(focused_index + pane_count - 1) % pane_count];
}

std::string focus_pane(SessionState &session, int pane_id) {
    if (find_pane_slot(session, pane_id) == nullptr) {
        return format_error("pane " + std::to_string(pane_id) + " does not exist");
    }
    if (session.focused_pane_id == pane_id) {
        return format_ok("pane " + std::to_string(pane_id) + " already selected");
    }
    session.focused_pane_id = pane_id;
    return format_ok("focused pane " + std::to_string(pane_id));
}

void maybe_repair_focus(SessionState &session) {
    if (session.focused_pane_id >= 0 && find_pane_slot(session, session.focused_pane_id) != nullptr) {
        return;
    }
    session.focused_pane_id = choose_focus_pane_id(session, -1);
}

bool session_has_live_panes(const SessionState &session) {
    for (const PaneSlot &slot : session.panes) {
        if (slot.state != PaneState::kDestroyed) {
            return true;
        }
    }
    return false;
}

bool send_client_hello(int socket_fd, bool read_only) {
    return send_message(socket_fd, MessageType::kClientHello, read_only ? 1 : 0, 0, nullptr, 0);
}

bool recv_client_hello(int socket_fd, bool &read_only) {
    Message message;
    if (!recv_message(socket_fd, message)) {
        return false;
    }
    if (message.type != MessageType::kClientHello) {
        errno = EPROTO;
        return false;
    }
    read_only = message.arg0 != 0;
    return true;
}

bool apply_winsize(int master_fd, const winsize &ws) {
    return ioctl(master_fd, TIOCSWINSZ, &ws) == 0;
}

std::string choose_shell() {
    const char *shell = std::getenv("SHELL");
    if (shell != nullptr && *shell != '\0') {
        return shell;
    }
    return "/bin/bash";
}

bool spawn_shell_pane(Pane &pane, const winsize &initial_size) {
    int master_fd = -1;
    int slave_fd = -1;
    if (openpty(&master_fd, &slave_fd, nullptr, nullptr, const_cast<winsize *>(&initial_size)) != 0) {
        return false;
    }

    set_cloexec(master_fd);
    set_cloexec(slave_fd);

    const pid_t pid = fork();
    if (pid < 0) {
        close(master_fd);
        close(slave_fd);
        return false;
    }

    if (pid == 0) {
        close(master_fd);

        if (setsid() < 0) {
            _exit(1);
        }
        if (ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            _exit(1);
        }
        if (dup2(slave_fd, STDIN_FILENO) < 0 ||
            dup2(slave_fd, STDOUT_FILENO) < 0 ||
            dup2(slave_fd, STDERR_FILENO) < 0) {
            _exit(1);
        }
        if (slave_fd > STDERR_FILENO) {
            close(slave_fd);
        }

        const std::string shell = choose_shell();
        if (std::getenv("TERM") == nullptr) {
            setenv("TERM", "xterm-256color", 1);
        }
        execl(shell.c_str(), shell.c_str(), static_cast<char *>(nullptr));
        execl("/bin/sh", "/bin/sh", static_cast<char *>(nullptr));
        _exit(127);
    }

    close(slave_fd);
    pane.master_fd = master_fd;
    pane.child_pid = pid;
    pane.master_closed = false;
    pane.child_reaped = false;
    pane.exit_code = 0;
    pane.exit_signal = 0;
    pane.size = initial_size;
    return true;
}

bool spawn_pane_slot(SessionState &session, int pane_id, const winsize &size) {
    PaneSlot slot{};
    slot.pane_id = pane_id;
    slot.state = PaneState::kCreated;
    if (!spawn_shell_pane(slot.pane, size)) {
        return false;
    }
    slot.state = PaneState::kRunning;
    session.panes.push_back(std::move(slot));
    return true;
}

std::string handle_client_command(ServerState &server, const ClientConnection &client, const std::string &raw_command) {
    std::string command = trim_ascii_whitespace(raw_command);
    if (!command.empty() && command.front() == ':') {
        command.erase(command.begin());
        command = trim_ascii_whitespace(command);
    }
    if (command.empty()) {
        return format_error("empty command");
    }

    std::string name;
    std::string rest;
    if (!split_command_name_and_rest(command, name, rest)) {
        return format_error("empty command");
    }

    if (name == "new") {
        if (!rest.empty()) {
            return format_usage(":new");
        }
        if (client.read_only) {
            return format_error("read-only clients cannot run :new");
        }

        const int pane_id = server.session.next_pane_id++;
        if (!spawn_pane_slot(server.session, pane_id, server.session.size)) {
            return format_error("failed to create pane");
        }
        server.session.focused_pane_id = pane_id;
        apply_session_layout(server.session);
        return format_ok("created pane " + std::to_string(pane_id));
    }

    if (name == "kill") {
        if (client.read_only) {
            return format_error("read-only clients cannot run :kill");
        }

        int pane_id = -1;
        if (!parse_single_non_negative_int_argument(rest, pane_id)) {
            return format_usage(":kill <pane_id>");
        }

        PaneSlot *slot = find_pane_slot(server.session, pane_id);
        if (slot == nullptr) {
            return format_error("pane " + std::to_string(pane_id) + " does not exist");
        }
        if (slot->pane.child_reaped) {
            return format_ok("pane " + std::to_string(pane_id) + " is already exiting");
        }

        cleanup_pane_outputs(*slot, true);
        if (slot->pane.child_pid > 0 && kill(slot->pane.child_pid, SIGHUP) != 0 && errno != ESRCH) {
            return format_error("failed to signal pane " + std::to_string(pane_id));
        }
        if (server.session.focused_pane_id == pane_id) {
            server.session.focused_pane_id = choose_focus_pane_id(server.session, pane_id);
        }
        apply_session_layout(server.session);
        return format_ok("terminating pane " + std::to_string(pane_id));
    }

    if (name == "focus") {
        if (client.read_only) {
            return format_error("read-only clients cannot run :focus");
        }

        int pane_id = -1;
        if (!parse_single_non_negative_int_argument(rest, pane_id)) {
            return format_usage(":focus <pane_id>");
        }
        return focus_pane(server.session, pane_id);
    }

    if (name == "next" || name == "prev") {
        if (!rest.empty()) {
            return format_usage(name == "next" ? ":next" : ":prev");
        }
        if (client.read_only) {
            return format_error("read-only clients cannot run :" + name);
        }

        const int pane_id = choose_adjacent_focus_pane_id(server.session, name == "next" ? 1 : -1);
        if (pane_id < 0) {
            return format_error("no panes available");
        }
        return focus_pane(server.session, pane_id);
    }

    if (name == "log") {
        if (client.read_only) {
            return format_error("read-only clients cannot run :log");
        }

        int pane_id = -1;
        std::string path_arg;
        if (!parse_pane_and_tail(rest, pane_id, path_arg)) {
            return format_usage(":log <pane_id> <file_path>");
        }

        PaneSlot *slot = find_pane_slot(server.session, pane_id);
        if (slot == nullptr) {
            return format_error("pane " + std::to_string(pane_id) + " does not exist");
        }
        if (!start_pane_log(*slot, path_arg)) {
            return format_error("failed to open log target");
        }
        return format_ok("logging pane " + std::to_string(pane_id));
    }

    if (name == "log-stop") {
        if (client.read_only) {
            return format_error("read-only clients cannot run :log-stop");
        }

        int pane_id = -1;
        if (!parse_single_non_negative_int_argument(rest, pane_id)) {
            return format_usage(":log-stop <pane_id>");
        }

        PaneSlot *slot = find_pane_slot(server.session, pane_id);
        if (slot == nullptr) {
            return format_error("pane " + std::to_string(pane_id) + " does not exist");
        }
        stop_pane_log(*slot);
        return format_ok("stopped log for pane " + std::to_string(pane_id));
    }

    if (name == "pipeout") {
        if (client.read_only) {
            return format_error("read-only clients cannot run :pipeout");
        }

        int pane_id = -1;
        std::string command_arg;
        if (!parse_pane_and_tail(rest, pane_id, command_arg)) {
            return format_usage(":pipeout <pane_id> <cmd>");
        }

        PaneSlot *slot = find_pane_slot(server.session, pane_id);
        if (slot == nullptr) {
            return format_error("pane " + std::to_string(pane_id) + " does not exist");
        }
        if (!start_pane_pipeout(*slot, command_arg)) {
            return format_error("failed to start pipeout");
        }
        return format_ok("pipeout started for pane " + std::to_string(pane_id));
    }

    if (name == "pipeout-stop") {
        if (client.read_only) {
            return format_error("read-only clients cannot run :pipeout-stop");
        }

        int pane_id = -1;
        if (!parse_single_non_negative_int_argument(rest, pane_id)) {
            return format_usage(":pipeout-stop <pane_id>");
        }

        PaneSlot *slot = find_pane_slot(server.session, pane_id);
        if (slot == nullptr) {
            return format_error("pane " + std::to_string(pane_id) + " does not exist");
        }
        stop_pane_pipeout(*slot, false, true);
        return format_ok("stopped pipeout for pane " + std::to_string(pane_id));
    }

    if (name == "capture") {
        if (client.read_only) {
            return format_error("read-only clients cannot run :capture");
        }

        int pane_id = -1;
        std::string path_arg;
        if (!parse_pane_and_tail(rest, pane_id, path_arg)) {
            return format_usage(":capture <pane_id> <file_path>");
        }

        PaneSlot *slot = find_pane_slot(server.session, pane_id);
        if (slot == nullptr) {
            return format_error("pane " + std::to_string(pane_id) + " does not exist");
        }
        if (!write_capture_snapshot(*slot, path_arg)) {
            return format_error("failed to write capture file");
        }
        return format_ok("captured pane " + std::to_string(pane_id));
    }

    return format_error("unknown command '" + name + "'");
}

void send_resize_to_server(int socket_fd) {
    const winsize ws = get_winsize_from_fd(STDIN_FILENO);
    send_message(socket_fd, MessageType::kClientResize, ws.ws_row, ws.ws_col, nullptr, 0);
}

int connect_to_server(const std::string &socket_path) {
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        return -1;
    }
    set_cloexec(fd);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path.c_str());

    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int create_listen_socket(const std::string &socket_path) {
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        return -1;
    }
    set_cloexec(fd);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path.c_str());

    unlink(socket_path.c_str());
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 16) != 0) {
        close(fd);
        unlink(socket_path.c_str());
        return -1;
    }
    return fd;
}

void remove_client(std::vector<ClientConnection> &clients, size_t index) {
    close_fd_if_valid(clients[index].fd);
    clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(index));
}

void broadcast_output(ServerState &server, int pane_id, const char *data, size_t size) {
    if (PaneSlot *slot = find_pane_slot(server.session, pane_id); slot != nullptr) {
        record_pane_output(*slot, data, size);
    }
    server.session.backlog.append(pane_id, data, size);
    for (size_t i = 0; i < server.clients.size();) {
        if (!send_message(server.clients[i].fd, MessageType::kServerOutput, pane_id, 0, data, size)) {
            remove_client(server.clients, i);
            continue;
        }
        ++i;
    }
}

void broadcast_redraw(ServerState &server) {
    const std::string summary = build_redraw_summary(server.session);
    for (size_t i = 0; i < server.clients.size();) {
        if (!send_message(server.clients[i].fd, MessageType::kServerRedraw,
                          server.session.focused_pane_id,
                          static_cast<int32_t>(server.session.panes.size()),
                          summary.data(), summary.size())) {
            remove_client(server.clients, i);
            continue;
        }
        ++i;
    }
}

void notify_server_exit(ServerState &server) {
    for (size_t i = 0; i < server.clients.size();) {
        if (!send_message(server.clients[i].fd, MessageType::kServerExit,
                          server.final_exit_code, server.final_exit_signal, nullptr, 0)) {
            remove_client(server.clients, i);
            continue;
        }
        ++i;
    }
}

void redirect_stdio_to_devnull() {
    const int devnull = open("/dev/null", O_RDWR);
    if (devnull < 0) {
        return;
    }
    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO) {
        close(devnull);
    }
}

int server_process(const std::string &socket_path, const winsize &initial_size) {
    ignore_sigpipe();

    ServerState server{};
    server.socket_path = socket_path;
    server.session.size = initial_size;

    int sig_pipe[2] = {-1, -1};
    if (pipe(sig_pipe) != 0) {
        perr("pipe(SIGCHLD)");
        return 1;
    }
    server.sigchld_read_fd = sig_pipe[0];
    server.sigchld_write_fd = sig_pipe[1];
    set_cloexec(server.sigchld_read_fd);
    set_cloexec(server.sigchld_write_fd);
    set_nonblock(server.sigchld_read_fd);
    set_nonblock(server.sigchld_write_fd);
    g_server_signal_fd = server.sigchld_write_fd;

    struct sigaction sa {};
    sa.sa_handler = server_sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, nullptr) != 0) {
        perr("sigaction(SIGCHLD)");
        return 1;
    }

    if (!spawn_pane_slot(server.session, 0, initial_size)) {
        perr("spawn pane");
        return 1;
    }
    server.session.focused_pane_id = 0;
    apply_session_layout(server.session);

    server.listen_fd = create_listen_socket(socket_path);
    if (server.listen_fd < 0) {
        perr("listen socket");
        for (PaneSlot &slot : server.session.panes) {
            cleanup_pane_outputs(slot, true);
            if (slot.pane.child_pid > 0) {
                kill(slot.pane.child_pid, SIGHUP);
                waitpid(slot.pane.child_pid, nullptr, 0);
            }
            close_fd_if_valid(slot.pane.master_fd);
            slot.state = PaneState::kDestroyed;
        }
        return 1;
    }

    while (!server.should_exit) {
        const std::string redraw_before_poll = build_redraw_summary(server.session);
        std::vector<pollfd> pfds;
        std::vector<size_t> pane_poll_indices;
        pfds.push_back({server.listen_fd, POLLIN, 0});
        pfds.push_back({server.sigchld_read_fd, POLLIN, 0});
        for (size_t pane_index = 0; pane_index < server.session.panes.size(); ++pane_index) {
            const PaneSlot &slot = server.session.panes[pane_index];
            if (slot.pane.master_fd >= 0) {
                pfds.push_back({slot.pane.master_fd, static_cast<short>(POLLIN | POLLHUP | POLLERR), 0});
                pane_poll_indices.push_back(pane_index);
            }
        }
        for (const ClientConnection &client : server.clients) {
            pfds.push_back({client.fd, static_cast<short>(POLLIN | POLLHUP | POLLERR), 0});
        }

        const int rc = poll(pfds.data(), pfds.size(), -1);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            perr("poll(server)");
            break;
        }

        size_t idx = 0;
        if (pfds[idx].revents & POLLIN) {
            const int client_fd = accept(server.listen_fd, nullptr, nullptr);
            if (client_fd >= 0) {
                set_cloexec(client_fd);
                bool read_only = false;
                if (!recv_client_hello(client_fd, read_only)) {
                    close(client_fd);
                } else if (server.session.backlog.send_to_client(client_fd)) {
                    server.clients.push_back({client_fd, read_only});
                    broadcast_redraw(server);
                } else {
                    close(client_fd);
                }
            }
        }
        ++idx;

        if (pfds[idx].revents & POLLIN) {
            drain_fd(server.sigchld_read_fd);
            while (true) {
                int status = 0;
                const pid_t pid = waitpid(-1, &status, WNOHANG);
                if (pid <= 0) {
                    break;
                }
                if (PaneSlot *pipe_slot = find_pipeout_slot_by_pid(server.session, pid); pipe_slot != nullptr) {
                    close_fd_if_valid(pipe_slot->output.pipeout.write_fd);
                    pipe_slot->output.pipeout.child_pid = -1;
                    continue;
                }
                for (PaneSlot &slot : server.session.panes) {
                    if (pid != slot.pane.child_pid) {
                        continue;
                    }
                    slot.pane.child_reaped = true;
                    slot.state = PaneState::kReaped;
                    cleanup_pane_outputs(slot, true);
                    if (WIFEXITED(status)) {
                        slot.pane.exit_code = WEXITSTATUS(status);
                        slot.pane.exit_signal = 0;
                    } else if (WIFSIGNALED(status)) {
                        slot.pane.exit_code = 128 + WTERMSIG(status);
                        slot.pane.exit_signal = WTERMSIG(status);
                    }
                    server.final_exit_code = slot.pane.exit_code;
                    server.final_exit_signal = slot.pane.exit_signal;
                    break;
                }
            }
        }
        ++idx;

        for (size_t pane_poll_cursor = 0; pane_poll_cursor < pane_poll_indices.size(); ++pane_poll_cursor, ++idx) {
            PaneSlot &slot = server.session.panes[pane_poll_indices[pane_poll_cursor]];
            if (pfds[idx].revents & (POLLIN | POLLHUP | POLLERR)) {
                char buffer[4096];
                const ssize_t read_rc = read(slot.pane.master_fd, buffer, sizeof(buffer));
                if (read_rc > 0) {
                    broadcast_output(server, slot.pane_id, buffer, static_cast<size_t>(read_rc));
                } else if (read_rc == 0 || (read_rc < 0 && errno != EINTR && errno != EAGAIN)) {
                    close_fd_if_valid(slot.pane.master_fd);
                    slot.pane.master_closed = true;
                    if (slot.state == PaneState::kRunning) {
                        slot.state = PaneState::kExited;
                    }
                }
            }
        }

        for (size_t client_index = 0; client_index < server.clients.size(); ++idx) {
            const short revents = pfds[idx].revents;
            bool keep_client = true;
            if (revents & (POLLHUP | POLLERR)) {
                keep_client = false;
            } else if (revents & POLLIN) {
                Message message;
                if (!recv_message(server.clients[client_index].fd, message)) {
                    keep_client = false;
                } else if (message.type == MessageType::kClientInput) {
                    PaneSlot *target = find_pane_slot(server.session, server.session.focused_pane_id);
                    if (!server.clients[client_index].read_only && target != nullptr &&
                        target->pane.master_fd >= 0 && !message.payload.empty()) {
                        if (!write_all(target->pane.master_fd, message.payload.data(), message.payload.size())) {
                            close_fd_if_valid(target->pane.master_fd);
                            target->pane.master_closed = true;
                            if (target->state == PaneState::kRunning) {
                                target->state = PaneState::kExited;
                            }
                        }
                    }
                } else if (message.type == MessageType::kClientResize) {
                    winsize ws{};
                    ws.ws_row = static_cast<unsigned short>(message.arg0);
                    ws.ws_col = static_cast<unsigned short>(message.arg1);
                    ws = normalize_winsize(ws);
                    server.session.size = ws;
                    apply_session_layout(server.session);
                } else if (message.type == MessageType::kClientCommand) {
                    const std::string raw_command(message.payload.begin(), message.payload.end());
                    const std::string status = handle_client_command(server, server.clients[client_index], raw_command);
                    if (!send_message(server.clients[client_index].fd, MessageType::kServerStatus, 0, 0,
                                      status.data(), status.size())) {
                        keep_client = false;
                    } else if (status.rfind("ok: created pane ", 0) == 0 ||
                               status.rfind("ok: terminating pane ", 0) == 0 ||
                               status.rfind("ok: focused pane ", 0) == 0 ||
                               status.find("already selected") != std::string::npos) {
                        broadcast_redraw(server);
                    }
                }
            }

            if (!keep_client) {
                remove_client(server.clients, client_index);
                continue;
            }
            ++client_index;
        }

        for (PaneSlot &slot : server.session.panes) {
            if (slot.pane.child_reaped && (slot.pane.master_fd < 0 || slot.pane.master_closed) &&
                slot.state != PaneState::kDestroyed) {
                slot.state = PaneState::kDestroyed;
            }
        }
        maybe_repair_focus(server.session);
        apply_session_layout(server.session);
        if (build_redraw_summary(server.session) != redraw_before_poll) {
            broadcast_redraw(server);
        }

        if (!session_has_live_panes(server.session)) {
            if (!server.exit_notified) {
                notify_server_exit(server);
                server.exit_notified = true;
            }
            server.should_exit = true;
        }
    }

    for (ClientConnection &client : server.clients) {
        close_fd_if_valid(client.fd);
    }
    for (PaneSlot &slot : server.session.panes) {
        cleanup_pane_outputs(slot, true);
        if (slot.pane.child_pid > 0 && !slot.pane.child_reaped) {
            kill(slot.pane.child_pid, SIGHUP);
            waitpid(slot.pane.child_pid, nullptr, 0);
        }
        close_fd_if_valid(slot.pane.master_fd);
        slot.state = PaneState::kDestroyed;
    }
    close_fd_if_valid(server.sigchld_read_fd);
    close_fd_if_valid(server.sigchld_write_fd);
    close_fd_if_valid(server.listen_fd);
    unlink(socket_path.c_str());
    return 0;
}

int client_process(int socket_fd, bool read_only) {
    ignore_sigpipe();

    if (!send_client_hello(socket_fd, read_only)) {
        perr("send(client hello)");
        return 1;
    }

    int sig_pipe[2] = {-1, -1};
    if (pipe(sig_pipe) != 0) {
        perr("pipe(SIGWINCH)");
        return 1;
    }
    set_cloexec(sig_pipe[0]);
    set_cloexec(sig_pipe[1]);
    set_nonblock(sig_pipe[0]);
    set_nonblock(sig_pipe[1]);
    g_client_signal_fd = sig_pipe[1];

    struct sigaction sa {};
    sa.sa_handler = client_sigwinch_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGWINCH, &sa, nullptr) != 0) {
        perr("sigaction(SIGWINCH)");
        return 1;
    }

    TerminalModeGuard terminal_guard;
    if (!terminal_guard.enable_raw(STDIN_FILENO)) {
        perr("tcsetattr(raw)");
        return 1;
    }

    if (!show_local_status(read_only ?
                           "mini-tmux: attached in read-only mode" :
                           "mini-tmux: attached")) {
        terminal_guard.restore();
        close(sig_pipe[0]);
        close(sig_pipe[1]);
        return 1;
    }

    send_resize_to_server(socket_fd);

    constexpr char kPrefixKey = 0x02;
    constexpr char kEscapeKey = 0x1b;
    constexpr char kBackspaceKey = 0x7f;
    ClientInputMode input_mode = ClientInputMode::kNormal;
    std::string command_buffer;
    ClientViewState view_state{};
    bool alternate_screen_active = false;

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

    int exit_code = 0;
    bool done = false;
    while (!done) {
        pollfd pfds[3]{};
        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = POLLIN;
        pfds[1].fd = socket_fd;
        pfds[1].events = POLLIN | POLLHUP | POLLERR;
        pfds[2].fd = sig_pipe[0];
        pfds[2].events = POLLIN;

        const int rc = poll(pfds, 3, -1);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            perr("poll(client)");
            break;
        }

        if (pfds[2].revents & POLLIN) {
            drain_fd(sig_pipe[0]);
            send_resize_to_server(socket_fd);
            if (use_structured_client_render(view_state)) {
                if (!ensure_render_surface() ||
                    !render_client_view(view_state, command_buffer, input_mode)) {
                    done = true;
                }
            }
        }

        if (pfds[0].revents & POLLIN) {
            char buffer[4096];
            const ssize_t read_rc = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (read_rc > 0) {
                for (ssize_t i = 0; i < read_rc && !done; ++i) {
                    const char ch = buffer[i];
                    if (input_mode == ClientInputMode::kCommand) {
                        if (ch == '\r' || ch == '\n') {
                            if (!clear_command_prompt(command_buffer)) {
                                done = true;
                                break;
                            }
                            if (!send_message(socket_fd, MessageType::kClientCommand, 0, 0,
                                              command_buffer.data(), command_buffer.size())) {
                                done = true;
                                break;
                            }
                            command_buffer.clear();
                            input_mode = ClientInputMode::kNormal;
                        } else if (ch == kEscapeKey || ch == 0x03) {
                            if (!clear_command_prompt(command_buffer)) {
                                done = true;
                                break;
                            }
                            command_buffer.clear();
                            input_mode = ClientInputMode::kNormal;
                        } else if (ch == kBackspaceKey || ch == '\b') {
                            if (!command_buffer.empty()) {
                                command_buffer.pop_back();
                                if (use_structured_client_render(view_state)) {
                                    if (!ensure_render_surface() ||
                                        !render_client_view(view_state, command_buffer, input_mode)) {
                                        done = true;
                                        break;
                                    }
                                } else if (!begin_command_prompt()) {
                                    done = true;
                                    break;
                                }
                            }
                        } else if (ch >= 32 && ch <= 126) {
                            command_buffer.push_back(ch);
                            if (use_structured_client_render(view_state)) {
                                if (!ensure_render_surface() ||
                                    !render_client_view(view_state, command_buffer, input_mode)) {
                                    done = true;
                                    break;
                                }
                            } else if (!redraw_command_prompt(command_buffer)) {
                                done = true;
                                break;
                            }
                        }
                        continue;
                    }

                    if (input_mode == ClientInputMode::kPrefix) {
                        if (ch == ':') {
                            input_mode = ClientInputMode::kCommand;
                            command_buffer.clear();
                            if (use_structured_client_render(view_state)) {
                                if (!render_client_view(view_state, command_buffer, input_mode)) {
                                    done = true;
                                    break;
                                }
                            } else if (!begin_command_prompt()) {
                                done = true;
                                break;
                            }
                        } else if (ch == 'd') {
                            input_mode = ClientInputMode::kNormal;
                            done = true;
                        } else if (ch == 'n' || ch == 'p') {
                            const std::string command = ch == 'n' ? "next" : "prev";
                            if (!read_only) {
                                if (!send_message(socket_fd, MessageType::kClientCommand, 0, 0,
                                                  command.data(), command.size())) {
                                    done = true;
                                    break;
                                }
                            } else if (!show_local_status("read-only clients cannot switch focus")) {
                                done = true;
                                break;
                            }
                            input_mode = ClientInputMode::kNormal;
                        } else if (!read_only && ch == kPrefixKey) {
                            if (!send_message(socket_fd, MessageType::kClientInput, 0, 0, &ch, 1)) {
                                done = true;
                                break;
                            }
                            input_mode = ClientInputMode::kNormal;
                        } else {
                            input_mode = ClientInputMode::kNormal;
                            if (!show_local_status(read_only ?
                                                   "read-only: unsupported prefix command" :
                                                   "unsupported prefix command")) {
                                done = true;
                                break;
                            }
                        }
                        continue;
                    }

                    if (ch == kPrefixKey) {
                        input_mode = ClientInputMode::kPrefix;
                        continue;
                    }

                    if (!read_only) {
                        if (!send_message(socket_fd, MessageType::kClientInput, 0, 0, &ch, 1)) {
                            done = true;
                            break;
                        }
                    }
                }
            } else if (read_rc == 0) {
                done = true;
            } else if (errno != EINTR && errno != EAGAIN) {
                done = true;
            }
        }

        if (pfds[1].revents & (POLLHUP | POLLERR)) {
            done = true;
        } else if (pfds[1].revents & POLLIN) {
            Message message;
            if (!recv_message(socket_fd, message)) {
                done = true;
            } else if (message.type == MessageType::kServerOutput ||
                       message.type == MessageType::kServerStatus) {
                const std::string chunk(message.payload.begin(), message.payload.end());
                if (message.type == MessageType::kServerStatus) {
                    view_state.local_status = chunk;
                    if (use_structured_client_render(view_state)) {
                        if (!ensure_render_surface() ||
                            !render_client_view(view_state, command_buffer, input_mode)) {
                            done = true;
                        }
                    } else if (!show_local_status(chunk)) {
                        done = true;
                    }
                } else {
                    view_state.local_status.clear();
                    append_client_output(view_state, message.arg0, chunk);
                    if (use_structured_client_render(view_state)) {
                        if (!ensure_render_surface() ||
                            !render_client_view(view_state, command_buffer, input_mode)) {
                            done = true;
                        }
                    } else if (!write_stdout(chunk)) {
                        done = true;
                    }
                }
            } else if (message.type == MessageType::kServerRedraw) {
                const std::string redraw(message.payload.begin(), message.payload.end());
                view_state.focused_pane_id = message.arg0;
                view_state.layout = parse_redraw_layout(redraw, message.arg0);
                if (use_structured_client_render(view_state)) {
                    if (!ensure_render_surface() ||
                        !render_client_view(view_state, command_buffer, input_mode)) {
                        done = true;
                    }
                } else if (!ensure_render_surface()) {
                    done = true;
                }
            } else if (message.type == MessageType::kServerExit) {
                exit_code = message.arg0;
                done = true;
            }
        }
    }

    if (alternate_screen_active) {
        leave_alternate_screen();
    }

    terminal_guard.restore();
    write_stdout("[?25h");
    show_local_status("mini-tmux: detached");
    close(sig_pipe[0]);
    close(sig_pipe[1]);
    return exit_code;
}

bool wait_for_server(const std::string &socket_path, int attempts, useconds_t sleep_us) {
    for (int i = 0; i < attempts; ++i) {
        const int fd = connect_to_server(socket_path);
        if (fd >= 0) {
            close(fd);
            return true;
        }
        usleep(sleep_us);
    }
    return false;
}

int start_server_and_attach(const std::string &socket_path, bool read_only) {
    const winsize initial_size = get_winsize_from_fd(STDIN_FILENO);

    const pid_t pid = fork();
    if (pid < 0) {
        perr("fork(server)");
        return 1;
    }

    if (pid == 0) {
        if (setsid() < 0) {
            _exit(1);
        }
        redirect_stdio_to_devnull();
        const int rc = server_process(socket_path, initial_size);
        _exit(rc);
    }

    if (!wait_for_server(socket_path, 100, 10000)) {
        std::cerr << "failed to start server at " << socket_path << "\n";
        return 1;
    }

    const int socket_fd = connect_to_server(socket_path);
    if (socket_fd < 0) {
        perr("connect(server)");
        return 1;
    }
    const int rc = client_process(socket_fd, read_only);
    close(socket_fd);
    return rc;
}

int attach_to_existing_server(const std::string &socket_path, bool read_only) {
    const int socket_fd = connect_to_server(socket_path);
    if (socket_fd < 0) {
        perr("attach");
        return 1;
    }
    const int rc = client_process(socket_fd, read_only);
    close(socket_fd);
    return rc;
}

void print_usage(const char *argv0) {
    std::cerr << "Usage: " << argv0 << " [attach [-r]]\n";
}

}  // namespace

int main(int argc, char **argv) {
    const std::string socket_path = get_socket_path();

    if (argc == 1) {
        const int existing_fd = connect_to_server(socket_path);
        if (existing_fd >= 0) {
            close(existing_fd);
            return attach_to_existing_server(socket_path, false);
        }
        return start_server_and_attach(socket_path, false);
    }

    if (argc == 2 && std::string(argv[1]) == "attach") {
        return attach_to_existing_server(socket_path, false);
    }

    if (argc == 3 && std::string(argv[1]) == "attach" && std::string(argv[2]) == "-r") {
        return attach_to_existing_server(socket_path, true);
    }

    print_usage(argv[0]);
    return 1;
}
