// Offline pairing ported from app/src/main/java/com/example/limbuszhcn/
// container/TranslationJsonPairer.kt and TranslationSnapshot.kt (bd32608).
// Pure data: no filesystem, no logging, no activation side effects.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "shared/json.h"

namespace bybus::translation {

struct IndexRecord {
    std::string path;
    std::string context;
    std::string field;
    std::string source;
    std::string translation;
    std::string stable_id;
};

struct PairResult {
    std::vector<IndexRecord> records;
    int source_fields = 0;
    int matched_fields = 0;

    // Kotlin: sourceFields == 0 || matchedFields * 100 >= sourceFields * 90
    bool sufficiently_paired() const {
        return source_fields == 0 ||
               static_cast<int64_t>(matched_fields) * 100 >= static_cast<int64_t>(source_fields) * 90;
    }
};

struct MergeResult {
    bool changed = false;      // false == Kotlin returned null (no translated leaf differs)
    std::string content;       // merged JSON text, only when changed
    int source_fields = 0;
    int matched_fields = 0;
    // Populated by merge() for the translation index compiler. Empty when the
    // caller uses pair() only.
    std::vector<IndexRecord> records;
};

// Identity map original node -> translated node, mirroring IdentityHashMap.
using SnapshotBinding = std::unordered_map<const shared::Json *, shared::JsonPtr>;

// TranslationSnapshot.decode(): strips the `_bybusTextSnapshot` marker.
struct DecodedSnapshot {
    shared::JsonPtr document;
    std::string source_hash;   // empty when the marker is absent
};

bool decode_snapshot(const shared::JsonPtr &value, DecodedSnapshot *out, std::string *error);

// TranslationSnapshot.encode(): adds the marker to a clone, returns the text.
bool encode_snapshot(const std::string &original_text, const shared::JsonPtr &document,
                     std::string *out, std::string *error);

// TranslationSnapshot.bind(): non-owning null when the hashes disagree.
bool bind_snapshot(const std::string &original_text, const shared::JsonPtr &original,
                   const DecodedSnapshot &decoded, SnapshotBinding *out);

class JsonPairer {
public:
    // Collects records only (no mutation).
    static bool pair(const std::string &path, const std::string &original_json,
                     const std::string &translation_json, PairResult *out, std::string *error);

    // Collects records and produces the merged document text. changed=false when
    // no display leaf differs, matching Kotlin's null return.
    static bool merge(const std::string &path, const std::string &original_json,
                      const std::string &translation_json, MergeResult *out, std::string *error);

    // TranslationJsonPairer.releasedTexts(): font-ownership catalogue.
    static bool released_texts(const std::string &json, std::vector<std::string> *out,
                               std::string *error);

    // TranslationJsonPairer.sourceTableName(): strips JP_/EN_/KR_ prefixes.
    static std::string source_table_name(const std::string &path);
};

} // namespace bybus::translation
