#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
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
constexpr int32_t kAttachDefaultSession = -1;
constexpr int32_t kCreateNewSession = -2;

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

bool write_best_effort(int fd, const void *buf, size_t len) {
    const char *ptr = static_cast<const char *>(buf);
    size_t written = 0;
    while (written < len) {
        const ssize_t rc = write(fd, ptr + written, len - written);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            return false;
        }
        if (rc == 0) {
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

void close_inherited_fds_in_child() {
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0) {
        max_fd = 1024;
    }
    for (int fd = STDERR_FILENO + 1; fd < max_fd; ++fd) {
        close(fd);
    }
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
    kClientListSessions = 9,
    kClientKillSession = 10,
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

bool parse_size_payload(const std::vector<char> &payload, winsize &size) {
    if (payload.size() != sizeof(uint16_t) * 2) {
        return false;
    }
    uint16_t dims[2]{};
    std::memcpy(dims, payload.data(), sizeof(dims));
    size.ws_row = dims[0];
    size.ws_col = dims[1];
    size = normalize_winsize(size);
    return true;
}

template <typename T>
void append_payload_value(std::string &payload, T value) {
    payload.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

template <typename T>
bool read_payload_value(const std::string &payload, size_t &offset, T &value) {
    if (offset + sizeof(value) > payload.size()) {
        return false;
    }
    std::memcpy(&value, payload.data() + static_cast<std::ptrdiff_t>(offset), sizeof(value));
    offset += sizeof(value);
    return true;
}

struct ClientConnection {
    int fd = -1;
    bool hello_received = false;
    bool read_only = false;
    winsize size{};
    bool has_size = false;
    int attached_session_id = -1;
    bool exit_sent = false;
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
    size_t cursor_row = 0;
    size_t cursor_col = 0;
    EscapeState escape_state = EscapeState::kText;
    std::string csi_buffer;
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
};

struct ManagedSession {
    int session_id = -1;
    SessionState state{};
    int exit_code = 0;
    int exit_signal = 0;
};

bool pane_is_present(const PaneSlot &slot) {
    return slot.state != PaneState::kDestroyed;
}

bool pane_is_active(const PaneSlot &slot) {
    return slot.state == PaneState::kCreated || slot.state == PaneState::kRunning;
}

struct ServerState {
    std::string socket_path;
    int listen_fd = -1;
    int sigchld_read_fd = -1;
    int sigchld_write_fd = -1;
    bool should_exit = false;
    bool exit_notified = false;
    bool shutdown_pending = false;
    std::chrono::steady_clock::time_point shutdown_notice_deadline{};
    std::chrono::steady_clock::time_point shutdown_deadline{};
    int final_exit_code = 0;
    int final_exit_signal = 0;
    int next_session_id = 0;
    std::vector<ManagedSession> sessions;
    std::vector<ClientConnection> clients;
};

ManagedSession *find_managed_session(ServerState &server, int session_id) {
    for (ManagedSession &session : server.sessions) {
        if (session.session_id == session_id) {
            return &session;
        }
    }
    return nullptr;
}

const ManagedSession *find_managed_session(const ServerState &server, int session_id) {
    for (const ManagedSession &session : server.sessions) {
        if (session.session_id == session_id) {
            return &session;
        }
    }
    return nullptr;
}

SessionState *find_client_session(ServerState &server, const ClientConnection &client) {
    if (ManagedSession *managed = find_managed_session(server, client.attached_session_id); managed != nullptr) {
        return &managed->state;
    }
    return nullptr;
}

ptrdiff_t find_client_index_by_fd(const ServerState &server, int fd) {
    for (size_t i = 0; i < server.clients.size(); ++i) {
        if (server.clients[i].fd == fd) {
            return static_cast<ptrdiff_t>(i);
        }
    }
    return -1;
}


int default_attach_session_id(const ServerState &server) {
    if (server.sessions.empty()) {
        return -1;
    }
    return server.sessions.front().session_id;
}
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
    return write_best_effort(STDOUT_FILENO, text.data(), text.size());
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
    if (max_lines == 0) {
        buffer.lines.assign(1, std::string{});
        buffer.cursor_row = 0;
        buffer.cursor_col = 0;
        return;
    }
    if (buffer.lines.empty()) {
        buffer.lines.emplace_back();
    }
    if (buffer.lines.size() > max_lines) {
        const size_t removed = buffer.lines.size() - max_lines;
        buffer.lines.erase(buffer.lines.begin(),
                           buffer.lines.begin() + static_cast<std::ptrdiff_t>(removed));
        buffer.cursor_row = buffer.cursor_row >= removed ? buffer.cursor_row - removed : 0;
    }
    if (buffer.lines.empty()) {
        buffer.lines.emplace_back();
        buffer.cursor_row = 0;
    }
    if (buffer.cursor_row >= buffer.lines.size()) {
        buffer.cursor_row = buffer.lines.size() - 1;
    }
}

void ensure_text_buffer_row(TextScreenBuffer &buffer, size_t row) {
    while (buffer.lines.size() <= row) {
        buffer.lines.emplace_back();
    }
}

std::string &active_text_buffer_line(TextScreenBuffer &buffer) {
    ensure_text_buffer_row(buffer, buffer.cursor_row);
    return buffer.lines[buffer.cursor_row];
}

std::vector<int> parse_csi_params(const std::string &raw) {
    std::vector<int> values;
    std::string params = raw;
    if (!params.empty() && params[0] == '?') {
        params.erase(params.begin());
    }
    if (params.empty()) {
        return values;
    }
    size_t start = 0;
    while (start <= params.size()) {
        const size_t end = params.find(';', start);
        const std::string field = params.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (field.empty()) {
            values.push_back(0);
        } else {
            char *parse_end = nullptr;
            errno = 0;
            const long value = std::strtol(field.c_str(), &parse_end, 10);
            values.push_back(errno == 0 && parse_end != nullptr && *parse_end == '\0' ? static_cast<int>(value) : 0);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return values;
}

void clear_line(TextScreenBuffer &buffer, int mode) {
    std::string &line = active_text_buffer_line(buffer);
    if (mode == 2) {
        line.clear();
        return;
    }
    if (mode == 1) {
        if (line.size() < buffer.cursor_col + 1) {
            line.resize(buffer.cursor_col + 1, ' ');
        }
        for (size_t i = 0; i <= buffer.cursor_col && i < line.size(); ++i) {
            line[i] = ' ';
        }
        return;
    }
    if (buffer.cursor_col < line.size()) {
        line.erase(static_cast<std::string::size_type>(buffer.cursor_col));
    }
}

void clear_screen(TextScreenBuffer &buffer, int mode) {
    ensure_text_buffer_row(buffer, buffer.cursor_row);
    if (mode == 2 || mode == 3) {
        for (std::string &line : buffer.lines) {
            line.clear();
        }
        return;
    }
    if (mode == 1) {
        for (size_t row = 0; row < buffer.cursor_row && row < buffer.lines.size(); ++row) {
            buffer.lines[row].clear();
        }
        clear_line(buffer, 1);
        return;
    }
    clear_line(buffer, 0);
    for (size_t row = buffer.cursor_row + 1; row < buffer.lines.size(); ++row) {
        buffer.lines[row].clear();
    }
}

void append_printable_char(TextScreenBuffer &buffer, char ch) {
    std::string &line = active_text_buffer_line(buffer);
    if (buffer.cursor_col < line.size()) {
        line[buffer.cursor_col] = ch;
    } else {
        while (line.size() < buffer.cursor_col) {
            line.push_back(' ');
        }
        line.push_back(ch);
    }
    ++buffer.cursor_col;
}

void append_spaces(TextScreenBuffer &buffer, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        append_printable_char(buffer, ' ');
    }
}

void handle_backspace(TextScreenBuffer &buffer) {
    std::string &line = active_text_buffer_line(buffer);
    if (buffer.cursor_col == 0 || line.empty()) {
        return;
    }
    const size_t erase_at = buffer.cursor_col - 1;
    if (erase_at < line.size()) {
        line.erase(line.begin() + static_cast<std::ptrdiff_t>(erase_at));
    }
    --buffer.cursor_col;
}

void handle_newline(TextScreenBuffer &buffer, size_t max_lines) {
    ++buffer.cursor_row;
    buffer.cursor_col = 0;
    ensure_text_buffer_row(buffer, buffer.cursor_row);
    trim_text_buffer_lines(buffer, max_lines);
}

void handle_csi_final(TextScreenBuffer &buffer, const std::string &raw, char final_byte) {
    const std::vector<int> values = parse_csi_params(raw);
    auto first_value = [&](int fallback) {
        return !values.empty() && values[0] > 0 ? values[0] : fallback;
    };

    if (final_byte == 'A') {
        const size_t amount = static_cast<size_t>(first_value(1));
        buffer.cursor_row = buffer.cursor_row > amount ? buffer.cursor_row - amount : 0;
        return;
    }
    if (final_byte == 'B') {
        buffer.cursor_row += static_cast<size_t>(first_value(1));
        ensure_text_buffer_row(buffer, buffer.cursor_row);
        return;
    }
    if (final_byte == 'C') {
        buffer.cursor_col += static_cast<size_t>(first_value(1));
        return;
    }
    if (final_byte == 'D') {
        const size_t amount = static_cast<size_t>(first_value(1));
        buffer.cursor_col = buffer.cursor_col > amount ? buffer.cursor_col - amount : 0;
        return;
    }
    if (final_byte == 'H' || final_byte == 'f') {
        const size_t row = static_cast<size_t>(first_value(1));
        const size_t col = static_cast<size_t>(values.size() >= 2 && values[1] > 0 ? values[1] : 1);
        buffer.cursor_row = row > 0 ? row - 1 : 0;
        buffer.cursor_col = col > 0 ? col - 1 : 0;
        ensure_text_buffer_row(buffer, buffer.cursor_row);
        return;
    }
    if (final_byte == 'J') {
        clear_screen(buffer, values.empty() ? 0 : values[0]);
        return;
    }
    if (final_byte == 'K') {
        clear_line(buffer, values.empty() ? 0 : values[0]);
        return;
    }
}

void append_text_output(TextScreenBuffer &buffer, const std::string &chunk, size_t max_lines) {
    ensure_text_buffer_row(buffer, buffer.cursor_row);
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
                    buffer.csi_buffer.clear();
                } else if (ch == ']') {
                    buffer.escape_state = TextScreenBuffer::EscapeState::kOsc;
                } else {
                    buffer.escape_state = TextScreenBuffer::EscapeState::kText;
                }
                continue;
            case TextScreenBuffer::EscapeState::kCsi:
                if (ch >= 0x40 && ch <= 0x7e) {
                    handle_csi_final(buffer, buffer.csi_buffer, ch);
                    buffer.csi_buffer.clear();
                    buffer.escape_state = TextScreenBuffer::EscapeState::kText;
                } else {
                    buffer.csi_buffer.push_back(ch);
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
    if (buffer.lines.empty()) {
        return "";
    }
    std::string snapshot = buffer.lines[0];
    for (size_t i = 1; i < buffer.lines.size(); ++i) {
        snapshot.push_back('\n');
        snapshot += buffer.lines[i];
    }
    return snapshot;
}

TextScreenBuffer parse_text_snapshot(const std::string &snapshot, size_t cursor_row, size_t cursor_col) {
    TextScreenBuffer buffer{};
    size_t start = 0;
    while (start <= snapshot.size()) {
        const size_t newline = snapshot.find('\n', start);
        if (newline == std::string::npos) {
            buffer.lines.push_back(snapshot.substr(start));
            break;
        }
        buffer.lines.push_back(snapshot.substr(start, newline - start));
        start = newline + 1;
        if (start == snapshot.size()) {
            buffer.lines.emplace_back();
            break;
        }
    }
    if (buffer.lines.empty()) {
        buffer.lines.emplace_back();
    }
    buffer.cursor_row = std::min(cursor_row, buffer.lines.size() - 1);
    buffer.cursor_col = cursor_col;
    return buffer;
}
void append_client_output(ClientViewState &view, int pane_id, const std::string &chunk) {
    ClientPaneBuffer *buffer = find_client_buffer(view, pane_id);
    append_text_output(buffer->text, chunk, kClientBufferedLines);
}

bool parse_redraw_payload(const std::string &payload, ClientViewState &view) {
    std::vector<PaneLayout> layout;
    std::vector<ClientPaneBuffer> buffers;
    size_t offset = 0;
    if (payload.rfind("redraw:", 0) == 0) {
        const size_t newline = payload.find('\n');
        if (newline == std::string::npos) {
            return false;
        }
        offset = newline + 1;
    }
    uint32_t pane_count = 0;
    if (!read_payload_value(payload, offset, pane_count)) {
        return false;
    }

    int focused_pane_id = -1;
    for (uint32_t i = 0; i < pane_count; ++i) {
        PaneLayout pane{};
        uint8_t focused = 0;
        uint16_t cursor_row = 0;
        uint16_t cursor_col = 0;
        uint32_t snapshot_size = 0;
        if (!read_payload_value(payload, offset, pane.pane_id) ||
            !read_payload_value(payload, offset, pane.top) ||
            !read_payload_value(payload, offset, pane.rows) ||
            !read_payload_value(payload, offset, pane.cols) ||
            !read_payload_value(payload, offset, focused) ||
            !read_payload_value(payload, offset, cursor_row) ||
            !read_payload_value(payload, offset, cursor_col) ||
            !read_payload_value(payload, offset, snapshot_size)) {
            return false;
        }
        if (offset + snapshot_size > payload.size()) {
            return false;
        }

        pane.focused = focused != 0;
        if (pane.focused) {
            focused_pane_id = pane.pane_id;
        }

        ClientPaneBuffer pane_buffer{};
        pane_buffer.pane_id = pane.pane_id;
        pane_buffer.text = parse_text_snapshot(
            payload.substr(offset, snapshot_size),
            static_cast<size_t>(cursor_row),
            static_cast<size_t>(cursor_col));
        offset += snapshot_size;

        layout.push_back(pane);
        buffers.push_back(std::move(pane_buffer));
    }

    if (offset != payload.size()) {
        return false;
    }

    view.layout = std::move(layout);
    view.buffers = std::move(buffers);
    view.focused_pane_id = focused_pane_id;
    return true;
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

bool render_client_view(const ClientViewState &view, const std::string &command_buffer,
                        ClientInputMode input_mode) {
    std::string frame = "\x1b[?25l\x1b[?7l\x1b[H\x1b[2J";
    int screen_row = 1;
    int cursor_row = 1;
    int cursor_col = 1;
    bool cursor_positioned = false;
    bool command_prompt_positioned = false;
    int command_prompt_row = 1;
    int command_prompt_col = 1;

    auto append_row = [&](int row, const std::string &text, unsigned short width) {
        frame += "\x1b[" + std::to_string(row) + ";1H";
        frame += fit_to_width(text, width);
        frame += "\x1b[K";
    };

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
        const int header_row = screen_row;
        append_row(screen_row, header, pane.cols);
        ++screen_row;

        std::vector<std::string> visible_lines;
        if (buffer != nullptr) {
            visible_lines = buffer->text.lines;
        }
        if (visible_lines.empty()) {
            visible_lines.emplace_back();
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
                const int raw_row = buffer != nullptr ? static_cast<int>(buffer->text.cursor_row) : 0;
                int cursor_offset = raw_row - start;
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
            if (input_mode == ClientInputMode::kCommand) {
                command_prompt_positioned = true;
                command_prompt_row = cursor_row;
                command_prompt_col = cursor_col;
            }
        }

        for (int row = 0; row < body_rows; ++row) {
            std::string line;
            const int source = start + row;
            if (source >= 0 && source < static_cast<int>(visible_lines.size())) {
                line = visible_lines[source];
            }
            append_row(screen_row, line, pane.cols);
            ++screen_row;
        }
        if (i + 1 != view.layout.size()) {
            append_row(screen_row, std::string(pane.cols, '-'), pane.cols);
            ++screen_row;
        }
    }

    if (input_mode == ClientInputMode::kCommand && command_prompt_positioned) {
        frame += "\x1b[" + std::to_string(command_prompt_row) + ";" + std::to_string(command_prompt_col) + "H:";
        frame += command_buffer;
        frame += "\x1b[K";
        const int visible_cols = std::max<int>(1, view.layout.empty() ? 1 : view.layout.front().cols);
        const int desired_col = command_prompt_col + 1 + static_cast<int>(command_buffer.size());
        cursor_row = command_prompt_row;
        cursor_col = std::min(desired_col, visible_cols);
    }

    if (cursor_positioned) {
        frame += "\x1b[" + std::to_string(cursor_row) + ";" + std::to_string(cursor_col) + "H";
    } else {
        frame += "\x1b[H";
    }
    frame += "\x1b[?7h\x1b[?25h";
    return write_best_effort(STDOUT_FILENO, frame.data(), frame.size());
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

void stop_pane_pipeout(PaneSlot &slot, bool terminate_process) {
    close_fd_if_valid(slot.output.pipeout.write_fd);
    const pid_t pid = slot.output.pipeout.child_pid;
    if (pid > 0 && terminate_process) {
        if (kill(pid, SIGHUP) != 0 && errno != ESRCH) {
            // Best-effort cleanup; the SIGCHLD path still reaps the child when it exits.
        }
    }
    if (pid <= 0) {
        slot.output.pipeout.child_pid = -1;
    }
}

void cleanup_pane_outputs(PaneSlot &slot) {
    stop_pane_log(slot);
    stop_pane_pipeout(slot, true);
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
    stop_pane_pipeout(slot, true);

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
void signal_foreground_process_group(const Pane &pane, int sig);

std::vector<size_t> collect_live_pane_indices(const SessionState &session) {
    std::vector<size_t> indices;
    for (size_t i = 0; i < session.panes.size(); ++i) {
        if (pane_is_active(session.panes[i])) {
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
    unsigned short top = 0;
    unsigned short remaining_rows = total_rows;

    for (size_t order = 0; order < live_indices.size(); ++order) {
        const PaneSlot &slot = session.panes[live_indices[order]];
        const size_t panes_left = pane_count - order;
        unsigned short rows = 0;
        if (panes_left > 0) {
            rows = static_cast<unsigned short>(remaining_rows / panes_left);
            if (remaining_rows % panes_left != 0) {
                rows = static_cast<unsigned short>(rows + 1);
            }
        }
        layout.push_back(PaneLayout{
            slot.pane_id,
            top,
            rows,
            total_cols,
            slot.pane_id == session.focused_pane_id,
        });
        top = static_cast<unsigned short>(top + rows);
        remaining_rows = total_rows > top ? static_cast<unsigned short>(total_rows - top) : 0;
    }
    return layout;
}

void apply_session_layout(SessionState &session) {
    const std::vector<PaneLayout> layout = compute_vertical_layout(session);
    for (const PaneLayout &entry : layout) {
        for (PaneSlot &slot : session.panes) {
            if (slot.pane_id != entry.pane_id || !pane_is_active(slot)) {
                continue;
            }
            const bool size_changed = slot.pane.size.ws_row != entry.rows ||
                                      slot.pane.size.ws_col != entry.cols;
            slot.pane.size.ws_row = entry.rows;
            slot.pane.size.ws_col = entry.cols;
            if (slot.pane.master_fd >= 0) {
                apply_winsize(slot.pane.master_fd, slot.pane.size);
                if (size_changed) {
                    signal_foreground_process_group(slot.pane, SIGWINCH);
                }
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

std::string build_redraw_payload(const SessionState &session) {
    const std::vector<PaneLayout> layout = compute_vertical_layout(session);
    std::string payload = build_redraw_summary(session);
    payload.push_back('\n');
    append_payload_value(payload, static_cast<uint32_t>(layout.size()));

    for (size_t i = 0; i < layout.size(); ++i) {
        const PaneLayout &entry = layout[i];
        for (const PaneSlot &slot : session.panes) {
            if (slot.pane_id != entry.pane_id || slot.state == PaneState::kDestroyed) {
                continue;
            }

            std::vector<std::string> visible_lines = slot.output.capture.lines;
            if (visible_lines.empty()) {
                visible_lines.emplace_back();
            }
            const int reserved_rows = 1 + (i + 1 != layout.size() ? 1 : 0);
            const int body_rows = std::max<int>(0, static_cast<int>(entry.rows) - reserved_rows);
            int start = static_cast<int>(visible_lines.size()) - body_rows;
            if (start < 0) {
                start = 0;
            }

            TextScreenBuffer visible_buffer{};
            if (body_rows > 0) {
                visible_buffer.lines.reserve(static_cast<size_t>(body_rows));
                for (int row = 0; row < body_rows; ++row) {
                    std::string line;
                    const int source = start + row;
                    if (source >= 0 && source < static_cast<int>(visible_lines.size())) {
                        line = visible_lines[source];
                    }
                    if (entry.cols > 0 && line.size() > entry.cols) {
                        line.resize(entry.cols);
                    }
                    visible_buffer.lines.push_back(std::move(line));
                }
                const int raw_row = static_cast<int>(slot.output.capture.cursor_row);
                int relative_row = raw_row - start;
                if (relative_row < 0) {
                    relative_row = 0;
                }
                if (relative_row >= body_rows) {
                    relative_row = body_rows - 1;
                }
                visible_buffer.cursor_row = static_cast<size_t>(relative_row);
            } else {
                visible_buffer.lines.emplace_back();
                visible_buffer.cursor_row = 0;
            }
            visible_buffer.cursor_col = slot.output.capture.cursor_col;
            const std::string snapshot = render_text_snapshot(visible_buffer);

            append_payload_value(payload, static_cast<int32_t>(slot.pane_id));
            append_payload_value(payload, entry.top);
            append_payload_value(payload, entry.rows);
            append_payload_value(payload, entry.cols);
            append_payload_value(payload, static_cast<uint8_t>(entry.focused ? 1 : 0));
            append_payload_value(payload, static_cast<uint16_t>(visible_buffer.cursor_row));
            append_payload_value(payload, static_cast<uint16_t>(visible_buffer.cursor_col));
            append_payload_value(payload, static_cast<uint32_t>(snapshot.size()));
            payload.append(snapshot);
            break;
        }
    }
    return payload;
}

PaneSlot *find_pane_slot(SessionState &session, int pane_id) {
    for (PaneSlot &slot : session.panes) {
        if (slot.pane_id == pane_id && pane_is_present(slot)) {
            return &slot;
        }
    }
    return nullptr;
}

void destroy_pane_slot(PaneSlot &slot) {
    cleanup_pane_outputs(slot);
    close_fd_if_valid(slot.pane.master_fd);
    slot.pane.master_closed = true;
    slot.state = slot.pane.child_reaped ? PaneState::kDestroyed : PaneState::kExited;
}

int choose_focus_pane_id(const SessionState &session, int skip_pane_id) {
    for (const PaneSlot &slot : session.panes) {
        if (pane_is_active(slot) && slot.pane_id != skip_pane_id) {
            return slot.pane_id;
        }
    }
    return -1;
}

std::vector<int> collect_live_pane_ids(const SessionState &session) {
    std::vector<int> pane_ids;
    for (const PaneSlot &slot : session.panes) {
        if (pane_is_active(slot)) {
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
    if (session.focused_pane_id >= 0) {
        if (PaneSlot *slot = find_pane_slot(session, session.focused_pane_id);
            slot != nullptr && pane_is_active(*slot)) {
            return;
        }
    }
    session.focused_pane_id = choose_focus_pane_id(session, -1);
}

bool session_has_live_panes(const SessionState &session) {
    for (const PaneSlot &slot : session.panes) {
        if (pane_is_active(slot)) {
            return true;
        }
    }
    return false;
}


bool send_client_hello(int socket_fd, bool read_only, int session_request) {
    const winsize ws = get_winsize_from_fd(STDIN_FILENO);
    const uint16_t dims[2] = {ws.ws_row, ws.ws_col};
    return send_message(socket_fd, MessageType::kClientHello, read_only ? 1 : 0, session_request,
                        reinterpret_cast<const char *>(dims), sizeof(dims));
}

bool apply_winsize(int master_fd, const winsize &ws) {
    return ioctl(master_fd, TIOCSWINSZ, &ws) == 0;
}

pid_t foreground_process_group(int master_fd) {
    const pid_t pgid = tcgetpgrp(master_fd);
    if (pgid < 0) {
        return -1;
    }
    return pgid;
}

pid_t pane_signal_process_group(const Pane &pane) {
    pid_t pgid = -1;
    if (pane.master_fd >= 0) {
        pgid = foreground_process_group(pane.master_fd);
    }
    if (pgid <= 0 && pane.child_pid > 0) {
        pgid = getpgid(pane.child_pid);
    }
    return pgid;
}

void signal_foreground_process_group(const Pane &pane, int sig) {
    const pid_t pgid = pane_signal_process_group(pane);
    if (pgid <= 0) {
        return;
    }
    if (kill(-pgid, sig) != 0 && errno != ESRCH) {
        // Best-effort notification; process groups may disappear during layout or detach races.
    }
}

winsize session_size_from_clients(const std::vector<ClientConnection> &clients,
                                  int session_id,
                                  winsize fallback) {
    fallback = normalize_winsize(fallback);
    bool have_size = false;
    winsize size{};
    for (const ClientConnection &client : clients) {
        if (!client.has_size || client.attached_session_id != session_id) {
            continue;
        }
        if (!have_size) {
            size = normalize_winsize(client.size);
            have_size = true;
            continue;
        }
        size.ws_row = std::min(size.ws_row, client.size.ws_row);
        size.ws_col = std::min(size.ws_col, client.size.ws_col);
    }
    return have_size ? normalize_winsize(size) : fallback;
}

void refresh_session_size_from_clients(ServerState &server, int session_id) {
    if (ManagedSession *managed = find_managed_session(server, session_id); managed != nullptr) {
        managed->state.size = session_size_from_clients(server.clients, session_id, managed->state.size);
    }
}


bool spawn_pane_slot(SessionState &session, int pane_id, const winsize &size);

bool create_managed_session(ServerState &server, const winsize &initial_size, int &session_id) {
    ManagedSession managed{};
    managed.session_id = server.next_session_id++;
    managed.state.size = initial_size;
    if (!spawn_pane_slot(managed.state, 0, initial_size)) {
        return false;
    }
    managed.state.focused_pane_id = 0;
    apply_session_layout(managed.state);
    session_id = managed.session_id;
    server.sessions.push_back(std::move(managed));
    return true;
}

bool session_ready_for_removal(const ManagedSession &managed) {
    if (session_has_live_panes(managed.state)) {
        return false;
    }
    for (const PaneSlot &slot : managed.state.panes) {
        if (slot.state != PaneState::kDestroyed) {
            return false;
        }
    }
    return true;
}

void cleanup_managed_session(ManagedSession &managed) {
    for (PaneSlot &slot : managed.state.panes) {
        cleanup_pane_outputs(slot);
        if (slot.pane.child_pid > 0 && !slot.pane.child_reaped) {
            kill(slot.pane.child_pid, SIGHUP);
            waitpid(slot.pane.child_pid, nullptr, 0);
            slot.pane.child_reaped = true;
        }
        close_fd_if_valid(slot.pane.master_fd);
        slot.pane.master_closed = true;
        slot.state = PaneState::kDestroyed;
    }
}

void request_session_shutdown(ManagedSession &managed) {
    for (PaneSlot &slot : managed.state.panes) {
        if (slot.state == PaneState::kDestroyed) {
            continue;
        }
        signal_foreground_process_group(slot.pane, SIGHUP);
        if (slot.pane.child_pid > 0) {
            kill(slot.pane.child_pid, SIGHUP);
        }
        destroy_pane_slot(slot);
    }
    maybe_repair_focus(managed.state);
    apply_session_layout(managed.state);
}

void reap_children(ServerState &server) {
    while (true) {
        int status = 0;
        const pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) {
            break;
        }

        bool handled = false;
        for (ManagedSession &managed : server.sessions) {
            if (PaneSlot *pipe_slot = find_pipeout_slot_by_pid(managed.state, pid); pipe_slot != nullptr) {
                close_fd_if_valid(pipe_slot->output.pipeout.write_fd);
                pipe_slot->output.pipeout.child_pid = -1;
                handled = true;
                break;
            }
            for (PaneSlot &slot : managed.state.panes) {
                if (pid != slot.pane.child_pid) {
                    continue;
                }
                slot.pane.child_reaped = true;
                if (slot.state != PaneState::kDestroyed) {
                    slot.state = PaneState::kReaped;
                }
                cleanup_pane_outputs(slot);
                if (WIFEXITED(status)) {
                    slot.pane.exit_code = WEXITSTATUS(status);
                    slot.pane.exit_signal = 0;
                } else if (WIFSIGNALED(status)) {
                    slot.pane.exit_code = 128 + WTERMSIG(status);
                    slot.pane.exit_signal = WTERMSIG(status);
                }
                managed.exit_code = slot.pane.exit_code;
                managed.exit_signal = slot.pane.exit_signal;
                server.final_exit_code = slot.pane.exit_code;
                server.final_exit_signal = slot.pane.exit_signal;
                handled = true;
                break;
            }
            if (handled) {
                break;
            }
        }
    }
}

std::string format_session_listing(const ServerState &server) {
    std::ostringstream out;
    for (const ManagedSession &managed : server.sessions) {
        out << managed.session_id << "\n";
    }
    return out.str();
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
        if (tcsetpgrp(STDIN_FILENO, getpgrp()) < 0) {
            _exit(1);
        }
        close_inherited_fds_in_child();

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
    SessionState *attached_session = find_client_session(server, client);
    if (attached_session == nullptr) {
        return format_error("client is not attached to a session");
    }
    SessionState &session = *attached_session;

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

        const int pane_id = session.next_pane_id++;
        if (!spawn_pane_slot(session, pane_id, session.size)) {
            return format_error("failed to create pane");
        }
        session.focused_pane_id = pane_id;
        apply_session_layout(session);
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

        PaneSlot *slot = find_pane_slot(session, pane_id);
        if (slot == nullptr) {
            return format_error("pane " + std::to_string(pane_id) + " does not exist");
        }
        if (slot->pane.child_reaped || slot->state == PaneState::kDestroyed) {
            return format_ok("pane " + std::to_string(pane_id) + " is already exiting");
        }

        signal_foreground_process_group(slot->pane, SIGHUP);
        if (slot->pane.child_pid > 0 && kill(slot->pane.child_pid, SIGHUP) != 0 && errno != ESRCH) {
            return format_error("failed to signal pane " + std::to_string(pane_id));
        }
        destroy_pane_slot(*slot);
        if (session.focused_pane_id == pane_id) {
            session.focused_pane_id = choose_focus_pane_id(session, pane_id);
        }
        apply_session_layout(session);
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
        return focus_pane(session, pane_id);
    }

    if (name == "next" || name == "prev") {
        if (!rest.empty()) {
            return format_usage(name == "next" ? ":next" : ":prev");
        }
        if (client.read_only) {
            return format_error("read-only clients cannot run :" + name);
        }

        const int pane_id = choose_adjacent_focus_pane_id(session, name == "next" ? 1 : -1);
        if (pane_id < 0) {
            return format_error("no panes available");
        }
        return focus_pane(session, pane_id);
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

        PaneSlot *slot = find_pane_slot(session, pane_id);
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

        PaneSlot *slot = find_pane_slot(session, pane_id);
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

        PaneSlot *slot = find_pane_slot(session, pane_id);
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

        PaneSlot *slot = find_pane_slot(session, pane_id);
        if (slot == nullptr) {
            return format_error("pane " + std::to_string(pane_id) + " does not exist");
        }
        stop_pane_pipeout(*slot, true);
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

        PaneSlot *slot = find_pane_slot(session, pane_id);
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

void remove_client(ServerState &server, size_t index) {
    const int session_id = server.clients[index].attached_session_id;
    close_fd_if_valid(server.clients[index].fd);
    server.clients.erase(server.clients.begin() + static_cast<std::ptrdiff_t>(index));
    if (session_id >= 0) {
        refresh_session_size_from_clients(server, session_id);
        if (ManagedSession *managed = find_managed_session(server, session_id); managed != nullptr) {
            apply_session_layout(managed->state);
        }
    }
}

void broadcast_output(ServerState &server, int session_id, int pane_id, const char *data, size_t size) {
    if (ManagedSession *managed = find_managed_session(server, session_id); managed != nullptr) {
        if (PaneSlot *slot = find_pane_slot(managed->state, pane_id); slot != nullptr) {
            record_pane_output(*slot, data, size);
        }
    }
    for (size_t i = 0; i < server.clients.size();) {
        if (!server.clients[i].hello_received || server.clients[i].attached_session_id != session_id) {
            ++i;
            continue;
        }
        if (!send_message(server.clients[i].fd, MessageType::kServerOutput, pane_id, 0, data, size)) {
            remove_client(server, i);
            continue;
        }
        ++i;
    }
}

bool send_redraw_to_client(int fd, const SessionState &session) {
    const std::string payload = build_redraw_payload(session);
    return send_message(fd, MessageType::kServerRedraw, session.focused_pane_id,
                        static_cast<int32_t>(session.panes.size()), payload.data(), payload.size());
}

bool send_initial_client_state(int fd, const SessionState &session) {
    return send_redraw_to_client(fd, session);
}

void broadcast_redraw(ServerState &server, int session_id) {
    const ManagedSession *managed = find_managed_session(server, session_id);
    if (managed == nullptr) {
        return;
    }
    for (size_t i = 0; i < server.clients.size();) {
        if (!server.clients[i].hello_received || server.clients[i].attached_session_id != session_id) {
            ++i;
            continue;
        }
        if (!send_redraw_to_client(server.clients[i].fd, managed->state)) {
            remove_client(server, i);
            continue;
        }
        ++i;
    }
}

void notify_session_exit(ServerState &server, int session_id, int exit_code, int exit_signal) {
    for (size_t i = 0; i < server.clients.size();) {
        if (!server.clients[i].hello_received || server.clients[i].attached_session_id != session_id) {
            ++i;
            continue;
        }
        const bool sent = send_message(server.clients[i].fd, MessageType::kServerExit,
                                       exit_code, exit_signal, nullptr, 0);
        server.clients[i].exit_sent = sent;
        if (!sent) {
            remove_client(server, i);
            continue;
        }
        ++i;
    }
}

void notify_server_exit(ServerState &server) {
    for (size_t i = 0; i < server.clients.size();) {
        if (!server.clients[i].hello_received) {
            ++i;
            continue;
        }
        if (server.clients[i].exit_sent) {
            ++i;
            continue;
        }
        if (!send_message(server.clients[i].fd, MessageType::kServerExit,
                          server.final_exit_code, server.final_exit_signal, nullptr, 0)) {
            remove_client(server, i);
            continue;
        }
        server.clients[i].exit_sent = true;
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

    int initial_session_id = -1;
    if (!create_managed_session(server, initial_size, initial_session_id)) {
        perr("spawn pane");
        return 1;
    }

    server.listen_fd = create_listen_socket(socket_path);
    if (server.listen_fd < 0) {
        perr("listen socket");
        for (ManagedSession &managed : server.sessions) {
            cleanup_managed_session(managed);
        }
        return 1;
    }

    struct PanePollEntry {
        int session_id = -1;
        size_t pane_index = 0;
    };

    while (!server.should_exit) {
        reap_children(server);

        int poll_timeout_ms = -1;
        if (server.shutdown_pending) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= server.shutdown_deadline) {
                poll_timeout_ms = 0;
            } else {
                poll_timeout_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    server.shutdown_deadline - now).count());
            }
        }

        std::vector<std::pair<int, std::string>> redraw_before_poll;
        redraw_before_poll.reserve(server.sessions.size());
        for (const ManagedSession &managed : server.sessions) {
            redraw_before_poll.push_back({managed.session_id, build_redraw_summary(managed.state)});
        }

        std::vector<pollfd> pfds;
        std::vector<PanePollEntry> pane_poll_entries;
        std::vector<int> polled_client_fds;
        polled_client_fds.reserve(server.clients.size());
        pfds.push_back({server.listen_fd, POLLIN, 0});
        pfds.push_back({server.sigchld_read_fd, POLLIN, 0});
        for (const ManagedSession &managed : server.sessions) {
            for (size_t pane_index = 0; pane_index < managed.state.panes.size(); ++pane_index) {
                const PaneSlot &slot = managed.state.panes[pane_index];
                if (slot.pane.master_fd >= 0) {
                    pfds.push_back({slot.pane.master_fd, static_cast<short>(POLLIN | POLLHUP | POLLERR), 0});
                    pane_poll_entries.push_back({managed.session_id, pane_index});
                }
            }
        }
        for (const ClientConnection &client : server.clients) {
            pfds.push_back({client.fd, static_cast<short>(POLLIN | POLLHUP | POLLERR), 0});
            polled_client_fds.push_back(client.fd);
        }

        const int rc = poll(pfds.data(), pfds.size(), poll_timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                reap_children(server);
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
                server.clients.push_back({client_fd, false, false, {}, false, -1, false});
            }
        }
        ++idx;

        if (pfds[idx].revents & POLLIN) {
            drain_fd(server.sigchld_read_fd);
            reap_children(server);
        }
        ++idx;

        for (size_t pane_poll_cursor = 0; pane_poll_cursor < pane_poll_entries.size(); ++pane_poll_cursor, ++idx) {
            const PanePollEntry &entry = pane_poll_entries[pane_poll_cursor];
            ManagedSession *managed = find_managed_session(server, entry.session_id);
            if (managed == nullptr || entry.pane_index >= managed->state.panes.size()) {
                continue;
            }
            PaneSlot &slot = managed->state.panes[entry.pane_index];
            if (pfds[idx].revents & (POLLIN | POLLHUP | POLLERR)) {
                char buffer[4096];
                const ssize_t read_rc = read(slot.pane.master_fd, buffer, sizeof(buffer));
                if (read_rc > 0) {
                    broadcast_output(server, entry.session_id, slot.pane_id, buffer, static_cast<size_t>(read_rc));
                } else if (read_rc == 0 || (read_rc < 0 && errno != EINTR && errno != EAGAIN)) {
                    close_fd_if_valid(slot.pane.master_fd);
                    slot.pane.master_closed = true;
                    if (slot.state == PaneState::kRunning) {
                        slot.state = PaneState::kExited;
                    }
                }
            }
        }

        for (size_t polled_client_index = 0; polled_client_index < polled_client_fds.size(); ++polled_client_index, ++idx) {
            const short revents = idx < pfds.size() ? pfds[idx].revents : 0;
            const ptrdiff_t found_index = find_client_index_by_fd(server, polled_client_fds[polled_client_index]);
            if (found_index < 0) {
                continue;
            }
            const size_t client_index = static_cast<size_t>(found_index);

            bool keep_client = true;
            if (revents & (POLLHUP | POLLERR)) {
                keep_client = false;
            } else if (revents & POLLIN) {
                Message message;
                if (!recv_message(server.clients[client_index].fd, message)) {
                    keep_client = false;
                } else if (!server.clients[client_index].hello_received) {
                    if (message.type == MessageType::kClientListSessions) {
                        const std::string listing = format_session_listing(server);
                        send_message(server.clients[client_index].fd, MessageType::kServerStatus, 0, 0,
                                     listing.data(), listing.size());
                        keep_client = false;
                    } else if (message.type == MessageType::kClientKillSession) {
                        const int session_id = message.arg0;
                        ManagedSession *managed = find_managed_session(server, session_id);
                        std::string status = format_error("session " + std::to_string(session_id) + " does not exist");
                        if (managed != nullptr) {
                            request_session_shutdown(*managed);
                            status = format_ok("terminating session " + std::to_string(session_id));
                        }
                        send_message(server.clients[client_index].fd, MessageType::kServerStatus, 0, 0,
                                     status.data(), status.size());
                        keep_client = false;
                    } else if (message.type != MessageType::kClientHello) {
                        keep_client = false;
                    } else {
                        server.clients[client_index].hello_received = true;
                        server.clients[client_index].read_only = message.arg0 != 0;

                        winsize hello_size{};
                        if (parse_size_payload(message.payload, hello_size)) {
                            server.clients[client_index].size = hello_size;
                            server.clients[client_index].has_size = true;
                        }

                        int target_session_id = message.arg1;
                        if (target_session_id == kCreateNewSession) {
                            const winsize session_size = server.clients[client_index].has_size ?
                                server.clients[client_index].size : initial_size;
                            if (!create_managed_session(server, session_size, target_session_id)) {
                                const std::string status = format_error("failed to create session");
                                send_message(server.clients[client_index].fd, MessageType::kServerStatus, 0, 0,
                                             status.data(), status.size());
                                keep_client = false;
                            }
                        } else if (target_session_id == kAttachDefaultSession) {
                            target_session_id = default_attach_session_id(server);
                        }

                        ManagedSession *managed = keep_client ? find_managed_session(server, target_session_id) : nullptr;
                        if (keep_client && managed == nullptr) {
                            const std::string status = format_error("session " + std::to_string(target_session_id) + " does not exist");
                            send_message(server.clients[client_index].fd, MessageType::kServerStatus, 0, 0,
                                         status.data(), status.size());
                            keep_client = false;
                        } else if (keep_client) {
                            server.clients[client_index].attached_session_id = target_session_id;
                            refresh_session_size_from_clients(server, target_session_id);
                            apply_session_layout(managed->state);
                            if (!send_initial_client_state(server.clients[client_index].fd, managed->state)) {
                                keep_client = false;
                            }
                        }
                    }
                } else if (message.type == MessageType::kClientInput) {
                    SessionState *session = find_client_session(server, server.clients[client_index]);
                    if (session != nullptr) {
                        PaneSlot *target = find_pane_slot(*session, session->focused_pane_id);
                        if (!server.clients[client_index].read_only && target != nullptr &&
                            target->pane.master_fd >= 0 && !message.payload.empty()) {
                            if (message.payload.size() == 1 &&
                                (message.payload[0] == 0x03 || message.payload[0] == 0x1a)) {
                                if (!write_all(target->pane.master_fd, message.payload.data(), message.payload.size())) {
                                    close_fd_if_valid(target->pane.master_fd);
                                    target->pane.master_closed = true;
                                    if (target->state == PaneState::kRunning) {
                                        target->state = PaneState::kExited;
                                    }
                                } else {
                                    signal_foreground_process_group(target->pane,
                                                                    message.payload[0] == 0x03 ? SIGINT : SIGTSTP);
                                }
                            } else if (!write_all(target->pane.master_fd, message.payload.data(), message.payload.size())) {
                                close_fd_if_valid(target->pane.master_fd);
                                target->pane.master_closed = true;
                                if (target->state == PaneState::kRunning) {
                                    target->state = PaneState::kExited;
                                }
                            }
                        }
                    }
                } else if (message.type == MessageType::kClientResize) {
                    winsize ws{};
                    ws.ws_row = static_cast<unsigned short>(message.arg0);
                    ws.ws_col = static_cast<unsigned short>(message.arg1);
                    ws = normalize_winsize(ws);
                    server.clients[client_index].size = ws;
                    server.clients[client_index].has_size = true;
                    const int session_id = server.clients[client_index].attached_session_id;
                    if (session_id >= 0) {
                        refresh_session_size_from_clients(server, session_id);
                        if (ManagedSession *managed = find_managed_session(server, session_id); managed != nullptr) {
                            apply_session_layout(managed->state);
                        }
                        broadcast_redraw(server, session_id);
                    }
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
                        const int session_id = server.clients[client_index].attached_session_id;
                        if (session_id >= 0) {
                            broadcast_redraw(server, session_id);
                        }
                    }
                }
            }

            if (!keep_client) {
                remove_client(server, client_index);
            }
        }

        for (size_t session_index = 0; session_index < server.sessions.size();) {
            ManagedSession &managed = server.sessions[session_index];
            for (PaneSlot &slot : managed.state.panes) {
                if (slot.pane.child_reaped && (slot.pane.master_fd < 0 || slot.pane.master_closed) &&
                    slot.state != PaneState::kDestroyed) {
                    slot.state = PaneState::kDestroyed;
                }
            }
            maybe_repair_focus(managed.state);
            apply_session_layout(managed.state);
            ++session_index;
        }

        for (const auto &entry : redraw_before_poll) {
            if (const ManagedSession *managed = find_managed_session(server, entry.first); managed != nullptr) {
                if (build_redraw_summary(managed->state) != entry.second) {
                    broadcast_redraw(server, entry.first);
                }
            }
        }

        for (size_t session_index = 0; session_index < server.sessions.size();) {
            ManagedSession &managed = server.sessions[session_index];
            if (!session_ready_for_removal(managed)) {
                ++session_index;
                continue;
            }
            const bool last_session = server.sessions.size() == 1;
            if (!last_session) {
                notify_session_exit(server, managed.session_id, managed.exit_code, managed.exit_signal);
            }
            cleanup_managed_session(managed);
            server.sessions.erase(server.sessions.begin() + static_cast<std::ptrdiff_t>(session_index));
        }

        if (server.sessions.empty()) {
            const auto now = std::chrono::steady_clock::now();
            if (!server.shutdown_pending) {
                server.shutdown_pending = true;
                server.shutdown_notice_deadline = now + std::chrono::milliseconds(3000);
                server.shutdown_deadline = now + std::chrono::milliseconds(3000);
                server.exit_notified = false;
            }
            if (!server.exit_notified && now >= server.shutdown_notice_deadline) {
                notify_server_exit(server);
                server.exit_notified = true;
            }
            if (now >= server.shutdown_deadline) {
                if (!server.exit_notified) {
                    notify_server_exit(server);
                    server.exit_notified = true;
                }
                usleep(100000);
                server.should_exit = true;
            }
        } else {
            server.shutdown_pending = false;
            server.exit_notified = false;
        }
    }

    for (ClientConnection &client : server.clients) {
        close_fd_if_valid(client.fd);
    }
    for (ManagedSession &managed : server.sessions) {
        cleanup_managed_session(managed);
    }
    close_fd_if_valid(server.sigchld_read_fd);
    close_fd_if_valid(server.sigchld_write_fd);
    close_fd_if_valid(server.listen_fd);
    unlink(socket_path.c_str());
    return 0;
}

int client_process(int socket_fd, bool read_only, int session_request) {
    ignore_sigpipe();

    if (!send_client_hello(socket_fd, read_only, session_request)) {
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
    const bool stdin_is_tty = isatty(STDIN_FILENO);
    set_nonblock(STDOUT_FILENO);

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
    std::string prefix_sequence;
    ClientViewState view_state{};
    bool alternate_screen_active = false;
    bool attached_to_session = false;

    auto ensure_render_surface = [&]() -> bool {
        if (!attached_to_session) {
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
    auto render_attached_view = [&]() -> bool {
        if (!attached_to_session) {
            return true;
        }
        if (!ensure_render_surface()) {
            return false;
        }
        return render_client_view(view_state, command_buffer, input_mode);
    };
    auto show_client_status = [&](const std::string &text) -> bool {
        view_state.local_status = text;
        if (attached_to_session) {
            return render_attached_view();
        }
        return show_local_status(text);
    };
    const std::string arrow_prefix = std::string(1, kEscapeKey) + "[";
    const std::string arrow_up = arrow_prefix + "A";
    const std::string arrow_down = arrow_prefix + "B";
    const std::string arrow_right = arrow_prefix + "C";
    const std::string arrow_left = arrow_prefix + "D";
    auto reset_prefix_mode = [&]() {
        input_mode = ClientInputMode::kNormal;
        prefix_sequence.clear();
    };
    auto run_prefix_focus_command = [&](const std::string &command) -> bool {
        if (!read_only) {
            if (!send_message(socket_fd, MessageType::kClientCommand, 0, 0,
                              command.data(), command.size())) {
                return false;
            }
        } else if (!show_client_status("read-only clients cannot switch focus")) {
            return false;
        }
        reset_prefix_mode();
        return true;
    };
    auto enter_command_mode = [&]() -> bool {
        input_mode = ClientInputMode::kCommand;
        prefix_sequence.clear();
        command_buffer.clear();
        if (attached_to_session) {
            return render_attached_view();
        }
        return begin_command_prompt();
    };

    int exit_code = 0;
    bool done = false;
    bool detached_by_user = false;
    bool received_server_exit = false;
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
            if (!render_attached_view()) {
                done = true;
            }
        }

        if (pfds[0].revents & POLLIN) {
            char buffer[4096];
            const ssize_t read_rc = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (read_rc > 0) {
                std::string pending_input;
                auto flush_pending_input = [&]() -> bool {
                    if (pending_input.empty()) {
                        return true;
                    }
                    if (!send_message(socket_fd, MessageType::kClientInput, 0, 0,
                                      pending_input.data(), pending_input.size())) {
                        return false;
                    }
                    pending_input.clear();
                    return true;
                };

                for (ssize_t i = 0; i < read_rc && !done; ++i) {
                    const char ch = buffer[i];
                    if (input_mode == ClientInputMode::kCommand) {
                        if (!flush_pending_input()) {
                            done = true;
                            break;
                        }
                        if (ch == '\r' || ch == '\n') {
                            if (!attached_to_session && !clear_command_prompt(command_buffer)) {
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
                            if (!attached_to_session && !clear_command_prompt(command_buffer)) {
                                done = true;
                                break;
                            }
                            command_buffer.clear();
                            input_mode = ClientInputMode::kNormal;
                        } else if (ch == kBackspaceKey || ch == '\b') {
                            if (!command_buffer.empty()) {
                                command_buffer.pop_back();
                                if (attached_to_session) {
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
                            if (attached_to_session) {
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
                        if (!flush_pending_input()) {
                            done = true;
                            break;
                        }
                        if (!prefix_sequence.empty() || ch == kEscapeKey) {
                            prefix_sequence.push_back(ch);
                            if (prefix_sequence == std::string(1, kEscapeKey) ||
                                prefix_sequence == arrow_prefix) {
                                continue;
                            }

                            std::string command;
                            if (prefix_sequence == arrow_up || prefix_sequence == arrow_left) {
                                command = "prev";
                            } else if (prefix_sequence == arrow_down || prefix_sequence == arrow_right) {
                                command = "next";
                            }

                            if (!command.empty()) {
                                if (!run_prefix_focus_command(command)) {
                                    done = true;
                                    break;
                                }
                            } else {
                                reset_prefix_mode();
                                if (!show_client_status(read_only ?
                                                        "read-only: unsupported prefix command" :
                                                        "unsupported prefix command")) {
                                    done = true;
                                    break;
                                }
                            }
                            continue;
                        }

                        if (ch == ':') {
                            if (!enter_command_mode()) {
                                done = true;
                                break;
                            }
                        } else if (ch == 'd') {
                            reset_prefix_mode();
                            detached_by_user = true;
                            done = true;
                        } else if (ch == 'n' || ch == 'p') {
                            if (!run_prefix_focus_command(ch == 'n' ? "next" : "prev")) {
                                done = true;
                                break;
                            }
                        } else if (ch == kPrefixKey) {
                            // Treat a repeated prefix as restarting the prefix sequence instead of
                            // forwarding Ctrl+B into the pane. This avoids leaking raw arrow-key
                            // escape sequences when the client is still in prefix mode.
                            prefix_sequence.clear();
                        } else {
                            reset_prefix_mode();
                            if (!show_client_status(read_only ?
                                                    "read-only: unsupported prefix command" :
                                                    "unsupported prefix command")) {
                                done = true;
                                break;
                            }
                        }
                        continue;
                    }

                    if (pending_input.empty() && ch == ':') {
                        if (!enter_command_mode()) {
                            done = true;
                            break;
                        }
                        continue;
                    }

                    if (ch == kPrefixKey) {
                        if (!flush_pending_input()) {
                            done = true;
                            break;
                        }
                        input_mode = ClientInputMode::kPrefix;
                        prefix_sequence.clear();
                        continue;
                    }

                    if (!read_only) {
                        if (ch == 0x03 || ch == 0x1a) {
                            if (!flush_pending_input() ||
                                !send_message(socket_fd, MessageType::kClientInput, 0, 0, &ch, 1)) {
                                done = true;
                                break;
                            }
                            continue;
                        }
                        pending_input.push_back(ch);
                    }
                }

                if (!done && !flush_pending_input()) {
                    done = true;
                }
            } else if (read_rc == 0) {
                if (!stdin_is_tty) {
                    done = true;
                }
            } else if (errno != EINTR && errno != EAGAIN) {
                if (errno == EIO && stdin_is_tty) {
                    continue;
                }
                done = true;
            }
        }

        if (pfds[1].revents & POLLIN) {
            Message message;
            if (!recv_message(socket_fd, message)) {
                done = true;
            } else if (message.type == MessageType::kServerOutput ||
                       message.type == MessageType::kServerStatus) {
                const std::string chunk(message.payload.begin(), message.payload.end());
                if (message.type == MessageType::kServerStatus) {
                    if (!show_client_status(chunk)) {
                        done = true;
                    }
                    if (!attached_to_session && chunk.rfind("error:", 0) == 0) {
                        exit_code = 1;
                        done = true;
                    }
                } else {
                    view_state.local_status.clear();
                    append_client_output(view_state, message.arg0, chunk);
                    if (attached_to_session && !render_attached_view()) {
                        done = true;
                    }
                }
            } else if (message.type == MessageType::kServerRedraw) {
                const std::string redraw(message.payload.begin(), message.payload.end());
                if (!parse_redraw_payload(redraw, view_state)) {
                    done = true;
                } else {
                    attached_to_session = true;
                    if (view_state.focused_pane_id < 0) {
                        view_state.focused_pane_id = message.arg0;
                    }
                    if (!render_attached_view()) {
                        done = true;
                    }
                }
            } else if (message.type == MessageType::kServerExit) {
                received_server_exit = true;
                exit_code = message.arg0;
                done = true;
            }
        } else if (pfds[1].revents & (POLLHUP | POLLERR)) {
            if (!detached_by_user && !received_server_exit && exit_code == 0) {
                exit_code = 1;
            }
            done = true;
        }
    }

    if (alternate_screen_active) {
        leave_alternate_screen();
    }

    terminal_guard.restore();
    write_stdout("[?25h");
    if (detached_by_user) {
        show_local_status("mini-tmux: detached");
    }
    close(sig_pipe[0]);
    close(sig_pipe[1]);
    if (detached_by_user) {
        return 0;
    }
    if (!received_server_exit && exit_code == 0) {
        return 1;
    }
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

bool should_run_server_only() {
    return !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO);
}

int start_server_only(const std::string &socket_path, const winsize &initial_size) {
    redirect_stdio_to_devnull();
    return server_process(socket_path, initial_size);
}

int start_server_and_attach(const char *argv0,
                            const std::string &socket_path,
                            bool read_only,
                            int session_request) {
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

        const std::string rows = std::to_string(initial_size.ws_row);
        const std::string cols = std::to_string(initial_size.ws_col);
        execl(argv0, argv0, "--server", rows.c_str(), cols.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }

    if (!wait_for_server(socket_path, 800, 10000)) {
        std::cerr << "failed to start server at " << socket_path << "\n";
        return 1;
    }

    const int socket_fd = connect_to_server(socket_path);
    if (socket_fd < 0) {
        perr("connect(server)");
        return 1;
    }
    const int rc = client_process(socket_fd, read_only, session_request);
    close(socket_fd);
    return rc;
}

int attach_to_existing_server(const std::string &socket_path, bool read_only, int session_request) {
    const int socket_fd = connect_to_server(socket_path);
    if (socket_fd < 0) {
        perr("attach");
        return 1;
    }
    const int rc = client_process(socket_fd, read_only, session_request);
    close(socket_fd);
    return rc;
}

bool receive_status_response(int socket_fd, std::string &payload) {
    while (true) {
        Message message;
        if (!recv_message(socket_fd, message)) {
            return false;
        }
        if (message.type == MessageType::kServerStatus) {
            payload.assign(message.payload.begin(), message.payload.end());
            return true;
        }
    }
}

int list_sessions(const std::string &socket_path) {
    const int socket_fd = connect_to_server(socket_path);
    if (socket_fd < 0) {
        return 0;
    }
    if (!send_message(socket_fd, MessageType::kClientListSessions, 0, 0, nullptr, 0)) {
        close(socket_fd);
        return 1;
    }
    std::string payload;
    const bool ok = receive_status_response(socket_fd, payload);
    close(socket_fd);
    if (!ok) {
        return 1;
    }
    if (!payload.empty() && !write_stdout(payload)) {
        return 1;
    }
    return 0;
}

int kill_session_on_server(const std::string &socket_path, int session_id) {
    const int socket_fd = connect_to_server(socket_path);
    if (socket_fd < 0) {
        perr("kill-session");
        return 1;
    }
    if (!send_message(socket_fd, MessageType::kClientKillSession, session_id, 0, nullptr, 0)) {
        close(socket_fd);
        return 1;
    }
    std::string payload;
    const bool ok = receive_status_response(socket_fd, payload);
    close(socket_fd);
    if (!ok) {
        return 1;
    }
    return payload.rfind("ok:", 0) == 0 ? 0 : 1;
}

bool parse_session_id_arg(const std::string &arg, int &session_id) {
    return parse_single_non_negative_int_argument(arg, session_id);
}

bool parse_server_size_args(const char *rows_arg, const char *cols_arg, winsize &size) {
    int rows = 0;
    int cols = 0;
    if (!parse_single_non_negative_int_argument(rows_arg, rows) ||
        !parse_single_non_negative_int_argument(cols_arg, cols)) {
        return false;
    }
    size.ws_row = static_cast<unsigned short>(rows);
    size.ws_col = static_cast<unsigned short>(cols);
    size = normalize_winsize(size);
    return true;
}

void print_usage(const char *argv0) {
    std::cerr << "Usage: " << argv0
              << " [--server <rows> <cols> | attach [-r] [session_id] | ls | kill-session <session_id>]\n";
}

}  // namespace

int main(int argc, char **argv) {
    const std::string socket_path = get_socket_path();

    if (argc == 4 && std::string(argv[1]) == "--server") {
        winsize initial_size{};
        if (!parse_server_size_args(argv[2], argv[3], initial_size)) {
            print_usage(argv[0]);
            return 1;
        }
        return start_server_only(socket_path, initial_size);
    }

    if (argc == 1) {
        const bool server_only = should_run_server_only();
        const int existing_fd = connect_to_server(socket_path);
        if (existing_fd >= 0) {
            close(existing_fd);
            return server_only ? 0 : attach_to_existing_server(socket_path, false, kCreateNewSession);
        }
        return server_only ? start_server_only(socket_path, get_winsize_from_fd(STDIN_FILENO))
                           : start_server_and_attach(argv[0], socket_path, false, 0);
    }

    if (argc == 2 && std::string(argv[1]) == "ls") {
        return list_sessions(socket_path);
    }

    if (argc == 2 && std::string(argv[1]) == "attach") {
        return attach_to_existing_server(socket_path, false, kAttachDefaultSession);
    }

    if (argc == 3 && std::string(argv[1]) == "attach" && std::string(argv[2]) == "-r") {
        return attach_to_existing_server(socket_path, true, kAttachDefaultSession);
    }

    if (argc == 3 && std::string(argv[1]) == "attach") {
        int session_id = -1;
        if (!parse_session_id_arg(argv[2], session_id)) {
            print_usage(argv[0]);
            return 1;
        }
        return attach_to_existing_server(socket_path, false, session_id);
    }

    if (argc == 4 && std::string(argv[1]) == "attach" && std::string(argv[2]) == "-r") {
        int session_id = -1;
        if (!parse_session_id_arg(argv[3], session_id)) {
            print_usage(argv[0]);
            return 1;
        }
        return attach_to_existing_server(socket_path, true, session_id);
    }

    if (argc == 3 && std::string(argv[1]) == "kill-session") {
        int session_id = -1;
        if (!parse_session_id_arg(argv[2], session_id)) {
            print_usage(argv[0]);
            return 1;
        }
        return kill_session_on_server(socket_path, session_id);
    }

    print_usage(argv[0]);
    return 1;
}


