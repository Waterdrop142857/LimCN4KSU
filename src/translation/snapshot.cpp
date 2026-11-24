#include "translation/snapshot.h"

#include <cstddef>
#include <string>
#include <vector>

#include "shared/json.h"
#include "shared/sha256.h"

namespace bybus::translation {
namespace {

bool starts_with(const std::string &text, const char *prefix) {
    const std::string head(prefix);
    return text.size() >= head.size() && text.compare(0, head.size(), head) == 0;
}

bool has_control(const std::string &text) {
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20) return true;
    }
    return false;
}

std::string normalize_file(const std::string &file) {
    std::string lowered;
    lowered.reserve(file.size());
    for (const char c : file) {
        lowered.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return lowered;
}

} // namespace

const char *resource_file_name(const std::string &resource) {
    const size_t at = resource.rfind('/');
    return at == std::string::npos ? resource.c_str() : resource.c_str() + at + 1;
}

bool valid_resource_name(const std::string &resource) {
    if (resource.empty() || resource.size() > 512) return false;
    if (has_control(resource)) return false;
    if (resource.find("..") != std::string::npos) return false;
    if (!starts_with(resource, "Localize/")) return false;
    const size_t language_end = resource.find('/', 9);
    if (language_end == std::string::npos) return false;
    const std::string language = resource.substr(9, language_end - 9);
    if (language != "jp" && language != "en" && language != "kr") return false;
    const std::string file = resource_file_name(resource);
    if (file.size() < 8) return false;  // JP_x.json minimum
    if (file.compare(file.size() - 5, 5, ".json") != 0) return false;
    const std::string prefix = file.substr(0, 3);
    return prefix == "JP_" || prefix == "EN_" || prefix == "KR_";
}

bool valid_table_path(const std::string &relative) {
    if (relative.size() != std::string(kTableDir).size() + 1 + 64 + 5) return false;
    if (!starts_with(relative, "tables/")) return false;
    if (relative.compare(relative.size() - 5, 5, ".json") != 0) return false;
    return shared::is_sha256_hex(relative.substr(7, 64));
}

bool valid_index_path(const std::string &relative) {
    return relative == "index/index.bin";
}

