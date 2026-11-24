// Host tests for src/runtime/cache_key.h.
//
// The merged-table cache is keyed by (generation, digest, manifest bytes,
// content bytes). A hit is only allowed when all four agree, so a generation
// swap or a changed digest can never replay the previous generation's bytes.
// Pure data layer: no filesystem, no Android APIs.
#include <cstddef>
#include <cstdint>
#include <string>

#include "runtime/cache_key.h"

#include "harness.h"

namespace {

constexpr const char *kGeneration =
    "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
constexpr const char *kOtherGeneration =
    "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100";
constexpr const char *kDigest =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr const char *kOtherDigest =
    "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
constexpr uint64_t kBytes = 4096;
constexpr size_t kContentBytes = 4096;

bybus::runtime::TableCacheKey make_entry() {
    bybus::runtime::TableCacheKey entry;
    entry.generation = kGeneration;
    entry.digest = kDigest;
    entry.bytes = kBytes;
    entry.content_bytes = kContentBytes;
    return entry;
}

bool matches(const bybus::runtime::TableCacheKey &entry, const std::string &generation,
             const std::string &digest, uint64_t bytes, size_t content_bytes) {
    return bybus::runtime::table_cache_key_matches(entry, generation, digest, bytes, content_bytes);
}

} // namespace

BYBUS_TEST(table_cache_key_matches_when_all_four_fields_are_equal) {
    check(matches(make_entry(), kGeneration, kDigest, kBytes, kContentBytes),
          "identical (generation, digest, bytes, content_bytes) must hit");
}

BYBUS_TEST(table_cache_key_misses_on_other_generation) {
    check(!matches(make_entry(), kOtherGeneration, kDigest, kBytes, kContentBytes),
          "a different generation must not reuse the cached table");
}

BYBUS_TEST(table_cache_key_misses_on_other_digest) {
    check(!matches(make_entry(), kGeneration, kOtherDigest, kBytes, kContentBytes),
          "a different manifest digest must not reuse the cached table");
}

BYBUS_TEST(table_cache_key_misses_on_other_declared_bytes) {
    check(!matches(make_entry(), kGeneration, kDigest, kBytes + 1, kContentBytes),
          "a different manifest byte count must not reuse the cached table");
}

BYBUS_TEST(table_cache_key_misses_on_other_content_bytes) {
    check(!matches(make_entry(), kGeneration, kDigest, kBytes, kContentBytes + 1),
          "a different stored length must not reuse the cached table");
}

BYBUS_TEST(table_cache_key_empty_digest_never_matches) {
    bybus::runtime::TableCacheKey entry = make_entry();
    entry.digest.clear();
    check(!matches(entry, kGeneration, kDigest, kBytes, kContentBytes),
          "an entry without a digest has nothing to bind it to a generation");
    check(!matches(make_entry(), kGeneration, std::string(), kBytes, kContentBytes),
          "a query without a digest cannot identify verified bytes");
}
