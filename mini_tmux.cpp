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

struct OutputBacklog {
    std::vector<char> data;

    void append(const char *chunk, size_t size) {
        if (size == 0) {
            return;
        }
        constexpr size_t kMaxBacklogBytes = 1 << 20;
        if (data.size() + size > kMaxBacklogBytes) {
            const size_t keep = kMaxBacklogBytes > size ? (kMaxBacklogBytes - size) : 0;
            if (keep == 0) {
                data.clear();
            } else if (data.size() > keep) {
                const size_t drop = data.size() - keep;
                data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(drop));
            }
        }
        data.insert(data.end(), chunk, chunk + size);
        if (data.size() > kMaxBacklogBytes) {
            const size_t drop = data.size() - kMaxBacklogBytes;
            data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(drop));
        }
    }

    bool send_to_client(int fd) const {
        size_t offset = 0;
        while (offset < data.size()) {
            const size_t chunk = std::min(kMaxPayload, data.size() - offset);
            if (!send_message(fd, MessageType::kServerOutput, 0, 0, data.data() + offset, chunk)) {
                return false;
            }
            offset += chunk;
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

struct SessionState {
    Pane pane{};
    PaneState pane_state = PaneState::kCreated;
    OutputBacklog backlog;
};

struct ServerState {
    std::string socket_path;
    int listen_fd = -1;
    int sigchld_read_fd = -1;
    int sigchld_write_fd = -1;
    bool should_exit = false;
    bool exit_notified = false;
    SessionState session{};
    std::vector<ClientConnection> clients;
};

enum class ClientInputMode : uint32_t {
    kNormal = 1,
    kPrefix = 2,
    kCommand = 3,
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

bool redraw_command_prompt(const std::string &buffer) {
    const std::string prompt = "\r\n:" + buffer;
    return write_stdout(prompt);
}

bool clear_command_prompt(const std::string &buffer) {
    std::string clear = "\r";
    clear.append(buffer.size() + 2, ' ');
    clear += "\r";
    return write_stdout(clear);
}

bool show_local_status(const std::string &text) {
    return write_stdout("\r\n" + text + "\r\n");
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

std::string handle_client_command(const ClientConnection &client, const std::string &raw_command) {
    std::string command = trim_ascii_whitespace(raw_command);
    if (!command.empty() && command.front() == ':') {
        command.erase(command.begin());
        command = trim_ascii_whitespace(command);
    }
    if (command.empty()) {
        return format_error("empty command");
    }

    const std::vector<std::string> tokens = split_ascii_whitespace(command);
    if (tokens.empty()) {
        return format_error("empty command");
    }

    const std::string &name = tokens[0];
    if (name == "new") {
        if (tokens.size() != 1) {
            return format_usage(":new");
        }
        if (client.read_only) {
            return format_error("read-only clients cannot run :new");
        }
        return format_ok(":new parsed; pane creation not implemented yet");
    }

    if (name == "kill") {
        if (tokens.size() != 2) {
            return format_usage(":kill <pane_id>");
        }
        if (client.read_only) {
            return format_error("read-only clients cannot run :kill");
        }

        int pane_id = -1;
        if (!parse_non_negative_int(tokens[1], pane_id)) {
            return format_error("pane_id must be a non-negative integer");
        }
        if (pane_id != 0) {
            return format_error("pane " + std::to_string(pane_id) + " does not exist");
        }
        return format_ok(":kill 0 parsed; pane removal not implemented yet");
    }

    if (name == "focus") {
        if (tokens.size() != 2) {
            return format_usage(":focus <pane_id>");
        }

        int pane_id = -1;
        if (!parse_non_negative_int(tokens[1], pane_id)) {
            return format_error("pane_id must be a non-negative integer");
        }
        if (pane_id != 0) {
            return format_error("pane " + std::to_string(pane_id) + " does not exist");
        }
        return format_ok("pane 0 already selected");
    }

    return format_error("unknown command '" + name + "'");
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

void broadcast_output(ServerState &server, const char *data, size_t size) {
    server.session.backlog.append(data, size);
    for (size_t i = 0; i < server.clients.size();) {
        if (!send_message(server.clients[i].fd, MessageType::kServerOutput, 0, 0, data, size)) {
            remove_client(server.clients, i);
            continue;
        }
        ++i;
    }
}

void notify_pane_exit(ServerState &server, int exit_code, int exit_signal) {
    for (size_t i = 0; i < server.clients.size();) {
        if (!send_message(server.clients[i].fd, MessageType::kServerExit, exit_code, exit_signal, nullptr, 0)) {
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

    if (!spawn_shell_pane(server.session.pane, initial_size)) {
        perr("spawn pane");
        return 1;
    }
    server.session.pane_state = PaneState::kRunning;

    server.listen_fd = create_listen_socket(socket_path);
    if (server.listen_fd < 0) {
        perr("listen socket");
        kill(server.session.pane.child_pid, SIGHUP);
        waitpid(server.session.pane.child_pid, nullptr, 0);
        close_fd_if_valid(server.session.pane.master_fd);
        return 1;
    }

    while (!server.should_exit) {
        std::vector<pollfd> pfds;
        pfds.push_back({server.listen_fd, POLLIN, 0});
        pfds.push_back({server.sigchld_read_fd, POLLIN, 0});
        if (server.session.pane.master_fd >= 0) {
            pfds.push_back({server.session.pane.master_fd, static_cast<short>(POLLIN | POLLHUP | POLLERR), 0});
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
                if (pid == server.session.pane.child_pid) {
                    server.session.pane.child_reaped = true;
                    server.session.pane_state = PaneState::kReaped;
                    if (WIFEXITED(status)) {
                        server.session.pane.exit_code = WEXITSTATUS(status);
                        server.session.pane.exit_signal = 0;
                    } else if (WIFSIGNALED(status)) {
                        server.session.pane.exit_code = 128 + WTERMSIG(status);
                        server.session.pane.exit_signal = WTERMSIG(status);
                    }
                }
            }
        }
        ++idx;

        if (server.session.pane.master_fd >= 0) {
            if (pfds[idx].revents & (POLLIN | POLLHUP | POLLERR)) {
                char buffer[4096];
                const ssize_t read_rc = read(server.session.pane.master_fd, buffer, sizeof(buffer));
                if (read_rc > 0) {
                    broadcast_output(server, buffer, static_cast<size_t>(read_rc));
                } else if (read_rc == 0 || (read_rc < 0 && errno != EINTR && errno != EAGAIN)) {
                    close_fd_if_valid(server.session.pane.master_fd);
                    server.session.pane.master_closed = true;
                    if (server.session.pane_state == PaneState::kRunning) {
                        server.session.pane_state = PaneState::kExited;
                    }
                }
            }
            ++idx;
        }

        for (size_t client_index = 0; client_index < server.clients.size();) {
            const short revents = pfds[idx].revents;
            bool keep_client = true;
            if (revents & (POLLHUP | POLLERR)) {
                keep_client = false;
            } else if (revents & POLLIN) {
                Message message;
                if (!recv_message(server.clients[client_index].fd, message)) {
                    keep_client = false;
                } else if (message.type == MessageType::kClientInput) {
                    if (!server.clients[client_index].read_only &&
                        server.session.pane.master_fd >= 0 && !message.payload.empty()) {
                        if (!write_all(server.session.pane.master_fd, message.payload.data(), message.payload.size())) {
                            close_fd_if_valid(server.session.pane.master_fd);
                            server.session.pane.master_closed = true;
                            if (server.session.pane_state == PaneState::kRunning) {
                                server.session.pane_state = PaneState::kExited;
                            }
                        }
                    }
                } else if (message.type == MessageType::kClientResize) {
                    if (server.session.pane.master_fd >= 0) {
                        winsize ws{};
                        ws.ws_row = static_cast<unsigned short>(message.arg0);
                        ws.ws_col = static_cast<unsigned short>(message.arg1);
                        ws = normalize_winsize(ws);
                        server.session.pane.size = ws;
                        apply_winsize(server.session.pane.master_fd, ws);
                    }
                } else if (message.type == MessageType::kClientCommand) {
                    const std::string raw_command(message.payload.begin(), message.payload.end());
                    const std::string status = handle_client_command(server.clients[client_index], raw_command);
                    if (!send_message(server.clients[client_index].fd, MessageType::kServerStatus, 0, 0,
                                      status.data(), status.size())) {
                        keep_client = false;
                    }
                }
            }

            if (!keep_client) {
                remove_client(server.clients, client_index);
                ++idx;
                continue;
            }
            ++client_index;
            ++idx;
        }

        if (server.session.pane.child_reaped &&
            (server.session.pane.master_fd < 0 || server.session.pane.master_closed)) {
            if (!server.exit_notified) {
                notify_pane_exit(server, server.session.pane.exit_code, server.session.pane.exit_signal);
                server.exit_notified = true;
            }
            server.should_exit = true;
        }
    }

    for (ClientConnection &client : server.clients) {
        close_fd_if_valid(client.fd);
    }
    close_fd_if_valid(server.session.pane.master_fd);
    close_fd_if_valid(server.sigchld_read_fd);
    close_fd_if_valid(server.sigchld_write_fd);
    close_fd_if_valid(server.listen_fd);
    server.session.pane_state = PaneState::kDestroyed;
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
            if (input_mode == ClientInputMode::kCommand && !redraw_command_prompt(command_buffer)) {
                done = true;
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
                                if (!clear_command_prompt(command_buffer + " ") ||
                                    !redraw_command_prompt(command_buffer)) {
                                    done = true;
                                    break;
                                }
                            }
                        } else if (ch >= 32 && ch <= 126) {
                            command_buffer.push_back(ch);
                            if (!redraw_command_prompt(command_buffer)) {
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
                            if (!redraw_command_prompt(command_buffer)) {
                                done = true;
                                break;
                            }
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
                if (!message.payload.empty() &&
                    !write_all(STDOUT_FILENO, message.payload.data(), message.payload.size())) {
                    done = true;
                }
                if (!done && input_mode == ClientInputMode::kCommand &&
                    !redraw_command_prompt(command_buffer)) {
                    done = true;
                }
            } else if (message.type == MessageType::kServerExit) {
                exit_code = message.arg0;
                done = true;
            }
        }
    }

    terminal_guard.restore();
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
