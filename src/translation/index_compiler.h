// Ported from app/src/main/java/com/example/limbuszhcn/container/
// TranslationIndexCompiler.kt (bd32608). Pure data: deterministic unique,
// term and contextual indexes written as the original schema-9 binary.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "translation/json_pairer.h"

namespace bybus::translation {

struct CompiledTranslationIndex {
    std::string version;
    std::string archive_sha256;
    std::string input_sha256;
    std::map<std::string, std::string> unique_entries;
    std::map<std::string, std::string> term_entries;
    std::map<std::string, std::string> contextual_entries;
    // The reference builds the three maps with linkedMapOf, so the binary keeps
    // first-insertion order instead of std::map's lexical order. A reassigned key
    // keeps its original slot; only a new key is appended.
    std::vector<std::string> unique_order;
    std::vector<std::string> term_order;
    std::vector<std::string> contextual_order;
    uint32_t source_record_count = 0;
    uint32_t conflict_count = 0;
    uint32_t dominant_entry_count = 0;
    uint32_t candidate_source_file_count = 0;
    uint32_t available_source_file_count = 0;
};

// Kotlin linkedMapOf semantics for one compiled map: the key is appended to
// `order` only the first time it appears, then the value is assigned.
void put_ordered(std::map<std::string, std::string> *entries,
                 std::vector<std::string> *order, const std::string &key,
                 const std::string &value);

// Applies the original display/term filters, conflict resolution and
// deterministic digest. Records not usable as display text are ignored.
CompiledTranslationIndex compile_translation_index(
    const std::vector<IndexRecord> &records, const std::string &version,
    const std::string &archive_sha256, uint32_t candidate_source_files = 0,
    uint32_t available_source_files = 0);

// Original binary layout: "LZTI1\0", schema 9, then the three map counts as
// big-endian u32, then every unique, term and contextual pair in insertion order
// (each one as a sized UTF-8 key followed by a sized UTF-8 value). Returns false
// with a fixed reason on any bound violation.
bool write_translation_index(const CompiledTranslationIndex &index, std::string *out,
                             std::string *error);

} // namespace bybus::translation
