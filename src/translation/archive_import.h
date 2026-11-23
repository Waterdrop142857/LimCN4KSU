// Text-pack importer ported from translation/LocalTranslationArchive.kt
// (bd32608). Validates ARC paths, sizes, UTF-8 and duplicate targets, and
// produces the in-memory table set the snapshot builder consumes. No
// filesystem writes happen here.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "shared/status.h"
#include "translation/archive_reader.h"

namespace bybus::translation {

struct ImportedTable {
    std::string raw_entry;      // path inside the archive
    std::string resource_name;  // e.g. "EN_Skills.json" or "StoryData/EN_x.json"
    std::string relative_path;  // cache layout: "files/en/<resource_name>"
    std::string content;
    uint64_t bytes = 0;
    std::string sha256;
};

struct ImportResult {
    shared::Code code = shared::Code::Ok;
    std::string detail;
    std::string archive_sha256;
    uint64_t archive_bytes = 0;
    uint32_t entry_count = 0;
    std::vector<ImportedTable> tables;
};

// Kotlin companion helpers, kept public for tests.
std::string safe_path(const std::string &raw);          // empty when invalid
std::string map_resource(const std::string &path);      // empty when not a text pack entry

// Reads the archive, applies the exclusion rules, and fills `out` on success.
shared::Code import_archive(const std::string &archive_path, ImportResult *out);

} // namespace bybus::translation
