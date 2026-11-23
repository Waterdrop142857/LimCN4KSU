#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include "shared/json.h"
#include "shared/status.h"

namespace {

constexpr const char *kSocketPath = "/data/adb/bybus/bybus.sock";
constexpr size_t kMaxRequestBytes = 256 * 1024;
constexpr size_t kMaxResponseBytes = 4 * 1024 * 1024;

void emit_error(bybus::shared::Code code) {
    auto value = bybus::shared::Json::object();
    value->put_int("schema", 1);
    value->put_bool("ok", false);
    value->put_string("code", bybus::shared::code_text(code));
    value->put_string("message", bybus::shared::code_message(code));
    value->put("data", bybus::shared::Json::object());
    std::puts(value->dump().c_str());
}

bool send_all(int socket, const std::string &text) {
    size_t offset = 0;
    while (offset < text.size()) {
        const ssize_t sent = ::send(socket, text.data() + offset, text.size() - offset, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        offset += static_cast<size_t>(sent);
    }
    return true;
}

bool read_line(int socket, std::string *line) {
    line->clear();
    char bytes[8192];
    while (line->size() <= kMaxResponseBytes) {
        const ssize_t count = ::recv(socket, bytes, sizeof(bytes), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        for (ssize_t i = 0; i < count; ++i) {
            if (bytes[i] == '\n') return !line->empty() && line->size() <= kMaxResponseBytes;
            line->push_back(bytes[i]);
            if (line->size() > kMaxResponseBytes) return false;
        }
    }
    return false;
}

} // namespace

int main(int argc, char **argv) {
    std::string command;
    std::string args_text = "{}";
    int command_words = 0;
    bool valid = true;
    for (int index = 1; index < argc; ++index) {
        const std::string word(argv[index]);
        if (word == "--json") {
            if (index + 2 != argc) valid = false;
            else args_text = argv[index + 1];
            break;
        }
        if (word.empty() || ++command_words > 2) { valid = false; break; }
        for (const char c : word) {
            if (c < 'a' || c > 'z') valid = false;
        }
        if (!command.empty()) command.push_back(' ');
        command += word;
    }
    const auto parsed = bybus::shared::parse(args_text);
    if (command.empty() || !valid || !parsed.ok || !parsed.value ||
        !parsed.value->is_object()) {
        emit_error(bybus::shared::Code::Usage);
        return 1;
    }
    auto request = bybus::shared::Json::object();
    request->put_int("schema", 1);
    request->put_string("command", command);
    request->put("args", parsed.value);
    const std::string wire = request->dump() + "\n";
    if (wire.size() > kMaxRequestBytes) {
        emit_error(bybus::shared::Code::InvalidArgument);
        return 0;
    }
    const int socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket < 0) { emit_error(bybus::shared::Code::IoError); return 0; }
    struct timeval timeout {};
    timeout.tv_sec = command == "update start" || command == "snapshot build" ? 10 : 30;
    ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, kSocketPath, std::strlen(kSocketPath) + 1);
    const bool connected = ::connect(socket, reinterpret_cast<sockaddr *>(&address),
                                     sizeof(address)) == 0;
    std::string line;
    const bool received = connected && send_all(socket, wire) && read_line(socket, &line);
    ::close(socket);
    if (!received) { emit_error(bybus::shared::Code::IoError); return 0; }
    const auto response = bybus::shared::parse(line);
    if (!response.ok || !response.value || !response.value->is_object()) {
        emit_error(bybus::shared::Code::IoError);
        return 0;
    }
    std::puts(line.c_str());
    return 0;
}
