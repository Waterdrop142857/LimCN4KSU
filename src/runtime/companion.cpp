#include "runtime/companion.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "runtime/log.h"
#include "shared/fs.h"
#include "shared/json.h"
#include "shared/sha256.h"
#include "translation/snapshot.h"

namespace bybus::runtime {
namespace {

constexpr const char *kStateRoot = "/data/adb/bybus";
constexpr const char *kGenerationsDir = "/data/adb/bybus/generations";
constexpr uint32_t kMaxPayload = 1024 * 1024;
constexpr uint32_t kMaxFds = 3;
constexpr int kSocketTimeoutSeconds = 20;
constexpr uint64_t kMaxManifestBytes = UINT64_MAX;

struct MessageHeader {
    uint32_t code;
    uint32_t length;
    uint32_t fd_count;
    uint32_t reserved;
};

bool write_all(int fd, const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::send(fd, bytes + offset, size - offset, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

// Sends one framed message. Descriptors travel with the header through
// SCM_RIGHTS, so the header must be delivered with sendmsg, not send().
bool send_frame(int fd, uint32_t code, const std::string &payload, const int *fds,
                uint32_t fd_count) {
    if (fd_count > kMaxFds || payload.size() > kMaxPayload) return false;
    MessageHeader header{code, static_cast<uint32_t>(payload.size()), fd_count, 0};
    struct iovec iov {};
    iov.iov_base = &header;
    iov.iov_len = sizeof(header);
    char control[CMSG_SPACE(sizeof(int) * kMaxFds)] = {};
    struct msghdr message {};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    if (fd_count > 0) {
        message.msg_control = control;
        message.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
        std::memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * fd_count);
    }
    ssize_t written = 0;
    do {
        written = ::sendmsg(fd, &message, MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    if (written <= 0) return false;
    if (written < static_cast<ssize_t>(sizeof(header)) &&
        !write_all(fd, reinterpret_cast<const uint8_t *>(&header) + written,
                   sizeof(header) - static_cast<size_t>(written))) return false;
    if (!payload.empty() && !write_all(fd, payload.data(), payload.size())) return false;
    return true;
}

bool read_all(int fd, void *data, size_t size) {
    auto *bytes = static_cast<uint8_t *>(data);
    size_t offset = 0;
    while (offset < size) {
        const ssize_t read_size = ::recv(fd, bytes + offset, size - offset, 0);
        if (read_size < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (read_size == 0) return false;
        offset += static_cast<size_t>(read_size);
    }
    return true;
}

// Receives the header plus any descriptors, then the payload.
bool receive_frame(int fd, uint32_t *code, std::string *payload, std::vector<int> *fds) {
    fds->clear();
    const auto fail = [fds]() {
        for (const int received_fd : *fds) ::close(received_fd);
        fds->clear();
        return false;
    };
    MessageHeader header{};
    struct iovec iov {};
    iov.iov_base = &header;
    iov.iov_len = sizeof(header);
    char control[CMSG_SPACE(sizeof(int) * kMaxFds)] = {};
    struct msghdr message {};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    ssize_t received = 0;
    do {
        received = ::recvmsg(fd, &message, MSG_CMSG_CLOEXEC | MSG_WAITALL);
    } while (received < 0 && errno == EINTR);
    if (received <= 0) return false;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
        if (cmsg->cmsg_len < CMSG_LEN(0)) return fail();
        const size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const int *received_fds = reinterpret_cast<const int *>(CMSG_DATA(cmsg));
        for (size_t i = 0; i < count && i < kMaxFds; ++i) fds->push_back(received_fds[i]);
    }
    if (received != static_cast<ssize_t>(sizeof(header)) ||
        (message.msg_flags & MSG_CTRUNC) != 0) return fail();
    if (header.fd_count > kMaxFds || fds->size() != header.fd_count) return fail();
    payload->clear();
    if (header.length > kMaxPayload) return fail();
    if (header.length > 0) {
        payload->resize(header.length);
        if (!read_all(fd, &(*payload)[0], header.length)) return fail();
    }
    *code = header.code;
    return true;
}

std::string read_active_generation() {
    std::string active;
    if (shared::read_file(std::string(kStateRoot) + "/active", 128, &active) != shared::Code::Ok) {
        return std::string();
    }
    while (!active.empty() && (active.back() == '\n' || active.back() == '\r')) active.pop_back();
    return shared::is_sha256_hex(active) ? active : std::string();
}

void write_receipt(const std::string &json) {
    if (json.size() > 4096) return;
    const std::string root = std::string(kStateRoot) + "/runtime";
    if (shared::make_dirs(root, 0700) != shared::Code::Ok) return;
    const std::string temporary = root + "/.receipt.tmp";
    if (shared::write_file(temporary, json) != shared::Code::Ok) return;
    shared::atomic_replace(temporary, root + "/receipt.json");
}

std::string runtime_dir() { return std::string(kStateRoot) + "/runtime"; }

std::string degraded_generation() {
    std::string text;
    if (shared::read_file(runtime_dir() + "/degrade.json", 4096, &text) != shared::Code::Ok) {
        return std::string();
    }
    const shared::ParseOutcome parsed = shared::parse(text);
    if (!parsed.ok || !parsed.value || !parsed.value->is_object()) return std::string();
    const shared::JsonPtr generation = parsed.value->opt("generation");
    const shared::JsonPtr reason = parsed.value->opt("reason");
    // Older versions wrote {generation} after any short session, including a
    // normal exit. Only an explicit manual marker is authoritative now.
    if (!generation || !generation->is_string() ||
        !shared::is_sha256_hex(generation->as_string()) || !reason || !reason->is_string() ||
        reason->as_string() != "manual") return std::string();
    return generation->as_string();
}

} // namespace

CompanionClient::~CompanionClient() { close(); }

bool CompanionClient::connect(int socket_fd) {
    if (socket_fd < 0) return false;
    close();
    socket_ = socket_fd;
    struct timeval timeout {};
    timeout.tv_sec = kSocketTimeoutSeconds;
    ::setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return true;
}

void CompanionClient::close() {
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
}

bool CompanionClient::send_request(CompanionRequest request, const std::string &payload, int fd) {
    if (socket_ < 0) return false;
    const int fds[1] = {fd};
    return send_frame(socket_, static_cast<uint32_t>(request), payload, fd >= 0 ? fds : nullptr,
                      fd >= 0 ? 1 : 0);
}

bool CompanionClient::receive_response(CompanionStatus *status, std::string *payload, int *fd) {
    if (socket_ < 0) return false;
    uint32_t code = 0;
    std::vector<int> fds;
    if (!receive_frame(socket_, &code, payload, &fds)) {
        for (const int received : fds) ::close(received);
        return false;
    }
    *status = static_cast<CompanionStatus>(code);
    *fd = -1;
    if (!fds.empty()) {
        *fd = fds.front();
        for (size_t i = 1; i < fds.size(); ++i) ::close(fds[i]);
    }
    return true;
}

bool CompanionClient::hello(HelloInfo *info) {
    std::lock_guard<std::mutex> guard(request_mutex_);
    if (!info || socket_ < 0) return false;
    if (!send_request(CompanionRequest::Hello, std::string(), -1)) return false;
    uint32_t code = 0;
    std::string payload;
    std::vector<int> fds;
    if (!receive_frame(socket_, &code, &payload, &fds)) {
        for (const int received : fds) ::close(received);
        return false;
    }
    if (code != static_cast<uint32_t>(CompanionStatus::Ok)) {
        for (const int received : fds) ::close(received);
        return false;
    }
    if (fds.empty()) return false;
    const shared::ParseOutcome parsed = shared::parse(payload);
    if (!parsed.ok || !parsed.value || !parsed.value->is_object()) {
        for (const int received : fds) ::close(received);
        return false;
    }
    const shared::JsonPtr generation = parsed.value->opt("generation");
    if (!generation || !generation->is_string()) {
        for (const int received : fds) ::close(received);
        return false;
    }
    const shared::JsonPtr degraded = parsed.value->opt("degraded");
    info->generation = generation->as_string();
    info->degraded = degraded && degraded->is_bool() && degraded->as_bool();
    info->manifest_fd = fds[0];
    info->font_fd = fds.size() > 1 ? fds[1] : -1;
    info->index_fd = fds.size() > 2 ? fds[2] : -1;
    return true;
}

int CompanionClient::open_table(const std::string &relative_path) {
    std::lock_guard<std::mutex> guard(request_mutex_);
    if (socket_ < 0 || relative_path.empty()) return -1;
    if (!send_request(CompanionRequest::Table, relative_path, -1)) return -1;
    uint32_t code = 0;
    std::string payload;
    std::vector<int> fds;
    if (!receive_frame(socket_, &code, &payload, &fds)) {
        for (const int received : fds) ::close(received);
        return -1;
    }
    if (code != static_cast<uint32_t>(CompanionStatus::Ok) || fds.empty()) {
        for (const int received : fds) ::close(received);
        return -1;
    }
    for (size_t i = 1; i < fds.size(); ++i) ::close(fds[i]);
    return fds.front();
}

bool CompanionClient::send_receipt(const std::string &json) {
    std::lock_guard<std::mutex> guard(request_mutex_);
    if (socket_ < 0) return false;
    if (!send_request(CompanionRequest::Receipt, json, -1)) return false;
    uint32_t code = 0;
    std::string payload;
    std::vector<int> fds;
    if (!receive_frame(socket_, &code, &payload, &fds)) {
        for (const int received : fds) ::close(received);
        return false;
    }
    for (const int received : fds) ::close(received);
    return code == static_cast<uint32_t>(CompanionStatus::Ok);
}

void serve_companion(int client_socket) {
    if (client_socket < 0) return;
    struct timeval timeout {};
    timeout.tv_sec = 120;
    ::setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    const std::string generation = read_active_generation();
    std::string generation_dir;
    std::string manifest_text;
    std::unordered_set<std::string> table_paths;
    std::vector<translation::SnapshotEntry> entries;
    bool index_required = false;
    if (!generation.empty()) {
        generation_dir = std::string(kGenerationsDir) + "/" + generation;
        const std::string manifest_path = generation_dir + "/manifest.json";
        if (shared::read_file(manifest_path, kMaxManifestBytes, &manifest_text) == shared::Code::Ok) {
            translation::SnapshotManifest manifest;
            std::string error;
            if (translation::parse_manifest(manifest_text, &manifest, &error) &&
                manifest.generation == generation) {
                entries = manifest.entries;
                index_required = manifest.index.present;
                for (const auto &entry : entries) table_paths.insert(entry.merged_path);
            } else {
                manifest_text.clear();
            }
        }
    }

    bool started = false;
    // An unconfirmed receipt cannot distinguish a crash from a normal short
    // session or another live process. A manual marker is the only degrade gate.
    const bool degraded = !generation.empty() && degraded_generation() == generation;
    while (true) {
        uint32_t code = 0;
        std::string payload;
        std::vector<int> fds;
        if (!receive_frame(client_socket, &code, &payload, &fds)) break;
        for (const int received : fds) ::close(received);
        switch (static_cast<CompanionRequest>(code)) {
            case CompanionRequest::Hello: {
                if (manifest_text.empty()) {
                    log_warn("companion: no published generation");
                    send_frame(client_socket, static_cast<uint32_t>(CompanionStatus::NotFound),
                               std::string(), nullptr, 0);
                    break;
                }
                const std::string manifest_path = generation_dir + "/manifest.json";
                const int manifest_fd = ::open(manifest_path.c_str(), O_RDONLY | O_CLOEXEC);
                if (manifest_fd < 0) {
                    send_frame(client_socket, static_cast<uint32_t>(CompanionStatus::IoError),
                               std::string(), nullptr, 0);
                    break;
                }
                int font_fd = ::open((generation_dir + "/" + translation::kFontRelativePath).c_str(),
                                     O_RDONLY | O_CLOEXEC);
                if (index_required && font_fd < 0) {
                    ::close(manifest_fd);
                    send_frame(client_socket, static_cast<uint32_t>(CompanionStatus::IoError),
                               std::string(), nullptr, 0);
                    break;
                }
                int index_fd = -1;
                if (index_required) {
                    index_fd = ::open((generation_dir + "/index/index.bin").c_str(),
                                      O_RDONLY | O_CLOEXEC);
                    if (index_fd < 0) {
                        ::close(manifest_fd);
                        if (font_fd >= 0) ::close(font_fd);
                        send_frame(client_socket, static_cast<uint32_t>(CompanionStatus::IoError),
                                   std::string(), nullptr, 0);
                        break;
                    }
                }
                int handles[3] = {manifest_fd, font_fd, index_fd};
                const uint32_t handle_count = index_fd >= 0 ? 3u : (font_fd >= 0 ? 2u : 1u);
                const std::string payload =
                    std::string("{\"generation\":\"") + generation + "\",\"degraded\":" +
                    (degraded ? "true" : "false") + "}";
                uint32_t status = static_cast<uint32_t>(CompanionStatus::Ok);
                if (!send_frame(client_socket, status, payload, handles, handle_count)) {
                    status = static_cast<uint32_t>(CompanionStatus::IoError);
                }
                ::close(manifest_fd);
                if (font_fd >= 0) ::close(font_fd);
                if (index_fd >= 0) ::close(index_fd);
                if (!started && status == static_cast<uint32_t>(CompanionStatus::Ok)) {
                    started = true;
                    write_receipt("{\"schema\":1,\"generation\":\"" + generation +
                                  "\",\"hookState\":\"none\",\"fontState\":\"none\","
                                  "\"hits\":0,\"misses\":0,\"code\":\"ok\",\"started\":true}");
                }
                break;
            }
            case CompanionRequest::Table: {
                if (payload.empty() || payload.size() > 128 ||
                    table_paths.find(payload) == table_paths.end()) {
                    send_frame(client_socket, static_cast<uint32_t>(CompanionStatus::BadRequest),
                               std::string(), nullptr, 0);
                    break;
                }
                const int fd = ::open((generation_dir + "/" + payload).c_str(), O_RDONLY | O_CLOEXEC);
                if (fd < 0) {
                    send_frame(client_socket, static_cast<uint32_t>(CompanionStatus::IoError),
                               std::string(), nullptr, 0);
                    break;
                }
                const int handle = fd;
                send_frame(client_socket, static_cast<uint32_t>(CompanionStatus::Ok), std::string(),
                           &handle, 1);
                ::close(fd);
                break;
            }
            case CompanionRequest::Receipt: {
                if (payload.size() > 4096) {
                    send_frame(client_socket, static_cast<uint32_t>(CompanionStatus::BadRequest),
                               std::string(), nullptr, 0);
                    break;
                }
                write_receipt(payload);
                send_frame(client_socket, static_cast<uint32_t>(CompanionStatus::Ok), std::string(),
                           nullptr, 0);
                break;
            }
            case CompanionRequest::Bye:
            default:
                ::close(client_socket);
                return;
        }
    }
    ::close(client_socket);
}

} // namespace bybus::runtime