namespace {

constexpr uint64_t kMaxModuleBytes = 512ull * 1024 * 1024;

bool hex16(const std::string &text) {
    if (text.size() != 16) return false;
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

shared::JsonPtr serialize_appsealing(const AppSealingPlan &plan) {
    shared::JsonPtr root = shared::Json::object();
    root->put_int("schema", 1);
    root->put_string("family", plan.family);
    root->put_string("protectionSha256", plan.protection_sha256);
    root->put_int("moduleSize", static_cast<int64_t>(plan.module_size));
    root->put_int("textVaddr", static_cast<int64_t>(plan.text_vaddr));
    root->put_int("textSize", static_cast<int64_t>(plan.text_size));
    root->put_int("textCrc", static_cast<int64_t>(plan.text_crc));
    root->put_int("managedVaddr", static_cast<int64_t>(plan.managed_vaddr));
    root->put_int("managedSize", static_cast<int64_t>(plan.managed_size));
    root->put_int("managedCrc", static_cast<int64_t>(plan.managed_crc));
    root->put_int("rangesVaddr", static_cast<int64_t>(plan.ranges_vaddr));
    root->put_int("rangesCount", static_cast<int64_t>(plan.ranges_count));
    root->put_string("rangesFingerprint", plan.ranges_fingerprint);
    shared::JsonPtr sites = root->put_array("sites");
    for (const AppSealingSite &site : plan.sites) {
        shared::JsonPtr row = shared::Json::object();
        row->put_int("kind", static_cast<int64_t>(site.kind));
        row->put_int("vaddr", static_cast<int64_t>(site.vaddr));
        row->put_int("targetVaddr", static_cast<int64_t>(site.target_vaddr));
        row->put_int("expected", static_cast<int64_t>(site.expected));
        row->put_int("word", static_cast<int64_t>(site.word));
        row->put_bool("substitute", site.substitute);
        sites->append(row);
    }
    shared::JsonPtr detach = root->put_object("detach");
    detach->put_bool("present", plan.detach.present);
    detach->put_int("entryVaddr", static_cast<int64_t>(plan.detach.entry_vaddr));
    shared::JsonPtr detach_expected = detach->put_array("expected");
    for (const uint32_t word : plan.detach.expected) {
        detach_expected->append(shared::Json::integer(static_cast<int64_t>(word)));
    }
    shared::JsonPtr report = root->put_object("report");
    report->put_bool("present", plan.report.present);
    report->put_int("entryVaddr", static_cast<int64_t>(plan.report.entry_vaddr));
    shared::JsonPtr report_expected = report->put_array("expected");
    for (const uint32_t word : plan.report.expected) {
        report_expected->append(shared::Json::integer(static_cast<int64_t>(word)));
    }
    report->put_int("code", static_cast<int64_t>(plan.report.code));
    report->put_int("callerVaddr", static_cast<int64_t>(plan.report.caller_vaddr));
    report->put_int("patternVaddr", static_cast<int64_t>(plan.report.pattern_vaddr));
    shared::JsonPtr pattern = report->put_array("pattern");
    for (const uint32_t word : plan.report.pattern) {
        pattern->append(shared::Json::integer(static_cast<int64_t>(word)));
    }
    report->put_int("continuationVaddr", static_cast<int64_t>(plan.report.continuation_vaddr));
    root->put_int("detachCallerVaddr", static_cast<int64_t>(plan.detach_caller_vaddr));
    root->put_int("detachContinuationVaddr",
                  static_cast<int64_t>(plan.detach_continuation_vaddr));
    root->put_int("confirmCallerVaddr", static_cast<int64_t>(plan.confirm_caller_vaddr));
    root->put_int("confirmContinuationVaddr",
                  static_cast<int64_t>(plan.confirm_continuation_vaddr));
    shared::JsonPtr stack = root->put_object("stack");
    stack->put_int("recordMinBytes", static_cast<int64_t>(plan.stack.record_min_bytes));
    stack->put_int("nameOffset", static_cast<int64_t>(plan.stack.name_offset));
    stack->put_int("textSizeOffset", static_cast<int64_t>(plan.stack.text_size_offset));
    stack->put_int("managedSizeOffset", static_cast<int64_t>(plan.stack.managed_size_offset));
    stack->put_int("textCrcOffset", static_cast<int64_t>(plan.stack.text_crc_offset));
    stack->put_int("managedCrcOffset", static_cast<int64_t>(plan.stack.managed_crc_offset));
    stack->put_int("callerStackMinBytes", static_cast<int64_t>(plan.stack.caller_stack_min_bytes));
    stack->put_int("envOffset", static_cast<int64_t>(plan.stack.env_offset));
    stack->put_int("stateOffset", static_cast<int64_t>(plan.stack.state_offset));
    stack->put_int("indexOffset", static_cast<int64_t>(plan.stack.index_offset));
    stack->put_int("descriptorIndexOffset",
                   static_cast<int64_t>(plan.stack.descriptor_index_offset));
    stack->put_int("moduleOffset", static_cast<int64_t>(plan.stack.module_offset));
    stack->put_int("expectedOffset", static_cast<int64_t>(plan.stack.expected_offset));
    stack->put_int("descriptorCountOffset",
                   static_cast<int64_t>(plan.stack.descriptor_count_offset));
    stack->put_int("descriptorEntriesOffset",
                   static_cast<int64_t>(plan.stack.descriptor_entries_offset));
    stack->put_int("descriptorStride", static_cast<int64_t>(plan.stack.descriptor_stride));
    shared::JsonPtr transient = root->put_object("transient");
    transient->put_bool("present", plan.transient.present);
    transient->put_int("vaddr", static_cast<int64_t>(plan.transient.vaddr));
    transient->put_int("targetVaddr", static_cast<int64_t>(plan.transient.target_vaddr));
    transient->put_int("expectedWord", static_cast<int64_t>(plan.transient.expected_word));
    transient->put_int("branchWord", static_cast<int64_t>(plan.transient.branch_word));
    transient->put_int("sigillPcVaddr", static_cast<int64_t>(plan.transient.sigill_pc_vaddr));
    transient->put_int("sigillLrVaddr", static_cast<int64_t>(plan.transient.sigill_lr_vaddr));
    shared::JsonPtr guards = root->put_array("guards");
    for (const AppSealingGuard &guard : plan.guards) {
        shared::JsonPtr row = shared::Json::object();
        row->put_int("vaddr", static_cast<int64_t>(guard.vaddr));
        row->put_int("expected", static_cast<int64_t>(guard.expected));
        guards->append(row);
    }
    root->put_string("exactProfile", plan.exact_profile);
    root->put_bool("reportStable", plan.report_stable);
    return root;
}

bool uint_field(const shared::JsonPtr &object, const char *name, uint64_t limit, uint64_t *out) {
    const shared::JsonPtr value = object ? object->opt(name) : nullptr;
    if (!value || !value->is_integral() || value->as_int() < 0) return false;
    const uint64_t parsed = static_cast<uint64_t>(value->as_int());
    if (parsed > limit) return false;
    *out = parsed;
    return true;
}

bool word_field(const shared::JsonPtr &object, const char *name, uint32_t *out) {
    uint64_t value = 0;
    if (!uint_field(object, name, 0xffffffffull, &value)) return false;
    *out = static_cast<uint32_t>(value);
    return true;
}

bool uint32_field(const shared::JsonPtr &object, const char *name, uint64_t limit,
                  uint32_t *out) {
    uint64_t value = 0;
    if (!uint_field(object, name, limit, &value) || value > 0xffffffffull) return false;
    *out = static_cast<uint32_t>(value);
    return true;
}

bool read_words(const shared::JsonPtr &array, std::vector<uint32_t> *out) {
    if (!array || !array->is_array() || array->length() == 0 || array->length() > 8) return false;
    out->clear();
    for (size_t i = 0; i < array->length(); ++i) {
        const shared::JsonPtr value = array->at(i);
        if (!value || !value->is_integral() || value->as_int() < 0 ||
            static_cast<uint64_t>(value->as_int()) > 0xffffffffull) {
            return false;
        }
        out->push_back(static_cast<uint32_t>(value->as_int()));
    }
    return true;
}

bool parse_appsealing(const shared::JsonPtr &root, AppSealingPlan *out, std::string *error) {
    uint64_t schema = 0;
    if (!uint_field(root, "schema", 1, &schema) || schema != 1) {
        *error = "manifest_appsealing_schema";
        return false;
    }
    AppSealingPlan plan;
    const shared::JsonPtr family = root->opt("family");
    if (!family || !family->is_string() || family->as_string().empty() ||
        family->as_string().size() > 32) {
        *error = "manifest_appsealing_family";
        return false;
    }
    plan.family = family->as_string();
    const shared::JsonPtr protection = root->opt("protectionSha256");
    if (!protection || !protection->is_string() ||
        !shared::is_sha256_hex(protection->as_string())) {
        *error = "manifest_appsealing_protection";
        return false;
    }
    plan.protection_sha256 = protection->as_string();
    if (!uint_field(root, "moduleSize", kMaxModuleBytes, &plan.module_size) ||
        plan.module_size < 4096) {
        *error = "manifest_appsealing_module";
        return false;
    }
    if (!uint_field(root, "textVaddr", kMaxModuleBytes, &plan.text_vaddr) ||
        !uint_field(root, "textSize", kMaxModuleBytes, &plan.text_size) ||
        (plan.text_size == 0) || (plan.text_size % 4) != 0 || (plan.text_vaddr % 4) != 0 ||
        plan.text_vaddr + plan.text_size > kMaxModuleBytes ||
        !word_field(root, "textCrc", &plan.text_crc)) {
        *error = "manifest_appsealing_text";
        return false;
    }
    if (!uint_field(root, "managedVaddr", kMaxModuleBytes, &plan.managed_vaddr) ||
        !uint_field(root, "managedSize", kMaxModuleBytes, &plan.managed_size) ||
        (plan.managed_size == 0) || (plan.managed_size % 4) != 0 ||
        (plan.managed_vaddr % 4) != 0 ||
        plan.managed_vaddr + plan.managed_size > kMaxModuleBytes ||
        !word_field(root, "managedCrc", &plan.managed_crc)) {
        *error = "manifest_appsealing_managed";
        return false;
    }
    if (!uint_field(root, "rangesVaddr", kMaxModuleBytes, &plan.ranges_vaddr) ||
        !uint32_field(root, "rangesCount", 512, &plan.ranges_count) || plan.ranges_count == 0) {
        *error = "manifest_appsealing_ranges";
        return false;
    }
    const shared::JsonPtr fingerprint = root->opt("rangesFingerprint");
    if (!fingerprint || !fingerprint->is_string() || !hex16(fingerprint->as_string())) {
        *error = "manifest_appsealing_fingerprint";
        return false;
    }
    plan.ranges_fingerprint = fingerprint->as_string();
    if (plan.ranges_vaddr + plan.ranges_count * 24ull > kMaxModuleBytes) {
        *error = "manifest_appsealing_ranges";
        return false;
    }

    const shared::JsonPtr sites = root->opt("sites");
    if (!sites || !sites->is_array() || sites->length() == 0 || sites->length() > 16) {
        *error = "manifest_appsealing_sites";
        return false;
    }
    for (size_t i = 0; i < sites->length(); ++i) {
        const shared::JsonPtr row = sites->at(i);
        if (!row || !row->is_object()) {
            *error = "manifest_appsealing_site";
            return false;
        }
        AppSealingSite site;
        uint64_t kind = 0;
        if (!uint_field(row, "kind", 2, &kind)) {
            *error = "manifest_appsealing_site";
            return false;
        }
        site.kind = static_cast<AppSealingSite::Kind>(kind);
        if (!uint_field(row, "vaddr", plan.module_size, &site.vaddr) ||
            (site.vaddr % 4) != 0 || site.vaddr + 4 > plan.module_size ||
            !word_field(row, "expected", &site.expected) ||
            !word_field(row, "word", &site.word)) {
            *error = "manifest_appsealing_site";
            return false;
        }
        if (site.kind == AppSealingSite::Kind::Branch) {
            if (!uint_field(row, "targetVaddr", plan.module_size, &site.target_vaddr) ||
                (site.target_vaddr % 4) != 0) {
                *error = "manifest_appsealing_site_target";
                return false;
            }
        }
        const shared::JsonPtr substitute = row->opt("substitute");
        if (substitute && !substitute->is_bool()) {
            *error = "manifest_appsealing_site";
            return false;
        }
        site.substitute = !substitute || substitute->as_bool();
        for (const AppSealingSite &other : plan.sites) {
            if (site.vaddr < other.vaddr + 4 && other.vaddr < site.vaddr + 4) {
                *error = "manifest_appsealing_site_overlap";
                return false;
            }
        }
        plan.sites.push_back(site);
    }

    const shared::JsonPtr detach = root->opt("detach");
    if (!detach || !detach->is_object()) {
        *error = "manifest_appsealing_detach";
        return false;
    }
    const shared::JsonPtr detach_present = detach->opt("present");
    plan.detach.present = detach_present && detach_present->is_bool() &&
                          detach_present->as_bool();
    if (!plan.detach.present ||
        !uint_field(detach, "entryVaddr", plan.module_size, &plan.detach.entry_vaddr) ||
        !read_words(detach->opt("expected"), &plan.detach.expected)) {
        *error = "manifest_appsealing_detach";
        return false;
    }

    const shared::JsonPtr report = root->opt("report");
    if (report && report->is_object()) {
        const shared::JsonPtr present = report->opt("present");
        plan.report.present = present && present->is_bool() && present->as_bool();
        if (plan.report.present) {
            if (!uint_field(report, "entryVaddr", plan.module_size, &plan.report.entry_vaddr) ||
                !read_words(report->opt("expected"), &plan.report.expected) ||
                !word_field(report, "code", &plan.report.code) ||
                !uint_field(report, "callerVaddr", plan.module_size, &plan.report.caller_vaddr) ||
                !uint_field(report, "patternVaddr", plan.module_size,
                            &plan.report.pattern_vaddr) ||
                !read_words(report->opt("pattern"), &plan.report.pattern) ||
                !uint_field(report, "continuationVaddr", plan.module_size,
                            &plan.report.continuation_vaddr)) {
                *error = "manifest_appsealing_report";
                return false;
            }
        }
    }

    if (!uint_field(root, "detachCallerVaddr", plan.module_size, &plan.detach_caller_vaddr) ||
        !uint_field(root, "detachContinuationVaddr", plan.module_size,
                    &plan.detach_continuation_vaddr) ||
        !uint_field(root, "confirmCallerVaddr", plan.module_size, &plan.confirm_caller_vaddr) ||
        !uint_field(root, "confirmContinuationVaddr", plan.module_size,
                    &plan.confirm_continuation_vaddr)) {
        *error = "manifest_appsealing_caller";
        return false;
    }

    const shared::JsonPtr stack = root->opt("stack");
    if (!stack || !stack->is_object()) {
        *error = "manifest_appsealing_stack";
        return false;
    }
    AppSealingStack layout;
    if (!uint_field(stack, "recordMinBytes", 8192, &layout.record_min_bytes) ||
        !uint_field(stack, "nameOffset", layout.record_min_bytes, &layout.name_offset) ||
        !uint_field(stack, "textSizeOffset", layout.record_min_bytes, &layout.text_size_offset) ||
        !uint_field(stack, "managedSizeOffset", layout.record_min_bytes,
                    &layout.managed_size_offset) ||
        !uint_field(stack, "textCrcOffset", layout.record_min_bytes, &layout.text_crc_offset) ||
        !uint_field(stack, "managedCrcOffset", layout.record_min_bytes,
                    &layout.managed_crc_offset) ||
        !uint_field(stack, "callerStackMinBytes", 65536, &layout.caller_stack_min_bytes) ||
        !uint_field(stack, "envOffset", layout.caller_stack_min_bytes, &layout.env_offset) ||
        !uint_field(stack, "stateOffset", layout.caller_stack_min_bytes, &layout.state_offset) ||
        !uint_field(stack, "indexOffset", layout.caller_stack_min_bytes, &layout.index_offset) ||
        !uint_field(stack, "descriptorIndexOffset", layout.caller_stack_min_bytes,
                    &layout.descriptor_index_offset) ||
        !uint_field(stack, "moduleOffset", layout.caller_stack_min_bytes,
                    &layout.module_offset) ||
        !uint_field(stack, "expectedOffset", layout.caller_stack_min_bytes,
                    &layout.expected_offset) ||
        !uint32_field(stack, "descriptorCountOffset", 64, &layout.descriptor_count_offset) ||
        !uint32_field(stack, "descriptorEntriesOffset", 64,
                      &layout.descriptor_entries_offset) ||
        !uint32_field(stack, "descriptorStride", 24, &layout.descriptor_stride) ||
        layout.descriptor_stride != 24 ||
        layout.caller_stack_min_bytes < layout.record_min_bytes) {
        *error = "manifest_appsealing_stack";
        return false;
    }
    const auto fits = [](uint64_t offset, uint64_t width, uint64_t limit) {
        return offset <= limit && width <= limit - offset;
    };
    if (!fits(layout.name_offset, 14, layout.record_min_bytes) ||
        !fits(layout.text_size_offset, 8, layout.record_min_bytes) ||
        !fits(layout.managed_size_offset, 8, layout.record_min_bytes) ||
        !fits(layout.text_crc_offset, 4, layout.record_min_bytes) ||
        !fits(layout.managed_crc_offset, 4, layout.record_min_bytes) ||
        !fits(layout.env_offset, 8, layout.caller_stack_min_bytes) ||
        !fits(layout.state_offset, 4, layout.caller_stack_min_bytes) ||
        !fits(layout.index_offset, 4, layout.caller_stack_min_bytes) ||
        !fits(layout.descriptor_index_offset, 4, layout.caller_stack_min_bytes) ||
        !fits(layout.module_offset, 8, layout.caller_stack_min_bytes) ||
        !fits(layout.expected_offset, 4, layout.caller_stack_min_bytes) ||
        !fits(layout.descriptor_count_offset, 4, layout.descriptor_stride) ||
        layout.descriptor_entries_offset < layout.descriptor_count_offset + 4 ||
        layout.descriptor_entries_offset != layout.descriptor_stride) {
        *error = "manifest_appsealing_stack";
        return false;
    }
    plan.stack = layout;

    const shared::JsonPtr transient = root->opt("transient");
    if (transient && transient->is_object()) {
        const shared::JsonPtr present = transient->opt("present");
        plan.transient.present = present && present->is_bool() && present->as_bool();
        if (plan.transient.present) {
            if (!uint_field(transient, "vaddr", plan.module_size, &plan.transient.vaddr) ||
                (plan.transient.vaddr % 4) != 0 ||
                !uint_field(transient, "targetVaddr", plan.module_size,
                            &plan.transient.target_vaddr) ||
                (plan.transient.target_vaddr % 4) != 0 ||
                !word_field(transient, "expectedWord", &plan.transient.expected_word) ||
                !word_field(transient, "branchWord", &plan.transient.branch_word) ||
                !uint_field(transient, "sigillPcVaddr", plan.module_size,
                            &plan.transient.sigill_pc_vaddr) ||
                !uint_field(transient, "sigillLrVaddr", plan.module_size,
                            &plan.transient.sigill_lr_vaddr)) {
                *error = "manifest_appsealing_transient";
                return false;
            }
            bool site_matches = false;
            for (const AppSealingSite &site : plan.sites) {
                if (site.vaddr == plan.transient.vaddr && !site.substitute) site_matches = true;
            }
            if (!site_matches) {
                *error = "manifest_appsealing_transient_site";
                return false;
            }
        }
    }

    const shared::JsonPtr guards = root->opt("guards");
    if (guards && guards->is_array()) {
        if (guards->length() > 32) {
            *error = "manifest_appsealing_guards";
            return false;
        }
        for (size_t i = 0; i < guards->length(); ++i) {
            const shared::JsonPtr row = guards->at(i);
            if (!row || !row->is_object()) {
                *error = "manifest_appsealing_guards";
                return false;
            }
            AppSealingGuard guard;
            if (!uint_field(row, "vaddr", plan.module_size, &guard.vaddr) ||
                !word_field(row, "expected", &guard.expected)) {
                *error = "manifest_appsealing_guards";
                return false;
            }
            plan.guards.push_back(guard);
        }
    }
    const shared::JsonPtr exact = root->opt("exactProfile");
    if (exact && (!exact->is_string() || exact->as_string().size() > 64)) {
        *error = "manifest_appsealing_exact";
        return false;
    }
    if (exact) plan.exact_profile = exact->as_string();
    const shared::JsonPtr stable = root->opt("reportStable");
    plan.report_stable = plan.report.present;
    if (stable && stable->is_bool()) plan.report_stable = stable->as_bool();

    plan.present = true;
    *out = std::move(plan);
    return true;
}

} // namespace


std::string serialize_manifest(const SnapshotManifest &manifest) {
    shared::JsonPtr root = shared::Json::object();
    root->put_int("schema", manifest.schema);
    root->put_string("generation", manifest.generation);
    root->put_string("sourceId", manifest.source_id);
    root->put_string("archiveSha256", manifest.archive_sha256);
    root->put_int("gameVersionCode", manifest.game_version_code);
    root->put_string("gameInstallIdentity", manifest.game_install_identity);
    if (!manifest.font_sha256.empty()) root->put_string("fontSha256", manifest.font_sha256);
    if (manifest.index.present) {
        shared::JsonPtr index = root->put_object("index");
        index->put_int("schema", static_cast<int64_t>(manifest.index.schema));
        index->put_string("path", manifest.index.path);
        index->put_string("sha256", manifest.index.sha256);
        index->put_int("bytes", static_cast<int64_t>(manifest.index.bytes));
        index->put_int("unique", static_cast<int64_t>(manifest.index.unique));
        index->put_int("terms", static_cast<int64_t>(manifest.index.terms));
        index->put_int("contextual", static_cast<int64_t>(manifest.index.contextual));
    }
    if (manifest.profile.present) {
        shared::JsonPtr profile = root->put_object("profile");
        profile->put_string("il2cppSha256", manifest.profile.il2cpp_sha256);
        profile->put_string("protectionSha256", manifest.profile.protection_sha256);
        profile->put_int("textOffset", static_cast<int64_t>(manifest.profile.text_offset));
        profile->put_int("textSize", static_cast<int64_t>(manifest.profile.text_size));
        profile->put_int("textCrc", static_cast<int64_t>(manifest.profile.text_crc));
        profile->put_int("textVaddr", static_cast<int64_t>(manifest.profile.text_vaddr));
        profile->put_int("managedVaddr", static_cast<int64_t>(manifest.profile.managed_vaddr));
        profile->put_int("managedSize", static_cast<int64_t>(manifest.profile.managed_size));
        profile->put_int("managedCrc", static_cast<int64_t>(manifest.profile.managed_crc));
        profile->put_int("rodataOffset", static_cast<int64_t>(manifest.profile.rodata_offset));
        profile->put_int("rodataSize", static_cast<int64_t>(manifest.profile.rodata_size));
        profile->put_int("rodataCrc", static_cast<int64_t>(manifest.profile.rodata_crc));
        if (manifest.profile.appsealing.present) {
            profile->put("appsealing", serialize_appsealing(manifest.profile.appsealing));
        }
    }
    shared::JsonPtr entries = root->put_array("entries");
    for (const SnapshotEntry &entry : manifest.entries) {
        shared::JsonPtr row = shared::Json::object();
        row->put_string("resource", entry.resource);
        row->put_string("originalSha256", entry.original_sha256);
        row->put_string("mergedPath", entry.merged_path);
        row->put_string("mergedSha256", entry.merged_sha256);
        row->put_int("mergedBytes", static_cast<int64_t>(entry.merged_bytes));
        entries->append(row);
    }
    return root->dump();
}

std::string appsealing_digest(const AppSealingPlan &plan) {
    if (!plan.present) return std::string("none");
    return shared::Sha256::hex(serialize_appsealing(plan)->dump());
}

bool parse_manifest(const std::string &text, SnapshotManifest *out, std::string *error) {
    const shared::ParseOutcome parsed = shared::parse(text);
    if (!parsed.ok || !parsed.value || !parsed.value->is_object()) {
        *error = "manifest_not_object";
        return false;
    }
    const shared::JsonPtr root = parsed.value;
    const shared::JsonPtr schema = root->opt("schema");
    if (!schema || !schema->is_integral() || schema->as_int() != kSnapshotSchema) {
        *error = "manifest_schema";
        return false;
    }
    SnapshotManifest manifest;
    manifest.schema = kSnapshotSchema;
    const shared::JsonPtr generation = root->opt("generation");
    if (!generation || !generation->is_string() ||
        !shared::is_sha256_hex(generation->as_string())) {
        *error = "manifest_generation";
        return false;
    }
    manifest.generation = generation->as_string();
    const shared::JsonPtr source = root->opt("sourceId");
    if (!source || !source->is_string() || source->as_string().empty() ||
        source->as_string().size() > 64) {
        *error = "manifest_source";
        return false;
    }
    manifest.source_id = source->as_string();
    const shared::JsonPtr archive = root->opt("archiveSha256");
    if (!archive || !archive->is_string() || !shared::is_sha256_hex(archive->as_string())) {
        *error = "manifest_archive";
        return false;
    }
    manifest.archive_sha256 = archive->as_string();
    const shared::JsonPtr version = root->opt("gameVersionCode");
    manifest.game_version_code = version && version->is_integral() ? version->as_int() : 0;
    const shared::JsonPtr identity = root->opt("gameInstallIdentity");
    if (!identity || !identity->is_string() ||
        !shared::is_sha256_hex(identity->as_string())) {
        *error = "manifest_identity";
        return false;
    }
    manifest.game_install_identity = identity->as_string();
    const shared::JsonPtr font = root->opt("fontSha256");
    if (font && font->is_string() && shared::is_sha256_hex(font->as_string())) {
        manifest.font_sha256 = font->as_string();
    }
    const shared::JsonPtr index = root->opt("index");
    if (index && index->is_object()) {
        uint64_t index_schema = 0;
        const shared::JsonPtr path = index->opt("path");
        const shared::JsonPtr digest = index->opt("sha256");
        uint64_t bytes = 0;
        uint64_t unique = 0;
        uint64_t terms = 0;
        uint64_t contextual = 0;
        if (!uint_field(index, "schema", 9, &index_schema) || index_schema != 9 || !path ||
            !path->is_string() || !valid_index_path(path->as_string()) || !digest ||
            !digest->is_string() || !shared::is_sha256_hex(digest->as_string()) ||
            !uint_field(index, "bytes", 64ull * 1024 * 1024, &bytes) || bytes == 0 ||
            !uint_field(index, "unique", 200000, &unique) ||
            !uint_field(index, "terms", 200000, &terms) ||
            !uint_field(index, "contextual", 200000, &contextual)) {
            *error = "manifest_index";
            return false;
        }
        manifest.index.present = true;
        manifest.index.schema = 9;
        manifest.index.path = path->as_string();
        manifest.index.sha256 = digest->as_string();
        manifest.index.bytes = bytes;
        manifest.index.unique = static_cast<uint32_t>(unique);
        manifest.index.terms = static_cast<uint32_t>(terms);
        manifest.index.contextual = static_cast<uint32_t>(contextual);
    }
    const shared::JsonPtr profile = root->opt("profile");
    if (profile && profile->is_object()) {
        const shared::JsonPtr il2cpp = profile->opt("il2cppSha256");
        const shared::JsonPtr text_offset = profile->opt("textOffset");
        const shared::JsonPtr text_size = profile->opt("textSize");
        const shared::JsonPtr text_crc = profile->opt("textCrc");
        if (!il2cpp || !il2cpp->is_string() || !shared::is_sha256_hex(il2cpp->as_string()) ||
            !text_offset || !text_offset->is_integral() || text_offset->as_int() < 0 ||
            !text_size || !text_size->is_integral() || text_size->as_int() <= 0 ||
            !text_crc || !text_crc->is_integral()) {
            *error = "manifest_profile";
            return false;
        }
        manifest.profile.present = true;
        manifest.profile.il2cpp_sha256 = il2cpp->as_string();
        manifest.profile.text_offset = static_cast<uint64_t>(text_offset->as_int());
        manifest.profile.text_size = static_cast<uint64_t>(text_size->as_int());
        manifest.profile.text_crc = static_cast<uint32_t>(text_crc->as_int());
        const shared::JsonPtr text_vaddr = profile->opt("textVaddr");
        if (text_vaddr && text_vaddr->is_integral() && text_vaddr->as_int() >= 0) {
            manifest.profile.text_vaddr = static_cast<uint64_t>(text_vaddr->as_int());
        }
        const shared::JsonPtr managed_vaddr = profile->opt("managedVaddr");
        const shared::JsonPtr managed_size = profile->opt("managedSize");
        const shared::JsonPtr managed_crc = profile->opt("managedCrc");
        if (managed_vaddr && managed_vaddr->is_integral() && managed_vaddr->as_int() >= 0 &&
            managed_size && managed_size->is_integral() && managed_size->as_int() > 0 &&
            managed_crc && managed_crc->is_integral()) {
            manifest.profile.managed_vaddr = static_cast<uint64_t>(managed_vaddr->as_int());
            manifest.profile.managed_size = static_cast<uint64_t>(managed_size->as_int());
            manifest.profile.managed_crc = static_cast<uint32_t>(managed_crc->as_int());
        }
        const shared::JsonPtr protection = profile->opt("protectionSha256");
        if (protection && protection->is_string() &&
            shared::is_sha256_hex(protection->as_string())) {
            manifest.profile.protection_sha256 = protection->as_string();
        }
        const shared::JsonPtr rodata_offset = profile->opt("rodataOffset");
        const shared::JsonPtr rodata_size = profile->opt("rodataSize");
        const shared::JsonPtr rodata_crc = profile->opt("rodataCrc");
        if (rodata_offset && rodata_offset->is_integral() && rodata_offset->as_int() >= 0 &&
            rodata_size && rodata_size->is_integral() && rodata_size->as_int() > 0 &&
            rodata_crc && rodata_crc->is_integral()) {
            manifest.profile.rodata_offset = static_cast<uint64_t>(rodata_offset->as_int());
            manifest.profile.rodata_size = static_cast<uint64_t>(rodata_size->as_int());
            manifest.profile.rodata_crc = static_cast<uint32_t>(rodata_crc->as_int());
        }
        const shared::JsonPtr appsealing = profile->opt("appsealing");
        if (appsealing && appsealing->is_object()) {
            if (!parse_appsealing(appsealing, &manifest.profile.appsealing, error)) return false;
            if (manifest.profile.appsealing.protection_sha256 !=
                manifest.profile.protection_sha256) {
                *error = "manifest_appsealing_protection";
                return false;
            }
        }
    }
    const shared::JsonPtr entries = root->opt("entries");
    if (!entries || !entries->is_array()) {
        *error = "manifest_entries";
        return false;
    }
    manifest.entries.reserve(entries->length());
    for (size_t i = 0; i < entries->length(); ++i) {
        const shared::JsonPtr row = entries->at(i);
        if (!row || !row->is_object()) {
            *error = "manifest_entry_type";
            return false;
        }
        SnapshotEntry entry;
        const shared::JsonPtr resource = row->opt("resource");
        if (!resource || !resource->is_string() || !valid_resource_name(resource->as_string())) {
            *error = "manifest_entry_resource";
            return false;
        }
        entry.resource = resource->as_string();
        const shared::JsonPtr original = row->opt("originalSha256");
        if (!original || !original->is_string() ||
            !shared::is_sha256_hex(original->as_string())) {
            *error = "manifest_entry_original";
            return false;
        }
        entry.original_sha256 = original->as_string();
        const shared::JsonPtr merged = row->opt("mergedPath");
        if (!merged || !merged->is_string() || !valid_table_path(merged->as_string())) {
            *error = "manifest_entry_path";
            return false;
        }
        entry.merged_path = merged->as_string();
        const shared::JsonPtr merged_digest = row->opt("mergedSha256");
        if (!merged_digest || !merged_digest->is_string() ||
            !shared::is_sha256_hex(merged_digest->as_string())) {
            *error = "manifest_entry_digest";
            return false;
        }
        entry.merged_sha256 = merged_digest->as_string();
        // The stored file name must equal its content digest.
        if (entry.merged_path.substr(7, 64) != entry.merged_sha256) {
            *error = "manifest_entry_digest_path";
            return false;
        }
        const shared::JsonPtr bytes = row->opt("mergedBytes");
        if (!bytes || !bytes->is_integral() || bytes->as_int() < 0) {
            *error = "manifest_entry_bytes";
            return false;
        }
        entry.merged_bytes = static_cast<uint64_t>(bytes->as_int());
        manifest.entries.push_back(std::move(entry));
    }
    *out = std::move(manifest);
    return true;
}

bool SnapshotIndex::load(const SnapshotManifest &manifest, std::string *error) {
    entries_ = manifest.entries;
    by_resource_.clear();
    by_file_.clear();
    for (size_t i = 0; i < entries_.size(); ++i) {
        const SnapshotEntry &entry = entries_[i];
        std::string key = entry.resource;
        for (char &c : key) {
            c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }
        if (by_resource_.find(key) != by_resource_.end()) {
            *error = "snapshot_duplicate_resource";
            return false;
        }
        by_resource_[key].push_back(i);
        by_file_[normalize_file(resource_file_name(entry.resource))].push_back(i);
    }
    return true;
}

const SnapshotEntry *SnapshotIndex::find(const std::string &resource_key,
                                         const std::string &original_sha256) const {
    if (original_sha256.size() != 64) return nullptr;
    std::string key = resource_key;
    for (char &c : key) {
        c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    const auto exact = by_resource_.find(key);
    if (exact != by_resource_.end()) {
        for (const size_t index : exact->second) {
            if (entries_[index].original_sha256 == original_sha256) return &entries_[index];
        }
    }
    const std::string file = normalize_file(resource_file_name(resource_key));
    const auto by_file = by_file_.find(file);
    if (by_file == by_file_.end() || by_file->second.size() != 1) return nullptr;
    const SnapshotEntry &candidate = entries_[by_file->second.front()];
    return candidate.original_sha256 == original_sha256 ? &candidate : nullptr;
}

} // namespace bybus::translation


