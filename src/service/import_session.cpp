// service/import_session.cpp
// Responsibility: implementation of the chunked import sessions. Bounded
// base64 decoding, contiguous index enforcement, digest verification and a
// 2 hour expiry sweep. The lock covers one session directory only (one part
// file plus one small JSON); no network and no snapshot build runs here.
#include "service/import_session.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "shared/fs.h"
#include "shared/json.h"
#include "shared/sha256.h"

namespace bybus::service {
namespace {

constexpr const char *kSessionName = "session.json";

int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Strict decoder: no whitespace, padding only in the final group, output
// bounded by `max_bytes`.
bool base64_decode(const std::string &text, uint64_t max_bytes, std::string *out) {
    if (text.empty() || text.size() % 4 != 0) return false;
    out->clear();
    out->reserve(text.size() / 4 * 3);
    for (size_t index = 0; index < text.size(); index += 4) {
        int values[4] = {0, 0, 0, 0};
        int padding = 0;
        for (size_t slot = 0; slot < 4; ++slot) {
            const char c = text[index + slot];
            if (c == '=') {
                if (index + 4 != text.size() || slot < 2) return false;
                ++padding;
                continue;
            }
            if (padding > 0) return false;
            const int value = base64_value(c);
            if (value < 0) return false;
            values[slot] = value;
        }
        const uint32_t triple = (static_cast<uint32_t>(values[0]) << 18) |
                                (static_cast<uint32_t>(values[1]) << 12) |
                                (static_cast<uint32_t>(values[2]) << 6) |
                                static_cast<uint32_t>(values[3]);
        out->push_back(static_cast<char>((triple >> 16) & 0xFF));
        if (padding < 2) out->push_back(static_cast<char>((triple >> 8) & 0xFF));
        if (padding < 1) out->push_back(static_cast<char>(triple & 0xFF));
        if (static_cast<uint64_t>(out->size()) > max_bytes) return false;
    }
    return true;
}

bool valid_import_name(const std::string &name) {
    if (name.empty() || name.size() > kMaxImportNameBytes) return false;
    for (const char c : name) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) return false;
        if (c == '/' || c == '\\') return false;
    }
    return true;
}

std::string part_path(const std::string &dir, uint64_t index) {
    return dir + "/part-" + std::to_string(static_cast<unsigned long long>(index));
}

} // namespace

ImportSessions::ImportSessions(const ServicePaths &paths) : paths_(paths) {}

std::string ImportSessions::dir_of(const std::string &session_id) const {
    return paths_.import_dir(session_id);
}

shared::Code ImportSessions::begin(const std::string &name, uint64_t total_bytes,
                                  const std::string &sha256, std::string *session_id) {
    if (session_id == nullptr) return shared::Code::InvalidArgument;
    if (!valid_import_name(name)) return shared::Code::InvalidArgument;
    if (total_bytes > kMaxArchiveBytes) return shared::Code::ArchiveLimit;
    if (!sha256.empty() && !shared::is_sha256_hex(sha256)) return shared::Code::InvalidArgument;
    ImportSessionInfo info;
    info.id = "s-" + random_hex(16);
    info.name = name;
    info.sha256 = sha256;
    info.total_bytes = total_bytes;
    info.created_at_millis = now_millis();
    {
        std::lock_guard<std::mutex> guard(mu_);
        const shared::Code made = shared::make_dirs(dir_of(info.id), 0700);
        if (made != shared::Code::Ok) return made;
        const shared::Code wrote = write_session(info);
        if (wrote != shared::Code::Ok) {
            (void)shared::remove_tree(dir_of(info.id));
            return wrote;
        }
    }
    *session_id = info.id;
    return shared::Code::Ok;
}

shared::Code ImportSessions::chunk(const std::string &session_id, uint64_t index,
                                   const std::string &base64, uint64_t *received,
                                   uint64_t *total) {
    if (!valid_session_id(session_id)) return shared::Code::InvalidArgument;
    std::string bytes;
    if (!base64_decode(base64, kImportChunkBytes, &bytes)) return shared::Code::InvalidArgument;
    std::lock_guard<std::mutex> guard(mu_);
    ImportSessionInfo info;
    const shared::Code read = read_session(session_id, &info);
    if (read != shared::Code::Ok) return read;
    if (index != info.next_index) return shared::Code::InvalidArgument;
    if (info.received + bytes.size() > info.total_bytes) return shared::Code::InvalidArgument;
    const shared::Code wrote =
        shared::write_file(part_path(dir_of(session_id), index), bytes);
    if (wrote != shared::Code::Ok) return wrote;
    info.received += bytes.size();
    info.next_index = index + 1;
    const shared::Code persisted = write_session(info);
    if (persisted != shared::Code::Ok) return persisted;
    if (received) *received = info.received;
    if (total) *total = info.total_bytes;
    return shared::Code::Ok;
}

