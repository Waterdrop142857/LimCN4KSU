#include "runtime/snapshot_reader.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <vector>

#include "runtime/log.h"
#include "shared/fs.h"
#include "shared/sha256.h"
#include "shared/utf.h"

namespace bybus::runtime {
namespace {

constexpr uint64_t kMaxManifestBytes = UINT64_MAX;
constexpr uint64_t kCacheBudgetBytes = 32ull * 1024 * 1024;
constexpr size_t kCacheMaxEntries = 32;

std::string read_fd_all(int fd, uint64_t limit, bool *ok) {
    *ok = false;
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) return std::string();
    if (info.st_size < 0 || static_cast<uint64_t>(info.st_size) > limit ||
        static_cast<uint64_t>(info.st_size) > std::string().max_size()) return std::string();
    std::string content;
    content.resize(static_cast<size_t>(info.st_size));
    size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t read_size = ::pread(fd, &content[offset], content.size() - offset,
                                          static_cast<off_t>(offset));
        if (read_size < 0) {
            if (errno == EINTR) continue;
            return std::string();
        }
        if (read_size == 0) break;
        offset += static_cast<size_t>(read_size);
    }
    content.resize(offset);
    *ok = offset == static_cast<size_t>(info.st_size);
    return content;
}

} // namespace

SnapshotReader::~SnapshotReader() {
    if (font_fd_ >= 0) ::close(font_fd_);
}

std::string SnapshotReader::normalize_resource_key(const std::string &raw) {
    if (raw.empty() || raw.size() > 512) return std::string();
    std::string key;
    key.reserve(raw.size());
    for (const char c : raw) {
        if (static_cast<unsigned char>(c) < 0x20) return std::string();
        key.push_back(c == '\\' ? '/' : c);
    }
    while (!key.empty() && key.front() == '/') key.erase(key.begin());
    while (key.size() > 2 && key.compare(0, 2, "./") == 0) key.erase(0, 2);
    const size_t localize = key.find("Localize/");
    if (localize != std::string::npos) {
        key = key.substr(localize);
    } else if (key.compare(0, 3, "jp/") == 0 || key.compare(0, 3, "en/") == 0 ||
               key.compare(0, 3, "kr/") == 0) {
        key = "Localize/" + key;
    } else if (key.find('/') == std::string::npos) {
        // Bare table name: the index still resolves it when unique by file name.
        return key;
    } else {
        return std::string();
    }
    if (key.find("..") != std::string::npos) return std::string();
    return key;
}

bool SnapshotReader::open(CompanionClient *client, const HelloInfo &hello, std::string *error) {
    // A new generation invalidates every derived table, exactly like the
    // reference runtime clearing its index-derived caches before the swap. Doing
    // it first also covers a failed open: the next successful open starts cold.
    reset_cache();
    client_ = client;
    generation_ = hello.generation;
    if (!client_ || !shared::is_sha256_hex(generation_)) {
        if (hello.manifest_fd >= 0) ::close(hello.manifest_fd);
        if (hello.font_fd >= 0) ::close(hello.font_fd);
        if (hello.index_fd >= 0) ::close(hello.index_fd);
        *error = "snapshot_generation";
        return false;
    }
    bool ok = false;
    const std::string text = read_fd_all(hello.manifest_fd, kMaxManifestBytes, &ok);
    if (hello.manifest_fd >= 0) ::close(hello.manifest_fd);
    if (!ok) {
        if (hello.font_fd >= 0) ::close(hello.font_fd);
        if (hello.index_fd >= 0) ::close(hello.index_fd);
        *error = "snapshot_manifest_read";
        return false;
    }
    translation::SnapshotManifest manifest;
    if (!translation::parse_manifest(text, &manifest, error)) {
        if (hello.font_fd >= 0) ::close(hello.font_fd);
        if (hello.index_fd >= 0) ::close(hello.index_fd);
        return false;
    }
    if (manifest.generation != generation_) {
        if (hello.font_fd >= 0) ::close(hello.font_fd);
        if (hello.index_fd >= 0) ::close(hello.index_fd);
        *error = "snapshot_generation_mismatch";
        return false;
    }
    if (!index_.load(manifest, error)) {
        if (hello.font_fd >= 0) ::close(hello.font_fd);
        if (hello.index_fd >= 0) ::close(hello.index_fd);
        return false;
    }
    translation_index_ = TranslationIndex();
    if (manifest.index.present) {
        if (hello.index_fd < 0) {
            if (hello.font_fd >= 0) ::close(hello.font_fd);
            *error = "snapshot_index_missing";
            return false;
        }
        bool index_ok = false;
        const std::string index_bytes =
            read_fd_all(hello.index_fd, manifest.index.bytes, &index_ok);
        ::close(hello.index_fd);
        if (!index_ok || index_bytes.size() != manifest.index.bytes ||
            shared::Sha256::hex(index_bytes) != manifest.index.sha256 ||
            !translation_index_.load(index_bytes, error) ||
            translation_index_.unique_count() != manifest.index.unique ||
            translation_index_.term_count() != manifest.index.terms ||
            translation_index_.contextual_count() != manifest.index.contextual) {
            if (hello.font_fd >= 0) ::close(hello.font_fd);
            *error = "snapshot_index_invalid";
            return false;
        }
    } else if (hello.index_fd >= 0) {
        ::close(hello.index_fd);
    }
    if (font_fd_ >= 0) ::close(font_fd_);
    profile_ = manifest.profile;
    font_sha256_ = manifest.font_sha256;
    entries_ = std::move(manifest.entries);
    font_fd_ = hello.font_fd;
    log_info("snapshot generation=%s entries=%zu font=%d", generation_.c_str(), index_.size(),
             font_fd_ >= 0 ? 1 : 0);
    return true;
}

