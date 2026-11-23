#include "service/ipc_server.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "shared/fs.h"

namespace bybus::service {
namespace {

constexpr size_t kReadChunk = 16 * 1024;

bool write_all(int fd, const std::string &data) {
    size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

} // namespace

IpcServer::IpcServer(const ServicePaths &paths, CommandService *commands, Logger *log)
    : paths_(paths), commands_(commands), log_(log) {}

IpcServer::~IpcServer() {
    stop();
    close_socket();
    for (std::thread &thread : threads_) {
        if (thread.joinable()) thread.join();
    }
}

shared::Code IpcServer::listen_socket() {
    if (shared::make_dirs(paths_.state_root, 0700) != shared::Code::Ok) return shared::Code::IoError;
    ::unlink(paths_.socket().c_str());
    socket_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_ < 0) return shared::Code::IoError;
    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (paths_.socket().size() >= sizeof(address.sun_path)) {
        ::close(socket_);
        socket_ = -1;
        return shared::Code::InvalidArgument;
    }
    std::memcpy(address.sun_path, paths_.socket().c_str(), paths_.socket().size());
    if (::bind(socket_, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) != 0) {
        ::close(socket_);
        socket_ = -1;
        return shared::Code::IoError;
    }
    ::chmod(paths_.socket().c_str(), 0600);
    if (::listen(socket_, 8) != 0) {
        ::close(socket_);
        socket_ = -1;
        return shared::Code::IoError;
    }
    if (log_) log_->info("ipc listening");
    return shared::Code::Ok;
}

void IpcServer::close_socket() {
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
    ::unlink(paths_.socket().c_str());
}

void IpcServer::stop() {
    stopping_.store(true);
}

void IpcServer::serve() {
    while (!stopping_.load()) {
        struct pollfd pending {};
        pending.fd = socket_;
        pending.events = POLLIN;
        const int ready = ::poll(&pending, 1, 250);
        if (stopping_.load()) break;
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;
        if ((pending.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) break;
        if ((pending.revents & POLLIN) == 0) continue;
        const int client = ::accept4(socket_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) continue;
            if (errno == EBADF || errno == EINVAL) break;
            continue;
        }
        if (connections_.load() >= kMaxConnections) {
            ::close(client);
            continue;
        }
        connections_.fetch_add(1);
        threads_.emplace_back([this, client]() {
            handle_connection(client);
            connections_.fetch_sub(1);
        });
        // Reap finished connections opportunistically.
        if (threads_.size() > 16) {
            for (std::thread &thread : threads_) {
                if (thread.joinable()) thread.join();
            }
            threads_.clear();
        }
    }
    for (std::thread &thread : threads_) {
        if (thread.joinable()) thread.join();
    }
    threads_.clear();
}

std::string IpcServer::dispatch_line(const std::string &line) {
    Request request;
    shared::Code code = shared::Code::Ok;
    if (!parse_request(line, &request, &code)) {
        Response response;
        response.code = code;
        response.message = shared::code_message(code);
        response.data = shared::Json::object();
        return commands_->render(response);
    }
    Response response = commands_->handle(request);
    if (commands_->shutdown_requested()) stop();
    return commands_->render(response);
}

void IpcServer::handle_connection(int client) {
    struct timeval timeout {};
    timeout.tv_sec = kConnectionReadMillis / 1000;
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    std::string request;
    bool complete = false;
    while (request.size() <= kMaxRequestBytes) {
        char buffer[kReadChunk];
        const ssize_t read_size = ::recv(client, buffer, sizeof(buffer), 0);
        if (read_size < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (read_size == 0) break;
        request.append(buffer, static_cast<size_t>(read_size));
        if (request.find('\n') != std::string::npos) {
            complete = true;
            break;
        }
    }
    if (!complete) {
        Response response;
        response.code = shared::Code::InvalidArgument;
        response.message = shared::code_message(shared::Code::InvalidArgument);
        response.data = shared::Json::object();
        write_all(client, commands_->render(response) + "\n");
        ::close(client);
        return;
    }
    const size_t newline = request.find('\n');
    std::string line = request.substr(0, newline);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    std::string response = dispatch_line(line);
    if (response.size() > kMaxResponseBytes) {
        Response failure;
        failure.code = shared::Code::InternalError;
        failure.data = shared::Json::object();
        response = commands_->render(failure);
    }
    write_all(client, response + "\n");
    ::close(client);
}

} // namespace bybus::service
