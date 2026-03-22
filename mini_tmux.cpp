#include <cerrno>
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

winsize get_winsize_from_fd(int fd) {
    winsize ws{};
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        return ws;
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
    kClientInput = 1,
    kClientResize = 2,
    kServerOutput = 3,
    kServerExit = 4,
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

void close_fd_if_valid(int &fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
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

void broadcast_output(std::vector<ClientConnection> &clients, const char *data, size_t size) {
    for (size_t i = 0; i < clients.size();) {
        if (!send_message(clients[i].fd, MessageType::kServerOutput, 0, 0, data, size)) {
            remove_client(clients, i);
            continue;
        }
        ++i;
    }
}

void notify_pane_exit(std::vector<ClientConnection> &clients, int exit_code, int exit_signal) {
    for (size_t i = 0; i < clients.size();) {
        if (!send_message(clients[i].fd, MessageType::kServerExit, exit_code, exit_signal, nullptr, 0)) {
            remove_client(clients, i);
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

    int sig_pipe[2] = {-1, -1};
    if (pipe(sig_pipe) != 0) {
        perr("pipe(SIGCHLD)");
        return 1;
    }
    set_cloexec(sig_pipe[0]);
    set_cloexec(sig_pipe[1]);
    set_nonblock(sig_pipe[0]);
    set_nonblock(sig_pipe[1]);
    g_server_signal_fd = sig_pipe[1];

    struct sigaction sa {};
    sa.sa_handler = server_sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, nullptr) != 0) {
        perr("sigaction(SIGCHLD)");
        return 1;
    }

    Pane pane{};
    if (!spawn_shell_pane(pane, initial_size)) {
        perr("spawn pane");
        return 1;
    }

    const int listen_fd = create_listen_socket(socket_path);
    if (listen_fd < 0) {
        perr("listen socket");
        kill(pane.child_pid, SIGHUP);
        waitpid(pane.child_pid, nullptr, 0);
        close_fd_if_valid(pane.master_fd);
        return 1;
    }

    bool should_exit = false;
    bool exit_notified = false;
    std::vector<ClientConnection> clients;

    while (!should_exit) {
        std::vector<pollfd> pfds;
        pfds.push_back({listen_fd, POLLIN, 0});
        pfds.push_back({sig_pipe[0], POLLIN, 0});
        if (pane.master_fd >= 0) {
            pfds.push_back({pane.master_fd, static_cast<short>(POLLIN | POLLHUP | POLLERR), 0});
        }
        for (const ClientConnection &client : clients) {
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
            const int client_fd = accept(listen_fd, nullptr, nullptr);
            if (client_fd >= 0) {
                set_cloexec(client_fd);
                clients.push_back({client_fd});
            }
        }
        ++idx;

        if (pfds[idx].revents & POLLIN) {
            drain_fd(sig_pipe[0]);
            while (true) {
                int status = 0;
                const pid_t pid = waitpid(-1, &status, WNOHANG);
                if (pid <= 0) {
                    break;
                }
                if (pid == pane.child_pid) {
                    pane.child_reaped = true;
                    if (WIFEXITED(status)) {
                        pane.exit_code = WEXITSTATUS(status);
                        pane.exit_signal = 0;
                    } else if (WIFSIGNALED(status)) {
                        pane.exit_code = 128 + WTERMSIG(status);
                        pane.exit_signal = WTERMSIG(status);
                    }
                }
            }
        }
        ++idx;

        if (pane.master_fd >= 0) {
            if (pfds[idx].revents & (POLLIN | POLLHUP | POLLERR)) {
                char buffer[4096];
                const ssize_t read_rc = read(pane.master_fd, buffer, sizeof(buffer));
                if (read_rc > 0) {
                    broadcast_output(clients, buffer, static_cast<size_t>(read_rc));
                } else if (read_rc == 0 || (read_rc < 0 && errno != EINTR && errno != EAGAIN)) {
                    close_fd_if_valid(pane.master_fd);
                    pane.master_closed = true;
                }
            }
            ++idx;
        }

        for (size_t client_index = 0; client_index < clients.size();) {
            const short revents = pfds[idx].revents;
            bool keep_client = true;
            if (revents & (POLLHUP | POLLERR)) {
                keep_client = false;
            } else if (revents & POLLIN) {
                Message message;
                if (!recv_message(clients[client_index].fd, message)) {
                    keep_client = false;
                } else if (message.type == MessageType::kClientInput) {
                    if (pane.master_fd >= 0 && !message.payload.empty()) {
                        if (!write_all(pane.master_fd, message.payload.data(), message.payload.size())) {
                            close_fd_if_valid(pane.master_fd);
                            pane.master_closed = true;
                        }
                    }
                } else if (message.type == MessageType::kClientResize) {
                    if (pane.master_fd >= 0) {
                        winsize ws{};
                        ws.ws_row = static_cast<unsigned short>(message.arg0);
                        ws.ws_col = static_cast<unsigned short>(message.arg1);
                        if (ws.ws_row > 0 && ws.ws_col > 0) {
                            pane.size = ws;
                            apply_winsize(pane.master_fd, ws);
                        }
                    }
                }
            }

            if (!keep_client) {
                remove_client(clients, client_index);
                ++idx;
                continue;
            }
            ++client_index;
            ++idx;
        }

        if (pane.child_reaped && (pane.master_fd < 0 || pane.master_closed)) {
            if (!exit_notified) {
                notify_pane_exit(clients, pane.exit_code, pane.exit_signal);
                exit_notified = true;
            }
            should_exit = true;
        }
    }

    for (ClientConnection &client : clients) {
        close_fd_if_valid(client.fd);
    }
    close_fd_if_valid(pane.master_fd);
    close(sig_pipe[0]);
    close(sig_pipe[1]);
    close(listen_fd);
    unlink(socket_path.c_str());
    return 0;
}

int client_process(int socket_fd) {
    ignore_sigpipe();

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

    send_resize_to_server(socket_fd);

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
        }

        if (pfds[0].revents & POLLIN) {
            char buffer[4096];
            const ssize_t read_rc = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (read_rc > 0) {
                if (!send_message(socket_fd, MessageType::kClientInput, 0, 0, buffer,
                                  static_cast<size_t>(read_rc))) {
                    done = true;
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
            } else if (message.type == MessageType::kServerOutput) {
                if (!message.payload.empty() &&
                    !write_all(STDOUT_FILENO, message.payload.data(), message.payload.size())) {
                    done = true;
                }
            } else if (message.type == MessageType::kServerExit) {
                exit_code = message.arg0;
                done = true;
            }
        }
    }

    terminal_guard.restore();
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

int start_server_and_attach(const std::string &socket_path) {
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
    const int rc = client_process(socket_fd);
    close(socket_fd);
    return rc;
}

int attach_to_existing_server(const std::string &socket_path) {
    const int socket_fd = connect_to_server(socket_path);
    if (socket_fd < 0) {
        perr("attach");
        return 1;
    }
    const int rc = client_process(socket_fd);
    close(socket_fd);
    return rc;
}

void print_usage(const char *argv0) {
    std::cerr << "Usage: " << argv0 << " [attach]\n";
}

}  // namespace

int main(int argc, char **argv) {
    const std::string socket_path = get_socket_path();

    if (argc == 1) {
        const int existing_fd = connect_to_server(socket_path);
        if (existing_fd >= 0) {
            close(existing_fd);
            return attach_to_existing_server(socket_path);
        }
        return start_server_and_attach(socket_path);
    }

    if (argc == 2 && std::string(argv[1]) == "attach") {
        return attach_to_existing_server(socket_path);
    }

    print_usage(argv[0]);
    return 1;
}
