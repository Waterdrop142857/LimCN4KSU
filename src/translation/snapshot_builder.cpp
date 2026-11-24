#include "translation/snapshot_builder.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "shared/fs.h"
#include "shared/sha256.h"
#include "translation/appsealing_profile.h"
#include "translation/index_compiler.h"
#include "translation/elf_profile.h"
#include "translation/json_pairer.h"

namespace bybus::translation {
namespace {

constexpr const char *kOfficialSigner =
    "9ea88fdc27195dfef99050f88a889cad86cdf1a80faca2e22d6464d47f973145";


bool starts_with(const std::string &text, const char *prefix) {
    const std::string head(prefix);
    return text.size() >= head.size() && text.compare(0, head.size(), head) == 0;
}

std::string basename_of(const std::string &path) {
    const size_t at = path.rfind('/');
    return at == std::string::npos ? path : path.substr(at + 1);
}

std::string dirname_of(const std::string &path) {
    const size_t at = path.rfind('/');
    return at == std::string::npos ? std::string() : path.substr(0, at);
}

// Packs are keyed by their "en/<folder>/EN_<file>" identity, the same mapping
// LocalTranslationArchive.mapResource() applies to archive entries.
std::string pack_key_for_resource(const std::string &resource) {
    const std::string prefix = "Localize/";
    if (!starts_with(resource, prefix.c_str())) return std::string();
    const size_t language_end = resource.find('/', prefix.size());
    if (language_end == std::string::npos) return std::string();
    const std::string rest = resource.substr(language_end + 1);
    const std::string folder = dirname_of(rest);
    std::string file = basename_of(rest);
    if (file.size() > 3) {
        const std::string head = file.substr(0, 3);
        if (head == "JP_" || head == "EN_" || head == "KR_") file = file.substr(3);
    }
    return (folder.empty() ? std::string() : folder + "/") + "EN_" + file;
}

std::string lower_ascii(std::string text) {
    for (char &c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

std::string canonical_inputs(const BuildInputs &inputs, const SnapshotManifest &manifest) {
    std::vector<std::string> lines;
    lines.emplace_back("bybus-snapshot-v1");
    lines.emplace_back("source " + inputs.source_id);
    lines.emplace_back("archive " + inputs.archive_sha256);
    lines.emplace_back("identity " + inputs.game_install_identity);
    lines.emplace_back("version " + std::to_string(inputs.game_version_code));
    lines.emplace_back("font " + (manifest.font_sha256.empty() ? std::string("none")
                                                               : manifest.font_sha256));
    lines.emplace_back("profile " +
                       (manifest.profile.present
                            ? manifest.profile.il2cpp_sha256 + " " +
                                  std::to_string(manifest.profile.text_offset) + " " +
                                  std::to_string(manifest.profile.text_size) + " " +
                                  std::to_string(manifest.profile.text_crc)
                            : std::string("none")));
    lines.emplace_back("appsealing " + appsealing_digest(manifest.profile.appsealing));
    lines.emplace_back("index " +
                       (manifest.index.present
                            ? manifest.index.sha256 + " " + std::to_string(manifest.index.unique) +
                                  " " + std::to_string(manifest.index.terms) + " " +
                                  std::to_string(manifest.index.contextual)
                            : std::string("none")));
    for (const SnapshotEntry &entry : manifest.entries) {
        lines.push_back("entry " + entry.resource + " " + entry.original_sha256 + " " +
                        entry.merged_sha256 + " " + std::to_string(entry.merged_bytes));
    }
    std::string joined;
    for (const std::string &line : lines) {
        joined += line;
        joined.push_back('\n');
    }
    return joined;
}

BuildOutcome failure(shared::Code code, const std::string &detail) {
    BuildOutcome outcome;
    outcome.code = code;
    outcome.detail = detail;
    return outcome;
}

} // namespace

BuildOutcome build_generation(const BuildInputs &inputs) {
    if (!shared::is_sha256_hex(inputs.archive_sha256) ||
        !shared::is_sha256_hex(inputs.game_install_identity) || inputs.source_id.empty()) {
        return failure(shared::Code::InvalidArgument, "build_inputs");
    }
    if (inputs.page_size != 0) {
        if (inputs.sdk < 26) return failure(shared::Code::UnsupportedRuntime, "qualification_sdk");
        if (inputs.abi.find("arm64-v8a") == std::string::npos) {
            return failure(shared::Code::UnsupportedRuntime, "qualification_abi");
        }
        if (!inputs.native_alignment_ok) {
            return failure(shared::Code::UnsupportedRuntime,
                           "qualification_native_" + inputs.native_alignment_error);
        }
        if (inputs.signer_sha256 != kOfficialSigner) {
            return failure(shared::Code::UnsupportedRuntime, "qualification_signer");
        }
    }
    std::unordered_map<std::string, const ImportedTable *> pack;
    pack.reserve(inputs.pack.size());
    for (const ImportedTable &table : inputs.pack) {
        pack.emplace(lower_ascii(table.resource_name), &table);
    }

    SnapshotManifest manifest;
    manifest.source_id = inputs.source_id;
    manifest.archive_sha256 = inputs.archive_sha256;
    manifest.game_version_code = inputs.game_version_code;
    manifest.game_install_identity = inputs.game_install_identity;

    // AppSealing layout of this exact build. A missing profile never fails the
    // build: the snapshot still publishes and the runtime refuses to patch.
    std::vector<std::string> apk_paths = inputs.game_apk_paths;
    if (apk_paths.empty() && !inputs.game_apk_path.empty()) {
        apk_paths.push_back(inputs.game_apk_path);
    }
    for (const std::string &apk_path : apk_paths) {
        const IntegrityProfileData collected =
            collect_integrity_profile(apk_path, inputs.game_version_code);
        if (collected.present) {
            manifest.profile.present = true;
            manifest.profile.il2cpp_sha256 = collected.il2cpp_sha256;
            manifest.profile.protection_sha256 = collected.protection_sha256;
            manifest.profile.text_offset = collected.text_offset;
            manifest.profile.text_size = collected.text_size;
            manifest.profile.text_crc = collected.text_crc;
            manifest.profile.text_vaddr = collected.text_vaddr;
            manifest.profile.managed_vaddr = collected.managed_vaddr;
            manifest.profile.managed_size = collected.managed_size;
            manifest.profile.managed_crc = collected.managed_crc;
            manifest.profile.appsealing = collected.appsealing;
            manifest.profile.rodata_offset = collected.rodata_offset;
            manifest.profile.rodata_size = collected.rodata_size;
            manifest.profile.rodata_crc = collected.rodata_crc;
            break;
        }
    }

    const std::string table_dir = inputs.work_dir + "/" + kTableDir;
    const shared::Code created = shared::make_dirs(table_dir, 0700);
    if (created != shared::Code::Ok) return failure(created, "build_mkdir");

    BuildOutcome outcome;
    std::unordered_map<std::string, std::string> written;  // digest -> file name
    std::vector<IndexRecord> index_records;
    // ContainerPatchInstaller.kt:236-238: a source file counts as available once
    // its pairing covers 90% of the display fields. The key is the canonical
    // GameTable resource name, the same identity the import side uses.
    std::unordered_set<std::string> paired_resources;
    for (const GameTable &original : inputs.originals) {
        if (!valid_resource_name(original.resource)) {
            ++outcome.skipped;
            continue;
        }
        const std::string key = pack_key_for_resource(original.resource);
        const auto found = pack.find(lower_ascii(key));
        if (found == pack.end()) {
            ++outcome.skipped;
            continue;
        }
        MergeResult merged;
        std::string error;
        if (!JsonPairer::merge(original.resource, original.content, found->second->content, &merged,
                               &error)) {
            // Unparseable or loader-mismatched original: keep the game text.
            ++outcome.skipped;
            continue;
        }
        if (merged.sufficiently_paired()) paired_resources.insert(original.resource);
        outcome.matched_fields += static_cast<uint32_t>(merged.matched_fields);
        outcome.source_fields += static_cast<uint32_t>(merged.source_fields);
        index_records.insert(index_records.end(), merged.records.begin(), merged.records.end());
        if (!merged.changed) {
            ++outcome.skipped;
            continue;
        }
        const std::string digest = shared::Sha256::hex(merged.content);
        std::string file_name = digest + ".json";
        const auto already = written.find(digest);
        if (already == written.end()) {
            const shared::Code wrote =
                shared::write_file(table_dir + "/" + file_name, merged.content);
            if (wrote != shared::Code::Ok) return failure(wrote, "build_write_table");
            written.emplace(digest, file_name);
        } else {
            file_name = already->second;
        }
        SnapshotEntry entry;
        entry.resource = original.resource;
        entry.original_sha256 = shared::Sha256::hex(original.content);
        entry.merged_path = std::string(kTableDir) + "/" + file_name;
        entry.merged_sha256 = digest;
        entry.merged_bytes = merged.content.size();
        manifest.entries.push_back(std::move(entry));
        ++outcome.tables;
    }

    if (manifest.entries.empty()) {
        return failure(shared::Code::SnapshotInvalid, "build_no_tables");
    }
    std::sort(manifest.entries.begin(), manifest.entries.end(),
              [](const SnapshotEntry &left, const SnapshotEntry &right) {
                  return left.resource < right.resource;
              });

    if (!inputs.font_source_path.empty()) {
        std::string font;
        const shared::Code read =
            shared::read_file(inputs.font_source_path, UINT64_MAX, &font);
        if (read != shared::Code::Ok) return failure(shared::Code::FontInvalid, "build_font_read");
        const std::string font_dir = inputs.work_dir + "/font";
        if (shared::make_dirs(font_dir, 0700) != shared::Code::Ok) {
            return failure(shared::Code::IoError, "build_font_dir");
        }
        const shared::Code wrote = shared::write_file(inputs.work_dir + "/" + kFontRelativePath, font);
        if (wrote != shared::Code::Ok) return failure(shared::Code::FontInvalid, "build_font_write");
        manifest.font_sha256 = shared::Sha256::hex(font);
    }

    // ContainerPatchInstaller.kt:258 / :430: 90% of the packed resources must
    // pair before the index is trusted, and an empty pack is never a pass.
    const int64_t packed_count = static_cast<int64_t>(inputs.pack.size());
    const int64_t available_count = static_cast<int64_t>(paired_resources.size());
    const bool resources_ready = packed_count > 0 && available_count * 100 >= packed_count * 90;
    if (resources_ready) {
        const CompiledTranslationIndex compiled = compile_translation_index(
            index_records, inputs.source_id + ":" + std::to_string(inputs.game_version_code),
            inputs.archive_sha256, static_cast<uint32_t>(inputs.pack.size()),
            static_cast<uint32_t>(paired_resources.size()));
        std::string index_bytes;
        std::string index_error;
        if (!write_translation_index(compiled, &index_bytes, &index_error)) {
            return failure(shared::Code::SnapshotInvalid, "build_index_" + index_error);
        }
        const std::string index_dir = inputs.work_dir + "/index";
        if (shared::make_dirs(index_dir, 0700) != shared::Code::Ok) {
            return failure(shared::Code::IoError, "build_index_dir");
        }
        const std::string index_path = inputs.work_dir + "/index/index.bin";
        const shared::Code wrote = shared::write_file(index_path, index_bytes);
        if (wrote != shared::Code::Ok) return failure(wrote, "build_index_write");
        manifest.index.present = true;
        manifest.index.schema = 9;
        manifest.index.path = "index/index.bin";
        manifest.index.sha256 = shared::Sha256::hex(index_bytes);
        manifest.index.bytes = index_bytes.size();
        manifest.index.unique = static_cast<uint32_t>(compiled.unique_entries.size());
        manifest.index.terms = static_cast<uint32_t>(compiled.term_entries.size());
        manifest.index.contextual = static_cast<uint32_t>(compiled.contextual_entries.size());
    } else {
        // Too few source tables paired: publish the snapshot without a runtime
        // index instead of activating a thin one.
        outcome.detail = "insufficient_pairs";
    }

    manifest.generation = shared::Sha256::hex(canonical_inputs(inputs, manifest));
    manifest.schema = kSnapshotSchema;
    const shared::Code wrote_manifest =
        shared::write_file(inputs.work_dir + "/" + kManifestName, serialize_manifest(manifest));
    if (wrote_manifest != shared::Code::Ok) {
        return failure(wrote_manifest, "build_write_manifest");
    }
    if (shared::sync_dir(inputs.work_dir) != shared::Code::Ok ||
        (manifest.index.present &&
         shared::sync_dir(inputs.work_dir + "/index") != shared::Code::Ok) ||
        shared::sync_dir(table_dir) != shared::Code::Ok) {
        return failure(shared::Code::IoError, "build_sync");
    }
    outcome.code = shared::Code::Ok;
    outcome.generation = manifest.generation;
    outcome.profile_present = manifest.profile.present;
    return outcome;
}

} // namespace bybus::translation
