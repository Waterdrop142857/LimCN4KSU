// Pairing fidelity tests for the ported TranslationJsonPairer / TranslationSnapshot
// policy: display gates, display-array gates, exact number binding, bounded scope
// ids and the pairing-quality threshold.
// Build and run: build/test-host.ps1 —or any C++17 compiler with -Isrc.
#include <string>
#include <vector>

#include "shared/json.h"
#include "shared/sha256.h"
#include "translation/json_pairer.h"
#include "translation/text_policy.h"

#include "harness.h"

namespace {

using bybus::shared::JsonPtr;
using bybus::shared::Sha256;
using bybus::translation::IndexRecord;
using bybus::translation::JsonPairer;
using bybus::translation::MergeResult;
using bybus::translation::PairResult;

const char *const kPlainPath = "Localize/en/UI/EN_Pairing.json";
const char *const kSkillPath = "Localize/en/UI/Skills.json";

// TranslationSnapshot.encode() marker over an object document: the marker proves
// the text was generated from this exact source, so binding may run. `members`
// is everything that follows the marker, including the closing brace.
std::string with_marker(const std::string &original, const std::string &members) {
    return "{\"_bybusTextSnapshot\":{\"schema\":1,\"sha256\":\"" + Sha256::hex(original) +
           "\"}," + members;
}

// Two rows under one id and without stable metadata: identity and position
// fallbacks cannot pair them, so only the snapshot binding can. The returned
// count therefore reports whether the binding accepted the numeric row.
int bound_row_count(const std::string &source_number, const std::string &translated_number) {
    auto row = [](const std::string &number, const char *name) {
        return "{\"id\":\"9\",\"count\":" + number + ",\"name\":\"" + name + "\"}";
    };
    const std::string original =
        "{\"rows\":[" + row(source_number, "a") + "," + row(source_number, "a") + "]}";
    const std::string translated = with_marker(
        original, "\"rows\":[" + row(translated_number, "b") + "," +
                      row(translated_number, "b") + "]}");
    MergeResult merged;
    std::string error;
    if (!JsonPairer::merge(kPlainPath, original, translated, &merged, &error)) return -1;
    return merged.matched_fields;
}

} // namespace

BYBUS_TEST(pairing_blank_uses_kotlin_whitespace) {
    using bybus::translation::is_blank;
    check(is_blank(""), "blank: empty text is blank");
    check(is_blank(" \t\n"), "blank: ascii whitespace is blank");
    check(is_blank(u8"\u3000"), "blank: ideographic space is blank");
    check(is_blank(u8"\u00A0"), "blank: no-break space is blank");
    check(!is_blank("a"), "blank: a letter is not blank");
    check(!is_blank(u8"\u200B"), "blank: zero-width space is not blank");
}

BYBUS_TEST(pairing_usable_display_text_rejects_damage) {
    using bybus::translation::is_usable_display_text;
    check(!is_usable_display_text(u8"\uFFFD"), "usable: replacement character is rejected");
    check(!is_usable_display_text("\x01"), "usable: C0 control is rejected");
    check(!is_usable_display_text("\x7F"), "usable: DEL is rejected");
    check(is_usable_display_text("a\nb"), "usable: embedded newline is fine");
    check(is_usable_display_text(u8"汉"), "usable: non-ascii text is fine");
}

BYBUS_TEST(pairing_merge_skips_blank_translation) {
    const std::string original = R"([{"id":1,"name":"old"}])";
    MergeResult merged;
    std::string error;
    check(JsonPairer::merge(kPlainPath, original, R"([{"id":1,"name":" \t\n"}])", &merged, &error),
          "pairing: blank translation parses");
    check(!merged.changed, "pairing: a blank translation edits nothing");
    check(merged.content.empty(), "pairing: a blank translation yields no content");
    check(merged.matched_fields == 0, "pairing: a blank translation is not a matched field");
}

BYBUS_TEST(pairing_merge_skips_damaged_translation) {
    const std::string original = R"([{"id":1,"name":"old"}])";
    MergeResult merged;
    std::string error;
    check(JsonPairer::merge(kPlainPath, original, R"([{"id":1,"name":"\uFFFD"}])", &merged, &error),
          "pairing: damaged translation parses");
    check(!merged.changed, "pairing: replacement character edits nothing");
    check(merged.content.empty(), "pairing: replacement character yields no content");
}

BYBUS_TEST(pairing_display_array_skips_unusable_entry) {
    const std::string original = R"([{"id":"9","successDesc":["one"," "]}])";
    MergeResult merged;
    std::string error;
    check(JsonPairer::merge(kPlainPath, original, R"([{"id":"9","successDesc":["a","b"]}])",
                            &merged, &error),
          "pairing: display array parses");
    check(merged.matched_fields == 1, "pairing: only the usable array entry is recorded");
    const auto parsed = bybus::shared::parse(merged.content);
    const JsonPtr row =
        parsed.ok && parsed.value && parsed.value->length() == 1 ? parsed.value->at(0) : nullptr;
    const JsonPtr list = row ? row->opt("successDesc") : nullptr;
    check(list && list->is_array() && list->length() == 2, "pairing: array shape is kept");
    if (list && list->length() == 2) {
        check(list->at(0)->as_string() == "a", "pairing: the usable entry took its translation");
        check(list->at(1)->as_string() == " ", "pairing: the blank entry kept the original text");
    }
}

