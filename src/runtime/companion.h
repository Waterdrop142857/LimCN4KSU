// Unix-domain companion channel between the game process and the root companion
// registered through Zygisk. The game sandbox cannot read /data/adb/bybus, so
// every snapshot handle is opened by the companion and passed as a file
// descriptor together with SCM_RIGHTS.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace bybus::runtime {

enum class CompanionRequest : uint32_t {
    Hello = 1,    // -> manifest fd + font fd (0 when the generation has no font)
    Table = 2,    // payload: manifest table path ("tables/<sha256>.json")
    Receipt = 3,  // payload: receipt JSON to persist
    Bye = 4,
};

enum class CompanionStatus : uint32_t {
    Ok = 0,
    BadRequest = 1,
    NotFound = 2,
    Rejected = 3,
    IoError = 4,
};

struct HelloInfo {
    std::string generation;
    bool degraded = false;  // explicit manual marker for this generation
    int manifest_fd = -1;
    int font_fd = -1;
    int index_fd = -1;
};

// Owns one connected companion socket for the lifetime of the game process.
class CompanionClient {
public:
    CompanionClient() = default;
    ~CompanionClient();
    CompanionClient(const CompanionClient &) = delete;
    CompanionClient &operator=(const CompanionClient &) = delete;

    bool connect(int socket_fd);
    bool hello(HelloInfo *info);
    // Returns a read-only descriptor for a prepared table, or -1.
    int open_table(const std::string &relative_path);
    bool send_receipt(const std::string &json);
    void close();

private:
    bool send_request(CompanionRequest request, const std::string &payload, int fd);
    bool receive_response(CompanionStatus *status, std::string *payload, int *fd);

    int socket_ = -1;
    std::mutex request_mutex_;
};

// Companion side: serves one accepted client socket. Runs in the root companion
// process spawned by Zygisk.
void serve_companion(int client_socket);

} // namespace bybus::runtime