shared::Code ImportSessions::describe(const std::string &session_id, ImportSessionInfo *out) const {
    if (!valid_session_id(session_id) || out == nullptr) return shared::Code::InvalidArgument;
    std::lock_guard<std::mutex> guard(mu_);
    return read_session(session_id, out);
}

shared::Code ImportSessions::assemble(const std::string &session_id, const std::string &target,
                                      std::string *sha256_hex, uint64_t *bytes) {
    if (!valid_session_id(session_id)) return shared::Code::InvalidArgument;
    if (!inside_state_root(paths_, target)) return shared::Code::InvalidArgument;
    std::lock_guard<std::mutex> guard(mu_);
    ImportSessionInfo info;
    const shared::Code read = read_session(session_id, &info);
    if (read != shared::Code::Ok) return read;
    if (info.received != info.total_bytes) return shared::Code::ArchiveInvalid;
    const int out_fd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out_fd < 0) return shared::Code::IoError;
    shared::Sha256 digest;
    uint64_t total = 0;
    shared::Code code = shared::Code::Ok;
    for (uint64_t index = 0; index < info.next_index; ++index) {
        std::string part;
        const shared::Code part_read =
            shared::read_file(part_path(dir_of(session_id), index), kImportChunkBytes, &part);
        if (part_read != shared::Code::Ok) {
            code = shared::Code::ArchiveInvalid;
            break;
        }
        digest.update(part);
        total += part.size();
        size_t offset = 0;
        while (offset < part.size()) {
            const ssize_t written = ::write(out_fd, part.data() + offset, part.size() - offset);
            if (written < 0) {
                if (errno == EINTR) continue;
                code = shared::Code::IoError;
                break;
            }
            offset += static_cast<size_t>(written);
        }
        if (code != shared::Code::Ok) break;
    }
    if (code == shared::Code::Ok && ::fsync(out_fd) != 0) code = shared::Code::IoError;
    ::close(out_fd);
    if (code != shared::Code::Ok) {
        (void)::unlink(target.c_str());
        return code;
    }
    if (total != info.total_bytes) {
        (void)::unlink(target.c_str());
        return shared::Code::ArchiveInvalid;
    }
    uint8_t raw[32];
    digest.finish(raw);
    const std::string computed = shared::Sha256::hex(raw, sizeof(raw));
    if (!info.sha256.empty() && computed != info.sha256) {
        (void)::unlink(target.c_str());
        return shared::Code::ArchiveInvalid;
    }
    if (sha256_hex) *sha256_hex = computed;
    if (bytes) *bytes = total;
    return shared::Code::Ok;
}

shared::Code ImportSessions::verify(const std::string &session_id, std::string *sha256_hex,
                                    uint64_t *bytes) {
    if (!valid_session_id(session_id)) return shared::Code::InvalidArgument;
    std::lock_guard<std::mutex> guard(mu_);
    ImportSessionInfo info;
    const shared::Code read = read_session(session_id, &info);
    if (read != shared::Code::Ok) return read;
    if (info.received != info.total_bytes) return shared::Code::ArchiveInvalid;
    shared::Sha256 digest;
    uint64_t total = 0;
    for (uint64_t index = 0; index < info.next_index; ++index) {
        std::string part;
        const shared::Code part_read =
            shared::read_file(part_path(dir_of(session_id), index), kImportChunkBytes, &part);
        if (part_read != shared::Code::Ok) return shared::Code::ArchiveInvalid;
        digest.update(part);
        total += part.size();
    }
    if (total != info.total_bytes) return shared::Code::ArchiveInvalid;
    uint8_t raw[32];
    digest.finish(raw);
    const std::string computed = shared::Sha256::hex(raw, sizeof(raw));
    if (!info.sha256.empty() && computed != info.sha256) return shared::Code::ArchiveInvalid;
    if (sha256_hex) *sha256_hex = computed;
    if (bytes) *bytes = total;
    return shared::Code::Ok;
}

