// Schema-9 translation index format tests.
//
// TranslationIndexCompiler.kt:538-556 writes one header (magic, schema, the three
// map counts) followed by three pair groups in linked-map order. The reader in
// src/runtime/translation_index.cpp must accept exactly that layout and reject the
// earlier port that repeated a count before every group.
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "harness.h"
#include "runtime/translation_index.h"
#include "translation/index_compiler.h"
#include "translation/json_pairer.h"

namespace {

using bybus::runtime::TranslationIndex;
using bybus::translation::CompiledTranslationIndex;
using bybus::translation::IndexRecord;

constexpr size_t kHeaderSize = 6 + 4 * 4; // magic, schema, three counts

void append_be32(std::string *out, uint32_t value) {
    out->push_back(static_cast<char>(value >> 24));
    out->push_back(static_cast<char>(value >> 16));
    out->push_back(static_cast<char>(value >> 8));
    out->push_back(static_cast<char>(value));
}

uint32_t read_be32(const std::string &bytes, size_t at) {
    const auto *data = reinterpret_cast<const unsigned char *>(bytes.data() + at);
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

void append_pair(std::string *out, const std::string &key, const std::string &value) {
    append_be32(out, static_cast<uint32_t>(key.size()));
    out->append(key);
    append_be32(out, static_cast<uint32_t>(value.size()));
    out->append(value);
}

// The layout the port produced before this change: a count in front of each map.
std::string legacy_layout_bytes() {
    std::string bytes("LZTI1\0", 6);
    append_be32(&bytes, 9);
    append_be32(&bytes, 1);
    append_pair(&bytes, "A", "B");
    append_be32(&bytes, 1);
    append_pair(&bytes, "C", "D");
    append_be32(&bytes, 0);
    return bytes;
}

// U+FFFD: is_usable_display_text() rejects encoding damage.
std::string replacement_character() { return u8"\uFFFD"; }

} // namespace

BYBUS_TEST(index_header_matches_reference_layout) {
    CompiledTranslationIndex index;
    index.unique_entries = {{"alpha", "A"}, {"beta", "B"}};
    index.unique_order = {"beta", "alpha"};
    index.term_entries = {{"alpha", "A"}};
    index.term_order = {"alpha"};

    std::string bytes;
    std::string error;
    check(bybus::translation::write_translation_index(index, &bytes, &error),
          "index: header fixture writes");
    check(bytes.size() > kHeaderSize, "index: header fixture has a payload");
    const unsigned char expected[kHeaderSize] = {
        'L', 'Z', 'T', 'I', '1', 0x00,                     // "LZTI1\0"
        0x00, 0x00, 0x00, 0x09,                            // schema 9
        0x00, 0x00, 0x00, 0x02,                            // uniqueCount
        0x00, 0x00, 0x00, 0x01,                            // termCount
        0x00, 0x00, 0x00, 0x00};                           // contextualCount
    check(std::memcmp(bytes.data(), expected, sizeof(expected)) == 0,
          "index: magic, schema and the three counts match the reference layout");
    // The first pair follows the injected order, not std::map's lexical order.
    check(read_be32(bytes, kHeaderSize) == 4u && bytes.compare(kHeaderSize + 4, 4, "beta") == 0,
          "index: unique group is written in insertion order");
}

BYBUS_TEST(index_roundtrip_preserves_three_groups) {
    CompiledTranslationIndex index;
    bybus::translation::put_ordered(&index.unique_entries, &index.unique_order, "Haste", "Rapid");
    bybus::translation::put_ordered(&index.unique_entries, &index.unique_order, "Burn", "Scorch");
    bybus::translation::put_ordered(&index.term_entries, &index.term_order, "Haste", "Rapid");
    bybus::translation::put_ordered(&index.contextual_entries, &index.contextual_order,
                                    "@skill:2\x1f" "name\x1f" "Burn", "Scorch");

    std::string bytes;
    std::string error;
    check(bybus::translation::write_translation_index(index, &bytes, &error), "index: round-trip write");
    check(read_be32(bytes, 6) == 9u, "index: round-trip schema");
    check(read_be32(bytes, 10) == 2u && read_be32(bytes, 14) == 1u && read_be32(bytes, 18) == 1u,
          "index: round-trip header counts");

    TranslationIndex loaded;
    check(loaded.load(bytes, &error), "index: round-trip read");
    check(loaded.unique_count() == 2u && loaded.term_count() == 1u &&
              loaded.contextual_count() == 1u,
          "index: round-trip counts");
    const std::string *haste = loaded.find("Haste");
    const std::string *burn = loaded.find("Burn");
    const std::string *term = loaded.find_term("Haste");
    const std::string *context = loaded.find_context("@skill:2\x1f" "name\x1f" "Burn");
    check(haste && *haste == "Rapid", "index: unique entry survives the round-trip");
    check(burn && *burn == "Scorch", "index: second unique entry survives the round-trip");
    check(term && *term == "Rapid", "index: term entry survives the round-trip");
    check(context && *context == "Scorch", "index: contextual entry survives the round-trip");
}

BYBUS_TEST(index_legacy_layout_is_rejected) {
    TranslationIndex legacy;
    std::string error;
    check(!legacy.load(legacy_layout_bytes(), &error), "index: per-group count layout is rejected");

    CompiledTranslationIndex index;
    bybus::translation::put_ordered(&index.unique_entries, &index.unique_order, "Haste", "Rapid");
    std::string bytes;
    check(bybus::translation::write_translation_index(index, &bytes, &error), "index: trailing fixture");
    bytes.push_back('\0');
    TranslationIndex trailing;
    check(!trailing.load(bytes, &error), "index: trailing byte is rejected");
}

BYBUS_TEST(index_rejects_encoding_damaged_records) {
    const std::string damaged = "Haste" + replacement_character();
    std::vector<IndexRecord> records;
    records.push_back({"Localize/en/UI/Bufs.json", "", "name", damaged, "Rapid", "@skill:1"});
    records.push_back({"Localize/en/UI/Bufs.json", "", "name", "Haste", "Rapid", "@skill:1"});
    const CompiledTranslationIndex compiled =
        bybus::translation::compile_translation_index(records, "test", "archive");
    check(compiled.unique_entries.find(damaged) == compiled.unique_entries.end(),
          "index: U+FFFD source is not indexed");
    check(compiled.unique_entries.find("Haste") != compiled.unique_entries.end(),
          "index: clean source is still indexed");
    check(compiled.source_record_count == 1u,
          "index: U+FFFD record is not counted as a source record");
}

BYBUS_TEST(index_deduplicates_on_all_six_fields) {
    std::vector<IndexRecord> distinct_context;
    distinct_context.push_back({"Localize/en/UI/A.json", "ctx-a", "name", "Burn", "Scorch", "@skill:2"});
    distinct_context.push_back({"Localize/en/UI/A.json", "ctx-b", "name", "Burn", "Scorch", "@skill:2"});
    check(bybus::translation::compile_translation_index(distinct_context, "test", "archive")
                  .source_record_count == 2u,
          "index: records differing only in context both survive");

    std::vector<IndexRecord> same_context;
    same_context.push_back({"Localize/en/UI/A.json", "ctx-a", "name", "Burn", "Scorch", "@skill:2"});
    same_context.push_back({"Localize/en/UI/A.json", "ctx-a", "name", "Burn", "Scorch", "@skill:2"});
    check(bybus::translation::compile_translation_index(same_context, "test", "archive")
                  .source_record_count == 1u,
          "index: fully identical records collapse");
}

BYBUS_TEST(index_put_ordered_keeps_first_position) {
    std::map<std::string, std::string> entries;
    std::vector<std::string> order;
    bybus::translation::put_ordered(&entries, &order, "beta", "one");
    bybus::translation::put_ordered(&entries, &order, "alpha", "two");
    bybus::translation::put_ordered(&entries, &order, "beta", "three");
    check(order.size() == 2u && order[0] == "beta" && order[1] == "alpha",
          "index: reassignment does not move a key");
    check(entries["beta"] == "three", "index: reassignment keeps the last value");
    check(entries.size() == 2u, "index: reassignment does not duplicate a key");
}
