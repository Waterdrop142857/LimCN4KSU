// Runtime side of the snapshot: loads the manifest delivered by the companion,
// indexes entries by (resource, original digest), and reads merged tables in a
// bounded LRU cache. The monitor thread can warm verified tables before text
// hooks are installed, keeping companion IPC and disk reads off Unity callbacks.
//
// Cached bytes carry the (generation, digest, manifest bytes, content bytes)
// key that produced them, so opening a new generation drops every derived table
// instead of replaying the previous generation's content.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/cache_key.h"
#include "runtime/companion.h"
#include "runtime/translation_index.h"
#include "translation/snapshot.h"

namespace bybus::runtime {

class SnapshotReader {
public:
    ~SnapshotReader();
    // Consumes the hello reply: parses the manifest and keeps the client.
    bool open(CompanionClient *client, const HelloInfo &hello, std::string *error);

    // Called on the monitor thread before hooks are installed. Failures leave
    // individual tables on the existing verified, on-demand read path.
    void prewarm_tables();

    // `resource_key` may be a full resource path or a bare table file name.
    bool lookup(const std::string &resource_key, const std::string &original_sha256,
                std::string *merged, std::string *error);

    const std::string &generation() const { return generation_; }
    const translation::SnapshotProfile &profile() const { return profile_; }
    const TranslationIndex &translation_index() const { return translation_index_; }
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }
    bool has_font() const { return font_fd_ >= 0; }
    int font_fd() const { return font_fd_; }
    const std::string &font_sha256() const { return font_sha256_; }

    // Canonicalises a loader-supplied path into a manifest resource key. Returns
    // an empty string when the input cannot identify a resource.
    static std::string normalize_resource_key(const std::string &raw);

private:
    // Drops every derived table so the next read in a new generation goes back
    // to the verified companion path, mirroring the reference runtime's
    // "new index, clear derived caches" swap.
    void reset_cache();
    bool read_verified_table(const std::string &relative_path, const std::string &digest,
                             uint64_t expected_bytes, std::string *content);
    bool load_table(const std::string &relative_path, const std::string &digest,
                    uint64_t expected_bytes, std::string *content);

    CompanionClient *client_ = nullptr;
    translation::SnapshotIndex index_;
    TranslationIndex translation_index_;
    translation::SnapshotProfile profile_;
    std::string generation_;
    std::string font_sha256_;
    std::vector<translation::SnapshotEntry> entries_;
    int font_fd_ = -1;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
    std::mutex lookup_mutex_;

    struct CacheEntry {
        std::string content;
        std::string digest;
        std::string generation;
        uint64_t bytes = 0;
        uint64_t at = 0;
    };
    std::unordered_map<std::string, CacheEntry> cache_;
    uint64_t cache_bytes_ = 0;
    uint64_t cache_clock_ = 0;
};

} // namespace bybus::runtime
