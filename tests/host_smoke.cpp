// Host tests for the pure data layers (no filesystem, no Android APIs).
// Build and run: build/test-host.ps1  —or any C++17 compiler with -Isrc.
//
// Coverage maps to 02-code-quality-standard.md §6:
//   * pairing: display fields, duplicates, nested arrays, unknown fields
//   * catalogue: released texts only, string-array fields honoured
//   * snapshot: manifest round-trip, path/digest rejection, marker rejection
//   * shared: SHA-256 vectors; content passes through the merge unchanged
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/owned_code.h"
#include "runtime/translation_index.h"
#include "shared/json.h"
#include "shared/sha256.h"
#include "translation/appsealing_profile.h"
#include "translation/index_compiler.h"
#include "translation/json_pairer.h"
#include "translation/snapshot.h"
#include "translation/text_policy.h"

namespace {

int g_failures = 0;

void check(bool condition, const char *what) {
    if (condition) return;
    std::printf("FAIL %s\n", what);
    ++g_failures;
}

std::string value_of(const bybus::shared::JsonPtr &root, const char *field) {
    if (!root) return std::string();
    const bybus::shared::JsonPtr value = root->opt(field);
    return value && value->is_string() ? value->as_string() : std::string();
}

void test_merge_basic() {
    using namespace bybus;
    const std::string original = R"([{"id":1,"name":"Hello","model":"m1"}])";
    const std::string translated = R"([{"id":1,"name":"\u4f60\u597d","model":"m1"}])";
    translation::MergeResult merged;
    std::string error;
    check(translation::JsonPairer::merge("Localize/en/UI/EN_Test.json", original, translated,
                                        &merged, &error),
          "merge: basic succeeds");
    check(merged.changed, "merge: basic changes");
    check(merged.matched_fields == 1, "merge: one display field matched");
    const auto parsed = shared::parse(merged.content);
    check(parsed.ok && parsed.value && parsed.value->is_array(), "merge: output is an array");
    if (parsed.ok && parsed.value && parsed.value->length() == 1) {
        check(value_of(parsed.value->at(0), "name") == "\xE4\xBD\xA0\xE5\xA5\xBD",
              "merge: name translated");
        check(value_of(parsed.value->at(0), "model") == "m1", "merge: metadata untouched");
    } else {
        check(false, "merge: single row preserved");
    }
}

void test_merge_unchanged_reports_null() {
    using namespace bybus;
    const std::string same = R"([{"id":1,"name":"same"}])";
    translation::MergeResult merged;
    std::string error;
    check(translation::JsonPairer::merge("Localize/en/UI/EN_Test.json", same, same, &merged,
                                        &error),
          "merge: identical input parses");
    check(!merged.changed, "merge: identical text reports no change");
}

void test_unknown_fields_preserved() {
    using namespace bybus;
    const std::string original = R"({"id":5,"name":"a","custom":{"k":[1,2]}})";
    const std::string translated = R"({"id":5,"name":"b"})";
    translation::MergeResult merged;
    std::string error;
    check(translation::JsonPairer::merge("Localize/en/UI/EN_Test.json", original, translated,
                                        &merged, &error),
          "merge: partial translation accepted");
    const auto parsed = shared::parse(merged.content);
    check(parsed.ok && parsed.value && parsed.value->opt("custom"), "merge: unknown field kept");
}

void test_duplicate_identity_by_metadata() {
    using namespace bybus;
    const std::string original =
        R"([{"id":"7","model":"A","name":"alpha"},{"id":"7","model":"B","name":"beta"}])";
    // Deliberately reversed order: pairing must use metadata, never position.
    const std::string translated =
        R"([{"id":"7","model":"B","name":"\u4e59"},{"id":"7","model":"A","name":"\u7532"}])";
    translation::MergeResult merged;
    std::string error;
    check(translation::JsonPairer::merge("Localize/en/UI/EN_Test.json", original, translated,
                                        &merged, &error),
          "merge: duplicate ids accepted");
    const auto parsed = shared::parse(merged.content);
    check(parsed.ok && parsed.value && parsed.value->length() == 2, "merge: both rows present");
    if (parsed.ok && parsed.value && parsed.value->length() == 2) {
        check(value_of(parsed.value->at(0), "name") == "\xE7\x94\xB2",
              "merge: model A row took its own translation");
        check(value_of(parsed.value->at(1), "name") == "\xE4\xB9\x99",
              "merge: model B row took its own translation");
    }
}

void test_string_array_field() {
    using namespace bybus;
    const std::string original = R"([{"id":"9","successDesc":["one","two"]}])";
    const std::string translated = R"([{"id":"9","successDesc":["a","b"]}])";
    translation::MergeResult merged;
    std::string error;
    check(translation::JsonPairer::merge("Localize/en/UI/EN_Test.json", original, translated,
                                        &merged, &error),
          "merge: string array accepted");
    const auto parsed = shared::parse(merged.content);
    const bybus::shared::JsonPtr row = parsed.ok && parsed.value && parsed.value->length() == 1
                                           ? parsed.value->at(0)
                                           : nullptr;
    const bybus::shared::JsonPtr list = row ? row->opt("successDesc") : nullptr;
    check(list && list->is_array() && list->length() == 2, "merge: array kept its shape");
    if (list && list->length() == 2) {
        check(list->at(0)->as_string() == "a" && list->at(1)->as_string() == "b",
              "merge: array entries replaced by index");
    }
}

void test_ego_gift_abilities() {
    using namespace bybus;
    const std::string original =
        R"([{"id":100,"simpleDesc":[{"abilityID":1,"simpleDesc":"[Haste] +1"},)"
        R"({"abilityID":2,"simpleDesc":"[Power] +2"}]}])";
    const std::string translated =
        R"([{"id":100,"simpleDesc":[{"abilityID":2,"simpleDesc":"[Power] +2 X"},)"
        R"({"abilityID":1,"simpleDesc":"[Haste] +1 Y"}]}])";
    translation::MergeResult merged;
    std::string error;
    check(translation::JsonPairer::merge("Localize/jp/EGOgift_Test.json", original, translated,
                                        &merged, &error),
          "merge: ego gift accepted");
    const auto parsed = shared::parse(merged.content);
    const bybus::shared::JsonPtr row = parsed.ok && parsed.value && parsed.value->length() == 1
                                           ? parsed.value->at(0)
                                           : nullptr;
    const bybus::shared::JsonPtr list = row ? row->opt("simpleDesc") : nullptr;
    check(list && list->is_array() && list->length() == 2, "merge: gift abilities preserved");
    if (list && list->length() == 2) {
        check(value_of(list->at(0), "simpleDesc") == "[Haste] +1 Y",
              "merge: ability 1 paired by identity");
        check(value_of(list->at(1), "simpleDesc") == "[Power] +2 X",
              "merge: ability 2 paired by identity");
    }
}

void test_long_skill_context() {
    using namespace bybus;
    const std::string id(260, '7');
    const std::string original = "[{\"id\":\"" + id +
        "\",\"levelList\":[{\"level\":1,\"name\":\"old\"}]}]";
    const std::string translated = "[{\"id\":\"" + id +
        "\",\"levelList\":[{\"level\":1,\"name\":\"new\"}]}]";
    translation::PairResult paired;
    std::string error;
    check(translation::JsonPairer::pair("Localize/en/UI/Skills.json", original, translated,
                                        &paired, &error),
          "pair: long skill id parses");
    bool kept_context = false;
    for (const translation::IndexRecord &record : paired.records) {
        if (record.translation == "new" &&
            record.stable_id == "@skill:" + id + "/level:1") kept_context = true;
    }
    check(kept_context, "pair: long skill context is preserved");
}

void test_released_texts() {
    using namespace bybus;
    std::vector<std::string> texts;
    std::string error;
    const std::string json = R"({"name":"A","model":"m","successDesc":["B"],"custom":"C"})";
    check(translation::JsonPairer::released_texts(json, &texts, &error),
          "catalogue: parses");
    bool has_a = false;
    bool has_b = false;
    bool has_c = false;
    for (const std::string &text : texts) {
        if (text == "A") has_a = true;
        if (text == "B") has_b = true;
        if (text == "C") has_c = true;
    }
    check(has_a, "catalogue: display field kept");
    check(has_b, "catalogue: string-array field kept");
    check(!has_c, "catalogue: unknown field excluded");
    bool has_model = false;
    for (const std::string &text : texts) {
        if (text == "m") has_model = true;
    }
    check(!has_model, "catalogue: metadata excluded");
}

void test_snapshot_marker_rejected() {
    using namespace bybus;
    const std::string original = R"({"id":1,"name":"a"})";
    const std::string bad = R"({"_bybusTextSnapshot":{"schema":2,"sha256":"00"},)"
                           R"("id":1,"name":"b"})";
    translation::MergeResult merged;
    std::string error;
    check(!translation::JsonPairer::merge("Localize/en/UI/EN_Test.json", original, bad, &merged,
                                          &error),
          "snapshot: bad marker rejected");
}

void test_manifest_validation() {
    using namespace bybus;
    translation::SnapshotManifest manifest;
    manifest.generation = shared::Sha256::hex("generation");
    manifest.source_id = "local-test";
    manifest.archive_sha256 = shared::Sha256::hex("archive");
    manifest.game_install_identity = shared::Sha256::hex("installed");
    translation::SnapshotEntry entry;
    entry.resource = "Localize/en/UI/EN_Test.json";
    entry.original_sha256 = shared::Sha256::hex("original");
    entry.merged_sha256 = shared::Sha256::hex("merged");
    entry.merged_path = "tables/" + entry.merged_sha256 + ".json";
    entry.merged_bytes = 2;
    manifest.entries.push_back(entry);
    manifest.profile.present = true;
    manifest.profile.il2cpp_sha256 = shared::Sha256::hex("il2cpp");
    manifest.profile.text_offset = 4096;
    manifest.profile.text_size = 8192;
    manifest.profile.text_crc = 1234;
    const std::string text = translation::serialize_manifest(manifest);

    translation::SnapshotManifest decoded;
    std::string error;
    check(translation::parse_manifest(text, &decoded, &error), "manifest: round-trip");
    check(decoded.profile.present && decoded.profile.text_crc == 1234,
          "manifest: profile preserved");
    translation::SnapshotIndex index;
    check(index.load(decoded, &error), "manifest: index loads");
    check(index.find(entry.resource, entry.original_sha256) != nullptr,
          "manifest: exact lookup hits");
    check(index.find(entry.resource, shared::Sha256::hex("wrong")) == nullptr,
          "manifest: digest mismatch misses");
    check(index.find("Localize/en/UI/EN_Other.json", entry.original_sha256) == nullptr,
          "manifest: unknown resource misses");

    translation::SnapshotManifest bad = manifest;
    bad.entries[0].merged_path = "tables/not-a-digest.json";
    check(!translation::parse_manifest(translation::serialize_manifest(bad), &decoded, &error),
          "manifest: bad table path rejected");
    translation::SnapshotManifest bad_digest = manifest;
    bad_digest.entries[0].merged_sha256 = shared::Sha256::hex("other");
    check(!translation::parse_manifest(translation::serialize_manifest(bad_digest), &decoded,
                                       &error),
          "manifest: digest/file mismatch rejected");
    translation::SnapshotManifest bad_resource = manifest;
    bad_resource.entries[0].resource = "Localize/fr/UI/EN_Test.json";
    check(!translation::parse_manifest(translation::serialize_manifest(bad_resource), &decoded,
                                       &error),
          "manifest: unknown locale rejected");

    translation::SnapshotManifest large = manifest;
    for (int i = 0; i < 8200; ++i) {
        translation::SnapshotEntry extra = entry;
        extra.resource = "Localize/en/UI/EN_Large_" + std::to_string(i) + ".json";
        large.entries.push_back(std::move(extra));
    }
    check(translation::parse_manifest(translation::serialize_manifest(large), &decoded, &error),
          "manifest: table count is not capped at 8192");

    translation::SnapshotManifest large_table = manifest;
    large_table.entries[0].merged_bytes = (64ull << 20) + 1;
    check(translation::parse_manifest(translation::serialize_manifest(large_table), &decoded,
                                       &error),
          "manifest: merged table bytes are not capped at 64 MiB");

    std::string padded_manifest = text;
    padded_manifest.append((32ull << 20) + 1, ' ');
    check(translation::parse_manifest(padded_manifest, &decoded, &error),
          "manifest: metadata bytes are not capped at 32 MiB");
}

void test_large_json_input() {
    using namespace bybus;
    constexpr size_t kLargeTextBytes = (64ull << 20) + 1;
    std::string text = "{\"name\":\"";
    text.append(kLargeTextBytes, 'x');
    text += "\"}";
    const shared::ParseOutcome parsed = shared::parse(text);
    check(parsed.ok && value_of(parsed.value, "name").size() == kLargeTextBytes,
          "json: document body is not capped at 64 MiB");
}

void test_text_policy() {
    using namespace bybus;
    check(translation::is_display_field("name"), "policy: name is a display field");
    check(!translation::is_display_field("model"), "policy: model is metadata");
    check(translation::is_string_array_field("successDesc"), "policy: successDesc is an array");
    translation::MergeResult merged;
    std::string error;
    check(translation::JsonPairer::merge("Localize/en/UI/EN_Test.json",
        R"([{"id":1,"name":"old"}])",
        R"([{"id":1,"name":"\uFFFD\n"}])", &merged, &error),
        "policy: unusual display text is accepted");
    check(merged.content.find("\\uFFFD") != std::string::npos ||
          merged.content.find("\xEF\xBF\xBD") != std::string::npos,
          "policy: replacement character survives merge");
}

void test_sha256_vectors() {
    using namespace bybus::shared;
    check(Sha256::hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256: abc vector");
    check(Sha256::hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "sha256: empty vector");
    check(is_sha256_hex(Sha256::hex("x")), "sha256: hex format");
    check(!is_sha256_hex("ABC"), "sha256: uppercase/短rejected");
}

void test_json_number_identity() {
    using namespace bybus::shared;
    const auto integers = parse("[1,2.5,true,null]");
    check(integers.ok, "json: mixed array parses");
    if (integers.ok && integers.value) {
        check(integers.value->at(0)->type() == Json::Type::Int, "json: integer type kept");
        check(integers.value->at(1)->type() == Json::Type::Double, "json: double type kept");
        check(integers.value->at(1)->dump() == "2.5", "json: double dumps like Java");
    }
    const auto trailing = parse("{} trailing");
    check(!trailing.ok, "json: trailing data rejected");
    const auto bom = parse("\xEF\xBB\xBF{}");
    check(bom.ok, "json: BOM tolerated");
}

void test_appsealing_helpers() {
    using namespace bybus;
    const std::uint8_t nop[4] = {0x1f, 0x20, 0x03, 0xd5};
    std::uint32_t crc_table[256];
    runtime::owned_code::build_crc_table(crc_table);
    std::uint32_t expected_nop = 0xffffffffu;
    for (const std::uint8_t byte : {static_cast<std::uint8_t>(0x03),
                                    static_cast<std::uint8_t>(0xd5)}) {
        expected_nop = crc_table[(expected_nop ^ byte) & 0xffu] ^ (expected_nop >> 8);
    }
    expected_nop = ~expected_nop;
    check(translation::appsealing_section_crc(nop, sizeof(nop)) == expected_nop,
          "appsealing: NOP first halfword is normalised");
    const std::uint8_t plain[4] = {1, 2, 3, 4};
    check(translation::appsealing_section_crc(plain, sizeof(plain)) !=
              translation::appsealing_section_crc(nop, sizeof(nop)),
          "appsealing: non-NOP word contributes");

    std::uint8_t items[48] = {};
    const std::uint64_t first_offset = 0x100;
    const std::uint32_t first_size = 0x20;
    const std::uint64_t second_offset = 0x200;
    const std::uint32_t second_size = 0x40;
    std::memcpy(items, &first_offset, sizeof(first_offset));
    std::memcpy(items + 8, &first_size, sizeof(first_size));
    std::memcpy(items + 24, &second_offset, sizeof(second_offset));
    std::memcpy(items + 32, &second_size, sizeof(second_size));
    const std::uint64_t shape = translation::appsealing_range_fingerprint(items, 2, 24);
    check(shape != 0, "appsealing: descriptor fingerprint is non-zero");
    check(shape == translation::appsealing_range_fingerprint(items, 2, 24),
          "appsealing: descriptor fingerprint is stable");
    std::memcpy(items + 24, &second_offset, sizeof(second_offset));
    const std::uint64_t changed_size = 0x41;
    std::memcpy(items + 32, &changed_size, sizeof(changed_size));
    check(shape != translation::appsealing_range_fingerprint(items, 2, 24),
          "appsealing: descriptor fingerprint tracks range changes");

    translation::AppSealingPlan plan;
    std::string error;
    check(!translation::build_appsealing_plan("", "", 0, &plan, &error),
          "appsealing: missing protection is rejected");
    check(error == "appsealing_no_protection", "appsealing: fixed error code");
}

void test_owned_code_ledger() {
    using namespace bybus::runtime::owned_code;
    unsigned char live[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    unsigned char original[16];
    std::memcpy(original, live, sizeof(original));
    Patch patch;
    patch.offset = 0;
    patch.size = 4;
    patch.substitute = true;
    std::memcpy(patch.before, live, 4);
    std::memcpy(patch.after, live, 4);
    const unsigned char replacement[4] = {0xaa, 0xbb, 0xcc, 0xdd};
    std::memcpy(live, replacement, 4);
    std::memcpy(patch.after, replacement, 4);

    std::uint32_t baseline = 0;
    std::size_t owned = 0;
    check(section_crc(original, 0, sizeof(original), {}, baseline, owned),
          "ledger: baseline section crc");
    std::uint32_t restored = 0;
    check(section_crc(live, 0, sizeof(live), {patch}, restored, owned),
          "ledger: patched section crc");
    check(restored == baseline && owned == 1,
          "ledger: registered bytes reproduce the original crc");

    Patch lying = patch;
    lying.after[0] ^= 0xff;
    std::uint32_t ignored = 0;
    check(!section_crc(live, 0, sizeof(live), {lying}, ignored, owned),
          "ledger: live bytes must match the recorded patch");

    const std::vector<Range> ranges = {{0, 4}, {8, 4}};
    std::uint32_t range_crc = 0;
    std::size_t substituted = 0;
    check(ranges_crc(live, sizeof(live), ranges, {patch}, range_crc, substituted),
          "ledger: ranges crc succeeds");
    check(substituted == 4, "ledger: ranges crc counts substituted bytes");
    unsigned char expected[8] = {};
    std::memcpy(expected, patch.before, 4);
    std::memcpy(expected + 4, original + 8, 4);
    std::uint32_t expected_crc = 0;
    std::size_t expected_owned = 0;
    check(section_crc(expected, 0, sizeof(expected), {}, expected_crc, expected_owned) &&
              range_crc == expected_crc,
          "ledger: ranges crc matches the substituted byte stream");
    check(!ranges_crc(live, 8, {{0, 8}, {4, 4}}, {patch}, range_crc, substituted),
          "ledger: overlapping ranges are rejected");
}

void test_translation_index_roundtrip() {
    using namespace bybus;
    std::vector<translation::IndexRecord> records;
    records.push_back({"Localize/en/UI/Bufs.json", "", "name", "Haste", "\xE5\x8A\xA0\xE9\x80\x9F",
                       "@skill:1"});
    records.push_back({"Localize/en/UI/Bufs.json", "", "name", "Haste", "\xE5\x8A\xA0\xE9\x80\x9F",
                       "@skill:1"});
    records.push_back({"Localize/en/UI/Bufs.json", "", "name", "Burn", "\xE7\x81\xBC\xE7\x83\xA7",
                       "@skill:2"});
    records.push_back({"Localize/en/StoryData/EN_Story.json", "", "dialog", "Line 1",
                       "\xE5\x8F\xB0\xE8\xAF\x8D", "@story:scene1/dialog:1"});
    const translation::CompiledTranslationIndex compiled =
        translation::compile_translation_index(records, "test", "archive");
    check(compiled.unique_entries.find("Haste") != compiled.unique_entries.end(),
          "index: unique entry compiled");
    check(compiled.term_entries.find("Haste") != compiled.term_entries.end(),
          "index: glossary term compiled");
    check(compiled.contextual_entries.find("Haste\x1f" "name\x1f" "Haste") ==
              compiled.contextual_entries.end(),
          "index: unambiguous source has no contextual override");
    std::string bytes;
    std::string error;
    check(translation::write_translation_index(compiled, &bytes, &error),
          "index: binary write");
    runtime::TranslationIndex loaded;
    check(loaded.load(bytes, &error), "index: binary read");
    check(loaded.unique_count() == compiled.unique_entries.size() &&
              loaded.term_count() == compiled.term_entries.size() &&
              loaded.contextual_count() == compiled.contextual_entries.size(),
          "index: counts round-trip");
    const std::string *haste = loaded.find("Haste");
    check(haste && *haste == "\xE5\x8A\xA0\xE9\x80\x9F", "index: unique lookup");
    std::string corrupt = bytes;
    corrupt[0] ^= 0x7f;
    runtime::TranslationIndex rejected;
    check(!rejected.load(corrupt, &error), "index: magic mismatch rejected");
}

bybus::translation::AppSealingPlan sample_plan() {
    using bybus::translation::AppSealingHookPoint;
    using bybus::translation::AppSealingPlan;
    using bybus::translation::AppSealingReport;
    using bybus::translation::AppSealingSite;
    AppSealingPlan plan;
    plan.present = true;
    plan.family = "test-family";
    plan.protection_sha256 = bybus::shared::Sha256::hex("protection");
    plan.module_size = 0x200000;
    plan.text_vaddr = 0x1000;
    plan.text_size = 0x2000;
    plan.text_crc = 1;
    plan.managed_vaddr = 0x4000;
    plan.managed_size = 0x2000;
    plan.managed_crc = 2;
    plan.ranges_vaddr = 0x8000;
    plan.ranges_count = 4;
    plan.ranges_fingerprint = "dd108bf95840fd56";
    AppSealingSite branch;
    branch.kind = AppSealingSite::Kind::Branch;
    branch.vaddr = 0x10000;
    branch.target_vaddr = 0x10100;
    branch.expected = 0x11111111;
    branch.word = 0x14000040;
    AppSealingSite nop;
    nop.kind = AppSealingSite::Kind::Nop;
    nop.vaddr = 0x10004;
    nop.expected = 0x22222222;
    nop.word = 0xd503201f;
    plan.sites = {branch, nop};
    plan.detach.present = true;
    plan.detach.entry_vaddr = 0x10008;
    plan.detach.expected = {1, 2, 3, 4, 5};
    plan.report.present = true;
    plan.report.entry_vaddr = 0x10010;
    plan.report.expected = {6, 7, 8, 9, 10};
    plan.report.code = 50048;
    plan.report.caller_vaddr = 0x10020;
    plan.report.pattern_vaddr = 0x10030;
    plan.report.pattern = {11, 12, 13, 14, 15, 16};
    plan.report.continuation_vaddr = 0x10040;
    plan.detach_caller_vaddr = 0x10050;
    plan.detach_continuation_vaddr = 0x10060;
    plan.confirm_caller_vaddr = 0x10070;
    plan.confirm_continuation_vaddr = 0x10080;
    plan.stack.record_min_bytes = 0x600;
    plan.stack.name_offset = 8;
    plan.stack.text_size_offset = 0x320;
    plan.stack.managed_size_offset = 0x348;
    plan.stack.text_crc_offset = 0x370;
    plan.stack.managed_crc_offset = 0x374;
    plan.stack.caller_stack_min_bytes = 0x700;
    plan.stack.env_offset = 0x580;
    plan.stack.state_offset = 0x2c8;
    plan.stack.index_offset = 0x2cc;
    plan.stack.descriptor_index_offset = 0x658;
    plan.stack.module_offset = 0x6b8;
    plan.stack.expected_offset = 0x648;
    plan.stack.descriptor_count_offset = 8;
    plan.stack.descriptor_entries_offset = 24;
    plan.stack.descriptor_stride = 24;
    plan.guards.push_back({0x700e4, 0xb942cbe0});
    plan.report_stable = true;
    return plan;
}

void test_appsealing_manifest() {
    using namespace bybus;
    translation::SnapshotManifest manifest;
    manifest.generation = shared::Sha256::hex("generation");
    manifest.source_id = "local-test";
    manifest.archive_sha256 = shared::Sha256::hex("archive");
    manifest.game_install_identity = shared::Sha256::hex("installed");
    manifest.profile.present = true;
    manifest.profile.il2cpp_sha256 = shared::Sha256::hex("il2cpp");
    manifest.profile.protection_sha256 = shared::Sha256::hex("protection");
    manifest.profile.text_offset = 4096;
    manifest.profile.text_size = 8192;
    manifest.profile.text_crc = 1234;
    manifest.profile.text_vaddr = 0x1000;
    manifest.profile.managed_vaddr = 0x4000;
    manifest.profile.managed_size = 0x2000;
    manifest.profile.managed_crc = 2;
    manifest.profile.appsealing = sample_plan();
    translation::SnapshotEntry entry;
    entry.resource = "Localize/en/UI/EN_Test.json";
    entry.original_sha256 = shared::Sha256::hex("original");
    entry.merged_sha256 = shared::Sha256::hex("merged");
    entry.merged_path = "tables/" + entry.merged_sha256 + ".json";
    entry.merged_bytes = 2;
    manifest.entries.push_back(entry);

    translation::SnapshotManifest decoded;
    std::string error;
    check(translation::parse_manifest(translation::serialize_manifest(manifest), &decoded,
                                      &error),
          "manifest: appsealing plan round-trip");
    check(decoded.profile.appsealing.present &&
              decoded.profile.appsealing.sites.size() == 2 &&
              decoded.profile.appsealing.report.code == 50048 &&
              decoded.profile.appsealing.guards.size() == 1 &&
              decoded.profile.appsealing.report_stable &&
              decoded.profile.appsealing.stack.descriptor_stride == 24,
          "manifest: appsealing fields preserved");
    const std::string digest = translation::appsealing_digest(manifest.profile.appsealing);

    translation::SnapshotManifest duplicate = manifest;
    duplicate.profile.appsealing.sites[1].vaddr =
        duplicate.profile.appsealing.sites[0].vaddr;
    check(!translation::parse_manifest(translation::serialize_manifest(duplicate), &decoded,
                                       &error),
          "manifest: overlapping appsealing sites are rejected");

    translation::SnapshotManifest bad_fingerprint = manifest;
    bad_fingerprint.profile.appsealing.ranges_fingerprint = "zz108bf95840fd56";
    check(!translation::parse_manifest(translation::serialize_manifest(bad_fingerprint),
                                       &decoded, &error),
          "manifest: bad appsealing fingerprint is rejected");

    translation::SnapshotManifest transient = manifest;
    transient.profile.appsealing.transient.present = true;
    transient.profile.appsealing.transient.vaddr = 0x10004;
    transient.profile.appsealing.transient.target_vaddr = 0x10100;
    transient.profile.appsealing.transient.expected_word = 0x22222222;
    transient.profile.appsealing.transient.branch_word = 0x1400003f;
    transient.profile.appsealing.transient.sigill_pc_vaddr = 0x10004;
    transient.profile.appsealing.transient.sigill_lr_vaddr = 0x10000;
    check(!translation::parse_manifest(translation::serialize_manifest(transient), &decoded,
                                       &error),
          "manifest: transient site must be registered without substitution");

    transient.profile.appsealing.sites[1].substitute = false;
    check(translation::parse_manifest(translation::serialize_manifest(transient), &decoded,
                                      &error),
          "manifest: transient site accepted when marked non-substitute");
    check(translation::appsealing_digest(manifest.profile.appsealing) !=
              translation::appsealing_digest(transient.profile.appsealing),
          "manifest: plan digest tracks plan changes");
    (void)digest;
}

} // namespace

int main() {
    test_merge_basic();
    test_merge_unchanged_reports_null();
    test_unknown_fields_preserved();
    test_duplicate_identity_by_metadata();
    test_string_array_field();
    test_ego_gift_abilities();
    test_long_skill_context();
    test_released_texts();
    test_snapshot_marker_rejected();
    test_manifest_validation();
    test_large_json_input();
    test_text_policy();
    test_sha256_vectors();
    test_json_number_identity();
    test_appsealing_helpers();
    test_owned_code_ledger();
    test_translation_index_roundtrip();
    test_appsealing_manifest();
    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all host checks passed\n");
    return 0;
}

