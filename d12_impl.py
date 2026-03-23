from pathlib import Path
path = Path('/home/kaiyi/lab1-student/mini_tmux.cpp')
text = path.read_text(encoding='utf-8')
text = text.replace('#include <cstdio>\n', '#include <cstdio>\n#include <cctype>\n', 1)
old = '''bool show_local_status(const std::string &text) {
    return write_stdout("\r\n" + text + "\r\n");
}

bool send_client_hello(int socket_fd, bool read_only) {
'''
new = r'''bool show_local_status(const std::string &text) {
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

std::string handle_client_command(const ClientConnection &client, const std::string &raw_command) {
    std::string command = trim_ascii_whitespace(raw_command);
    if (!command.empty() && command.front() == ':') {
        command.erase(command.begin());
        command = trim_ascii_whitespace(command);
    }
    if (command.empty()) {
        return "error: empty command";
    }

    const std::vector<std::string> tokens = split_ascii_whitespace(command);
    if (tokens.empty()) {
        return "error: empty command";
    }

    const std::string &name = tokens[0];
    if (name == "new") {
        if (tokens.size() != 1) {
            return "usage: :new";
        }
        if (client.read_only) {
            return "error: read-only clients cannot run :new";
        }
        return "new not implemented yet";
    }

    if (name == "kill") {
        if (tokens.size() != 2) {
            return "usage: :kill <pane_id>";
        }
        if (client.read_only) {
            return "error: read-only clients cannot run :kill";
        }

        int pane_id = -1;
        if (!parse_non_negative_int(tokens[1], pane_id)) {
            return "error: pane_id must be a non-negative integer";
        }
        if (pane_id != 0) {
            return "error: pane " + std::to_string(pane_id) + " does not exist";
        }
        return "kill not implemented yet for pane 0";
    }

    if (name == "focus") {
        if (tokens.size() != 2) {
            return "usage: :focus <pane_id>";
        }

        int pane_id = -1;
        if (!parse_non_negative_int(tokens[1], pane_id)) {
            return "error: pane_id must be a non-negative integer";
        }
        if (pane_id != 0) {
            return "error: pane " + std::to_string(pane_id) + " does not exist";
        }
        return "focus: pane 0 already selected";
    }

    return "error: unknown command '" + name + "'";
}

bool send_client_hello(int socket_fd, bool read_only) {
'''
if old not in text:
    raise SystemExit('helper insertion anchor not found')
text = text.replace(old, new, 1)
old = '''                } else if (message.type == MessageType::kClientCommand) {
                    std::string status;
                    if (message.payload.empty()) {
                        status = "empty command";
                    } else {
                        status.assign(message.payload.begin(), message.payload.end());
                        status = "command not implemented yet: " + status;
                    }
                    if (!send_message(server.clients[client_index].fd, MessageType::kServerStatus, 0, 0,
                                      status.data(), status.size())) {
                        keep_client = false;
                    }
                }
'''
new = r'''                } else if (message.type == MessageType::kClientCommand) {
                    const std::string raw_command(message.payload.begin(), message.payload.end());
                    const std::string status = handle_client_command(server.clients[client_index], raw_command);
                    if (!send_message(server.clients[client_index].fd, MessageType::kServerStatus, 0, 0,
                                      status.data(), status.size())) {
                        keep_client = false;
                    }
                }
'''
if old not in text:
    raise SystemExit('command handler anchor not found')
text = text.replace(old, new, 1)
path.write_text(text, encoding='utf-8')