shared::Code ImportSessions::abort(const std::string &session_id) {
    if (!valid_session_id(session_id)) return shared::Code::InvalidArgument;
    std::lock_guard<std::mutex> guard(mu_);
    const std::string dir = dir_of(session_id);
    if (!shared::exists(dir)) return shared::Code::InvalidArgument;
    return shared::remove_tree(dir);
}

shared::Code ImportSessions::cleanup_expired() {
    std::vector<std::string> names;
    const shared::Code listed = shared::list_dir(paths_.imports(), 256, &names);
    if (listed == shared::Code::GameMissing) return shared::Code::Ok;
    if (listed != shared::Code::Ok) return listed;
    const int64_t cutoff = now_millis() - kImportSessionTtlMillis;
    for (const std::string &name : names) {
        if (!valid_session_id(name)) continue;
        const std::string dir = dir_of(name);
        struct stat info {};
        if (::stat(dir.c_str(), &info) != 0) continue;
        if (static_cast<int64_t>(info.st_mtime) * 1000 > cutoff) continue;
        std::lock_guard<std::mutex> guard(mu_);
        (void)shared::remove_tree(dir);
    }
    return shared::Code::Ok;
}

shared::Code ImportSessions::cleanup_all() {
    std::vector<std::string> names;
    const shared::Code listed = shared::list_dir(paths_.imports(), 256, &names);
    if (listed == shared::Code::GameMissing) return shared::Code::Ok;
    if (listed != shared::Code::Ok) return listed;
    std::lock_guard<std::mutex> guard(mu_);
    for (const std::string &name : names) {
        if (name.empty() || name == "." || name == "..") continue;
        (void)shared::remove_tree(paths_.imports() + "/" + name);
    }
    return shared::Code::Ok;
}

shared::Code ImportSessions::read_session(const std::string &session_id,
                                         ImportSessionInfo *out) const {
    std::string text;
    const shared::Code read =
        shared::read_file(dir_of(session_id) + "/" + kSessionName, kMaxJobFileBytes, &text);
    if (read != shared::Code::Ok) return shared::Code::InvalidArgument;
    const shared::ParseOutcome parsed = shared::parse(text);
    if (!parsed.ok || !parsed.value || !parsed.value->is_object()) {
        return shared::Code::InvalidArgument;
    }
    const shared::JsonPtr root = parsed.value;
    const shared::JsonPtr id = root->opt("id");
    const shared::JsonPtr name = root->opt("name");
    const shared::JsonPtr total = root->opt("totalBytes");
    if (!id || !id->is_string() || id->as_string() != session_id) {
        return shared::Code::InvalidArgument;
    }
    if (!name || !name->is_string()) return shared::Code::InvalidArgument;
    if (!total || !total->is_integral() || total->as_int() < 0 ||
        static_cast<uint64_t>(total->as_int()) > kMaxArchiveBytes) {
        return shared::Code::InvalidArgument;
    }
    ImportSessionInfo info;
    info.id = id->as_string();
    info.name = name->as_string();
    info.total_bytes = static_cast<uint64_t>(total->as_int());
    const shared::JsonPtr sha = root->opt("sha256");
    if (sha && sha->is_string() && shared::is_sha256_hex(sha->as_string())) {
        info.sha256 = sha->as_string();
    }
    const shared::JsonPtr received = root->opt("received");
    if (received && received->is_integral() && received->as_int() >= 0) {
        info.received = static_cast<uint64_t>(received->as_int());
    }
    const shared::JsonPtr next = root->opt("nextIndex");
    if (next && next->is_integral() && next->as_int() >= 0) {
        info.next_index = static_cast<uint64_t>(next->as_int());
    }
    const shared::JsonPtr created = root->opt("createdAtMillis");
    if (created && created->is_integral()) info.created_at_millis = created->as_int();
    if (info.received > info.total_bytes) return shared::Code::InvalidArgument;
    *out = std::move(info);
    return shared::Code::Ok;
}

shared::Code ImportSessions::write_session(const ImportSessionInfo &info) const {
    shared::JsonPtr root = shared::Json::object();
    root->put_int("schema", 1);
    root->put_string("id", info.id);
    root->put_string("name", info.name);
    root->put_string("sha256", info.sha256);
    root->put_int("totalBytes", static_cast<int64_t>(info.total_bytes));
    root->put_int("received", static_cast<int64_t>(info.received));
    root->put_int("nextIndex", static_cast<int64_t>(info.next_index));
    root->put_int("createdAtMillis", info.created_at_millis);
    return write_atomic(dir_of(info.id) + "/" + kSessionName, root->dump());
}

} // namespace bybus::service
