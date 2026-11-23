// service/paths.cpp
// Responsibility: implementation of the layout helpers declared in paths.h.
// Only directory creation, atomic file replacement and clock/random helpers;
// no protocol knowledge and no job logic.
#include "service/paths.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>

#include "shared/fs.h"

namespace bybus::service {
namespace {

constexpr const char *kHexDigits = "0123456789abcdef";

int64_t clock_millis(clockid_t id) {
    struct timespec spec {};
    if (::clock_gettime(id, &spec) != 0) return 0;
    return static_cast<int64_t>(spec.tv_sec) * 1000 + static_cast<int64_t>(spec.tv_nsec / 1000000);
}

bool fill_random(unsigned char *buffer, size_t size) {
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t filled = 0;
    while (filled < size) {
        const ssize_t got = ::read(fd, buffer + filled, size - filled);
        if (got < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (got == 0) break;
        filled += static_cast<size_t>(got);
    }
    ::close(fd);
    return filled == size;
}

} // namespace

int64_t now_millis() { return clock_millis(CLOCK_REALTIME); }

int64_t monotonic_millis() { return clock_millis(CLOCK_MONOTONIC); }

std::string random_hex(size_t characters) {
    std::string out;
    out.reserve(characters);
    unsigned char buffer[32];
    uint64_t state = static_cast<uint64_t>(now_millis()) ^
                     (static_cast<uint64_t>(::getpid()) << 24) ^
                     static_cast<uint64_t>(monotonic_millis());
    while (out.size() < characters) {
        if (!fill_random(buffer, sizeof(buffer))) {
            for (size_t offset = 0; offset < sizeof(buffer); offset += 8) {
                state = state * 6364136223846793005ull + 1442695040888963407ull;
                const uint64_t mixed = state ^ (state >> 29) ^ (state >> 47);
                for (size_t byte = 0; byte < 8; ++byte) {
                    buffer[offset + byte] = static_cast<unsigned char>(mixed >> (byte * 8));
                }
            }
        }
        for (size_t index = 0; index < sizeof(buffer) && out.size() < characters; ++index) {
            out.push_back(kHexDigits[buffer[index] & 0x0F]);
            if (out.size() < characters) out.push_back(kHexDigits[(buffer[index] >> 4) & 0x0F]);
        }
    }
    return out;
}

bool valid_hex_token(const std::string &value, const char *prefix, size_t hex_chars) {
    const std::string head(prefix);
    if (value.size() != head.size() + hex_chars) return false;
    if (value.compare(0, head.size(), head) != 0) return false;
    for (size_t index = head.size(); index < value.size(); ++index) {
        const char c = value[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool valid_job_id(const std::string &value) { return valid_hex_token(value, "j-", 16); }

bool valid_session_id(const std::string &value) { return valid_hex_token(value, "s-", 16); }

bool valid_source_id(const std::string &value) {
    if (value.empty() || value.size() > 64) return false;
    if (value.front() == '-') return false;
    for (const char c : value) {
        const bool accepted = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!accepted) return false;
    }
    return true;
}

bool valid_absolute_path(const std::string &value) {
    if (value.empty() || value.size() > 4096) return false;
    if (value.front() != '/') return false;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t separator = value.find('/', start);
        const size_t end = separator == std::string::npos ? value.size() : separator;
        const std::string segment = value.substr(start, end - start);
        if (segment == "..") return false;
        if (separator == std::string::npos) break;
        start = separator + 1;
    }
    for (const char c : value) {
        if (static_cast<unsigned char>(c) < 0x20) return false;
    }
    return true;
}

shared::Code ensure_layout(const ServicePaths &paths) {
    if (!valid_absolute_path(paths.state_root)) return shared::Code::InvalidArgument;
    if (!valid_absolute_path(paths.module_root)) return shared::Code::InvalidArgument;
    static const char *const kSuffixes[] = {"",          "/sources", "/archives", "/imports",
                                            "/generations", "/jobs",  "/logs",     "/runtime",
                                            "/inbox",   "/run"};
    for (const char *suffix : kSuffixes) {
        const shared::Code made = shared::make_dirs(paths.state_root + suffix, 0700);
        if (made != shared::Code::Ok) return made;
    }
    if (::chmod(paths.state_root.c_str(), 0700) != 0) return shared::Code::IoError;
    return shared::Code::Ok;
}

bool inside_state_root(const ServicePaths &paths, const std::string &candidate) {
    return shared::path_is_inside(paths.state_root, candidate);
}

shared::Code write_atomic(const std::string &path, const std::string &content) {
    const std::string temp = path + ".tmp";
    const shared::Code wrote = shared::write_file(temp, content);
    if (wrote != shared::Code::Ok) return wrote;
    return shared::atomic_replace(temp, path);
}

} // namespace bybus::service
