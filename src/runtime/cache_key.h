// Identity of one verified table inside the bounded table cache.
//
// A cached table may only be reused when the generation that produced it, the
// manifest digest it was verified against, the declared byte count and the
// stored in-memory length all agree. Binding all four keeps a generation swap
// from serving bytes that belong to the previous generation, which is the same
// rule the reference runtime applies when it swaps the index and drops every
// derived cache.
//
// Header-only and free of Android headers so host tests can include it as is.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace bybus::runtime {

struct TableCacheKey {
    std::string generation;
    std::string digest;       // 64 hex digits from the manifest
    uint64_t bytes = 0;       // manifest byte count
    size_t content_bytes = 0; // actual in-memory length
};

// All four fields must be equal for a hit. An empty digest never matches: bytes
// with no verified digest have nothing to bind them to a generation.
inline bool table_cache_key_matches(const TableCacheKey &key, const std::string &generation,
                                    const std::string &digest, uint64_t bytes,
                                    size_t content_bytes) {
    if (key.digest.empty() || digest.empty()) return false;
    return key.generation == generation && key.digest == digest && key.bytes == bytes &&
           key.content_bytes == content_bytes;
}

} // namespace bybus::runtime
