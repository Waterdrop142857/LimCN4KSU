// service/ipc_server.h
// Responsibility: the Unix-domain socket the WebUI reaches through bybusctl.
// One request line in, one response line out, bounded frames, a hard connection
// cap and per-request timeouts. No protocol knowledge beyond framing.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "service/commands.h"
#include "service/diagnostics.h"
#include "service/paths.h"
#include "shared/status.h"

namespace bybus::service {

class IpcServer {
public:
    IpcServer(const ServicePaths &paths, CommandService *commands, Logger *log);
    ~IpcServer();
    IpcServer(const IpcServer &) = delete;
    IpcServer &operator=(const IpcServer &) = delete;

    shared::Code listen_socket();
    // Blocks until stop() is called or a `shutdown` request arrives.
    void serve();
    void stop();
    void close_socket();

private:
    void handle_connection(int client);
    std::string dispatch_line(const std::string &line);

    ServicePaths paths_;
    CommandService *commands_;
    Logger *log_;
    int socket_ = -1;
    std::atomic<bool> stopping_{false};
    std::atomic<uint32_t> connections_{0};
    std::vector<std::thread> threads_;
};

} // namespace bybus::service