BYBUS_TEST(pairing_display_array_rejects_non_string_entry) {
    const std::string original = R"([{"id":"9","successDesc":["one",1]}])";
    MergeResult merged;
    std::string error;
    check(JsonPairer::merge(kPlainPath, original, R"([{"id":"9","successDesc":["a",2]}])",
                            &merged, &error),
          "pairing: non-string display array parses");
    check(merged.matched_fields == 0, "pairing: a non-string entry rejects the whole array");
    check(!merged.changed, "pairing: a non-string entry edits nothing");
}

BYBUS_TEST(pairing_number_binding_is_exact_beyond_two_pow_53) {
    // Both integers are the same double, so a double comparison would bind them.
    const std::string original =
        R"({"rows":[{"id":"9","count":9007199254740993,"name":"a"},)"
        R"({"id":"9","count":9007199254740993,"name":"a"}]})";
    const std::string translated = with_marker(
        original,
        R"("rows":[{"id":"9","count":9007199254740992,"name":"b"},)"
        R"({"id":"9","count":9007199254740992,"name":"b"}]})");
    MergeResult merged;
    std::string error;
    check(JsonPairer::merge(kPlainPath, original, translated, &merged, &error),
          "pairing: 2^53 documents parse");
    check(merged.matched_fields == 0,
          "pairing: 9007199254740993 and 9007199254740992 never bind");
    check(!merged.changed, "pairing: an unbound row keeps the game text");
    check(merged.content.empty(), "pairing: an unbound row yields no content");
}

BYBUS_TEST(pairing_number_binding_normalizes_decimals) {
    check(bound_row_count("1", "1.0") == 2, "pairing: 1 and 1.0 bind");
    check(bound_row_count("1.50", "1.5") == 2, "pairing: 1.50 and 1.5 bind");
    check(bound_row_count("2.5e3", "2500") == 2, "pairing: exponent form binds its integer");
    check(bound_row_count("1.5", "1.6") == 0, "pairing: different decimals stay apart");
}

BYBUS_TEST(pairing_sufficiently_paired_thresholds) {
    using bybus::translation::sufficiently_paired;
    check(sufficiently_paired(0, 0), "paired: an empty source set is trivially paired");
    check(sufficiently_paired(10, 9), "paired: exactly 90% passes");
    check(!sufficiently_paired(100, 89), "paired: below 90% fails");
    MergeResult merged;
    merged.source_fields = 0;
    check(merged.sufficiently_paired(), "paired: MergeResult forwards the rule");
    merged.source_fields = 100;
    merged.matched_fields = 89;
    check(!merged.sufficiently_paired(), "paired: MergeResult reports the shortfall");
}

BYBUS_TEST(pairing_skill_scope_requires_bounded_numeric_id) {
    auto document = [](const std::string &id, const char *text) {
        return "[{\"id\":\"" + id + "\",\"name\":\"" + text +
               "\",\"levelList\":[{\"level\":1,\"name\":\"" + text + "\"}]}]";
    };
    const std::string long_id(260, '7');
    PairResult paired;
    std::string error;
    check(JsonPairer::pair(kSkillPath, document(long_id, "old"), document(long_id, "new"), &paired,
                           &error),
          "pairing: an over-long skill id table parses");
    bool id_itself = false;
    bool scoped = false;
    for (const IndexRecord &record : paired.records) {
        if (record.stable_id == long_id) id_itself = true;
        if (record.stable_id.find("@skill:") != std::string::npos ||
            record.stable_id.find("/level:") != std::string::npos) {
            scoped = true;
        }
    }
    check(id_itself, "pairing: a 260-digit id is not a skill number");
    check(!scoped, "pairing: an over-long id never becomes a skill scope");

    const std::string bounded_id(15, '3');
    PairResult bounded;
    check(JsonPairer::pair(kSkillPath, document(bounded_id, "old"), document(bounded_id, "new"),
                           &bounded, &error),
          "pairing: a 15-digit skill id table parses");
    bool root_scoped = false;
    bool level_scoped = false;
    for (const IndexRecord &record : bounded.records) {
        if (record.stable_id == "@skill:" + bounded_id) root_scoped = true;
        if (record.stable_id == "@skill:" + bounded_id + "/level:1") level_scoped = true;
    }
    check(root_scoped, "pairing: a bounded id becomes the skill root");
    check(level_scoped, "pairing: a bounded id keeps the skill level context");
}
