// service/file_ops.cpp
// Responsibility: implementation of the streaming digest/copy helpers.
#include "service/file_ops.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>

#include "shared/sha256.h"

namespace bybus::service {
namespace {

constexpr size_t kChunkBytes = 256 * 1024;

shared::Code read_chunk(int fd, std::string *buffer, bool *eof) {
    buffer->resize(kChunkBytes);
    ssize_t got = -1;
    do {
        got = ::read(fd, &(*buffer)[0], kChunkBytes);
    } while (got < 0 && errno == EINTR);
    if (got < 0) {
        buffer->clear();
        return shared::Code::IoError;
    }
    buffer->resize(static_cast<size_t>(got));
    *eof = got == 0;
    return shared::Code::Ok;
}

} // namespace

shared::Code hash_file(const std::string &path, std::string *sha256_hex, uint64_t *bytes) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return shared::Code::IoError;
    shared::Sha256 digest;
    std::string buffer;
    uint64_t total = 0;
    shared::Code code = shared::Code::Ok;
    for (;;) {
        bool eof = false;
        code = read_chunk(fd, &buffer, &eof);
        if (code != shared::Code::Ok) break;
        if (eof) break;
        digest.update(buffer);
        if (buffer.size() > UINT64_MAX - total) { code = shared::Code::IoError; break; }
        total += buffer.size();
    }
    ::close(fd);
    if (code != shared::Code::Ok) return code;
    uint8_t raw[32];
    digest.finish(raw);
    if (sha256_hex) *sha256_hex = shared::Sha256::hex(raw, sizeof(raw));
    if (bytes) *bytes = total;
    return shared::Code::Ok;
}

shared::Code copy_file_hashed(const std::string &source, const std::string &target,
                              uint64_t max_bytes, std::string *sha256_hex, uint64_t *bytes) {
    const int in = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return shared::Code::IoError;
    struct stat info {};
    if (::fstat(in, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(in);
        return shared::Code::IoError;
    }
    if (static_cast<uint64_t>(info.st_size) > max_bytes) {
        ::close(in);
        return shared::Code::ArchiveLimit;
    }
    const int out = ::open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out < 0) {
        ::close(in);
        return shared::Code::IoError;
    }
    shared::Sha256 digest;
    std::string buffer;
    uint64_t total = 0;
    shared::Code code = shared::Code::Ok;
    for (;;) {
        bool eof = false;
        code = read_chunk(in, &buffer, &eof);
        if (code != shared::Code::Ok) break;
        if (eof) break;
        digest.update(buffer);
        if (buffer.size() > UINT64_MAX - total) { code = shared::Code::IoError; break; }
        total += buffer.size();
        if (total > max_bytes) {
            code = shared::Code::ArchiveLimit;
            break;
        }
        size_t offset = 0;
        while (offset < buffer.size()) {
            const ssize_t written = ::write(out, buffer.data() + offset, buffer.size() - offset);
            if (written < 0) {
                if (errno == EINTR) continue;
                code = shared::Code::IoError;
                break;
            }
            if (written == 0) { code = shared::Code::IoError; break; }
            offset += static_cast<size_t>(written);
        }
        if (code != shared::Code::Ok) break;
    }
    if (code == shared::Code::Ok && ::fsync(out) != 0) code = shared::Code::IoError;
    ::close(out);
    ::close(in);
    if (code != shared::Code::Ok) {
        (void)::unlink(target.c_str());
        return code;
    }
    uint8_t raw[32];
    digest.finish(raw);
    if (sha256_hex) *sha256_hex = shared::Sha256::hex(raw, sizeof(raw));
    if (bytes) *bytes = total;
    return shared::Code::Ok;
}

} // namespace bybus::service
