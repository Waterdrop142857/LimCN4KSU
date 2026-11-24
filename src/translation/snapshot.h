// Generation manifest (01-implementation-design.md §4.4) plus the runtime lookup
// index. The manifest is the only cross-boundary contract between bybusd and the
// Zygisk runtime, so it is parsed strictly and validated twice: at publish time
// and again on every runtime read.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bybus::translation {

constexpr int kSnapshotSchema = 1;
constexpr const char *kManifestName = "manifest.json";
constexpr const char *kTableDir = "tables";
constexpr const char *kFontRelativePath = "font/ChineseFont.ttf";

struct SnapshotEntry {
    std::string resource;        // Localize/<locale>/<path>/<LANG>_<table>.json
    std::string original_sha256; // digest of the loader-visible UTF-8 text
    std::string merged_path;     // "tables/<sha256>.json", relative to the generation
    std::string merged_sha256;
    uint64_t merged_bytes = 0;
};

struct SnapshotIndexInfo {
    bool present = false;
    uint32_t schema = 9;
    std::string path;  // "index/index.bin"
    std::string sha256;
    uint64_t bytes = 0;
    uint32_t unique = 0;
    uint32_t terms = 0;
    uint32_t contextual = 0;
};

// One AppSealing patch site. Offsets are module-relative loaded addresses; the
// values are produced by bybusd from the installed APK and re-checked against
// the live process before any byte is written.
struct AppSealingSite {
    enum class Kind : uint8_t { Branch = 0, Nop = 1, Word = 2 };
    Kind kind = Kind::Nop;
    uint64_t vaddr = 0;
    uint64_t target_vaddr = 0;  // branch only
    uint32_t expected = 0;      // original instruction word
    uint32_t word = 0;          // replacement word for Kind::Word
    bool substitute = true;     // false for the self-modifying 50048 site
};

struct AppSealingHookPoint {
    bool present = false;
    uint64_t entry_vaddr = 0;
    std::vector<uint32_t> expected;
};

struct AppSealingReport {
    bool present = false;
    uint64_t entry_vaddr = 0;
    std::vector<uint32_t> expected;
    uint32_t code = 0;
    uint64_t caller_vaddr = 0;
    uint64_t pattern_vaddr = 0;
    std::vector<uint32_t> pattern;
    uint64_t continuation_vaddr = 0;
};

struct AppSealingStack {
    uint64_t record_min_bytes = 0;
    uint64_t name_offset = 0;
    uint64_t text_size_offset = 0;
    uint64_t managed_size_offset = 0;
    uint64_t text_crc_offset = 0;
    uint64_t managed_crc_offset = 0;
    uint64_t caller_stack_min_bytes = 0;
    uint64_t env_offset = 0;
    uint64_t state_offset = 0;
    uint64_t index_offset = 0;
    uint64_t descriptor_index_offset = 0;
    uint64_t module_offset = 0;
    uint64_t expected_offset = 0;
    uint32_t descriptor_count_offset = 0;
    uint32_t descriptor_entries_offset = 0;
    uint32_t descriptor_stride = 0;
};

struct AppSealingTransient {
    bool present = false;
    uint64_t vaddr = 0;
    uint64_t target_vaddr = 0;
    uint32_t expected_word = 0;
    uint32_t branch_word = 0;
    uint64_t sigill_pc_vaddr = 0;
    uint64_t sigill_lr_vaddr = 0;
};

struct AppSealingGuard {
    uint64_t vaddr = 0;
    uint32_t expected = 0;
};

struct AppSealingSigsegv {
    bool present = false;
    uint64_t pc_offset = 0;
    uint64_t lr_offset = 0;
    uint64_t recovery_offset = 0;
    uint32_t x1_expected = 0;
    std::vector<uint32_t> pattern;
};

// Local compatibility plan for libcovault-appsec.so. Absent means the runtime
// must not install any protection-related patch.
struct AppSealingPlan {
    bool present = false;
    std::string family;
    std::string protection_sha256;
    uint64_t module_size = 0;
    uint64_t text_vaddr = 0;
    uint64_t text_size = 0;
    uint32_t text_crc = 0;
    uint64_t managed_vaddr = 0;
    uint64_t managed_size = 0;
    uint32_t managed_crc = 0;
    uint64_t ranges_vaddr = 0;
    uint32_t ranges_count = 0;
    std::string ranges_fingerprint;
    std::vector<AppSealingSite> sites;
    AppSealingHookPoint detach;
    AppSealingReport report;
    uint64_t detach_caller_vaddr = 0;
    uint64_t detach_continuation_vaddr = 0;
    uint64_t confirm_caller_vaddr = 0;
    uint64_t confirm_continuation_vaddr = 0;
    std::vector<AppSealingGuard> guards;
    std::string exact_profile;
    bool report_stable = false;
    AppSealingStack stack;
    AppSealingTransient transient;
    AppSealingSigsegv sigsegv;
};

// AppSealing layout of the installed build, computed locally by bybusd from the
// APK. Absent means the runtime must not patch the process.
struct SnapshotProfile {
    bool present = false;
    std::string il2cpp_sha256;
    std::string protection_sha256;
    uint64_t text_offset = 0;
    uint64_t text_size = 0;
    uint32_t text_crc = 0;
    uint64_t text_vaddr = 0;
    uint64_t managed_vaddr = 0;
    uint64_t managed_size = 0;
    uint32_t managed_crc = 0;
    uint64_t rodata_offset = 0;
    uint64_t rodata_size = 0;
    uint32_t rodata_crc = 0;
    AppSealingPlan appsealing;
};

struct SnapshotManifest {
    int schema = kSnapshotSchema;
    std::string generation;
    std::string source_id;
    std::string archive_sha256;
    int64_t game_version_code = 0;
    std::string game_install_identity;
    std::string font_sha256;  // optional extension; empty when no font is published
    SnapshotProfile profile;
    SnapshotIndexInfo index;
    std::vector<SnapshotEntry> entries;
};

std::string serialize_manifest(const SnapshotManifest &manifest);

// Stable digest of one compatibility plan, used by the generation identity.
std::string appsealing_digest(const AppSealingPlan &plan);

// Strict: schema, digests, resource names, table paths and byte counts.
bool parse_manifest(const std::string &text, SnapshotManifest *out, std::string *error);

// "tables/<64 hex>.json" only.
bool valid_table_path(const std::string &relative);

// "index/index.bin" only.
bool valid_index_path(const std::string &relative);

// Canonical resource name: Localize/<jp|en|kr>/<path>/<LANG>_<table>.json
bool valid_resource_name(const std::string &resource);

const char *resource_file_name(const std::string &resource);

class SnapshotIndex {
public:
    bool load(const SnapshotManifest &manifest, std::string *error);
    // Exact (resource, digest) hit first, then a unique file-name hit.
    const SnapshotEntry *find(const std::string &resource_key,
                              const std::string &original_sha256) const;
    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

private:
    std::vector<SnapshotEntry> entries_;
    std::unordered_map<std::string, std::vector<size_t>> by_resource_;
    std::unordered_map<std::string, std::vector<size_t>> by_file_;
};

} // namespace bybus::translation