void SnapshotReader::reset_cache() {
    cache_.clear();
    cache_bytes_ = 0;
}

bool SnapshotReader::read_verified_table(const std::string &relative_path,
                                         const std::string &digest, uint64_t expected_bytes,
                                         std::string *content) {
    if (!client_) return false;
    const int fd = client_->open_table(relative_path);
    if (fd < 0) return false;
    bool ok = false;
    std::string text = read_fd_all(fd, expected_bytes, &ok);
    ::close(fd);
    if (!ok || text.size() != expected_bytes) return false;
    if (shared::Sha256::hex(text) != digest) {
        log_warn("table digest mismatch");
        return false;
    }
    *content = std::move(text);
    return true;
}

void SnapshotReader::prewarm_tables() {
    size_t ready = 0;
    size_t failed = 0;
    // Warm through load_table so prewarmed bytes travel the same verified path
    // (digest check on the way in, key check on every later hit) as on-demand
    // reads. There is no second, unchecked tier any more.
    for (const translation::SnapshotEntry &entry : entries_) {
        try {
            std::string content;
            if (!load_table(entry.merged_path, entry.merged_sha256, entry.merged_bytes, &content)) {
                ++failed;
                continue;
            }
            ++ready;
        } catch (const std::bad_alloc &) {
            ++failed;
            // Keep already verified tables and leave the rest on the normal
            // on-demand path. Do not make an allocation failure fatal here.
            break;
        }
    }
    log_info("snapshot prewarm ready=%zu failed=%zu", ready, failed);
}

// A cache hit deliberately skips the SHA-256 recomputation. The bytes were
// hashed against the manifest digest before they entered the map, the key binds
// that digest together with the generation and both byte counts, and the stored
// buffer is only mutable from inside this reader. Re-hashing on every hit would
// re-check data no external writer can reach; the key check preserves the same
// "bytes of this digest in this generation" guarantee at a fraction of the cost.
bool SnapshotReader::load_table(const std::string &relative_path, const std::string &digest,
                                uint64_t expected_bytes, std::string *content) {
    const auto cached = cache_.find(relative_path);
    if (cached != cache_.end()) {
        TableCacheKey key;
        key.generation = cached->second.generation;
        key.digest = cached->second.digest;
        key.bytes = cached->second.bytes;
        key.content_bytes = cached->second.content.size();
        if (table_cache_key_matches(key, generation_, digest, expected_bytes, key.content_bytes)) {
            cached->second.at = ++cache_clock_;
            *content = cached->second.content;
            return true;
        }
        // Stale entry: the generation or the digest moved on. Drop it and read
        // the verified bytes again instead of serving the older table.
        cache_bytes_ -= cached->second.content.size();
        cache_.erase(cached);
    }
    std::string text;
    if (!read_verified_table(relative_path, digest, expected_bytes, &text)) return false;
    if (text.size() > kCacheBudgetBytes) {
        *content = std::move(text);
        return true;
    }
    while (cache_bytes_ + text.size() > kCacheBudgetBytes && !cache_.empty()) {
        auto oldest = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.at < oldest->second.at) oldest = it;
        }
        cache_bytes_ -= oldest->second.content.size();
        cache_.erase(oldest);
    }
    if (cache_.size() >= kCacheMaxEntries) {
        auto oldest = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.at < oldest->second.at) oldest = it;
        }
        cache_bytes_ -= oldest->second.content.size();
        cache_.erase(oldest);
    }
    CacheEntry entry;
    entry.content = text;
    entry.digest = digest;
    entry.generation = generation_;
    entry.bytes = expected_bytes;
    entry.at = ++cache_clock_;
    // Account only for bytes that really entered the map; a failed emplace must
    // not leave cache_bytes_ larger than the entries it describes.
    const auto inserted = cache_.emplace(relative_path, std::move(entry));
    if (inserted.second) cache_bytes_ += inserted.first->second.content.size();
    *content = std::move(text);
    return true;
}

bool SnapshotReader::lookup(const std::string &resource_key, const std::string &original_sha256,
                            std::string *merged, std::string *error) {
    std::lock_guard<std::mutex> guard(lookup_mutex_);
    const std::string key = normalize_resource_key(resource_key);
    if (key.empty() || original_sha256.size() != 64) {
        ++misses_;
        *error = "lookup_key";
        return false;
    }
    const translation::SnapshotEntry *entry = index_.find(key, original_sha256);
    if (!entry) {
        ++misses_;
        *error = "lookup_miss";
        return false;
    }
    if (!load_table(entry->merged_path, entry->merged_sha256, entry->merged_bytes, merged)) {
        ++misses_;
        *error = "lookup_read";
        return false;
    }
    ++hits_;
    return true;
}

} // namespace bybus::runtime
