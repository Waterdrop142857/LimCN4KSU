#include "translation/text_policy.h"

#include <unordered_set>

#include "shared/utf.h"

namespace bybus::translation {
namespace {

// Character.isWhitespace(c) || Character.isSpaceChar(c), i.e. the set Kotlin's
// String.isBlank() consults. U+200B (Cf) and U+FEFF are deliberately absent.
bool is_kotlin_space(uint32_t unit) {
    if ((unit >= 0x09 && unit <= 0x0D) || (unit >= 0x1C && unit <= 0x1F) || unit == 0x20) {
        return true;
    }
    return unit == 0xA0 || unit == 0x1680 || (unit >= 0x2000 && unit <= 0x200A) ||
           unit == 0x2028 || unit == 0x2029 || unit == 0x202F || unit == 0x205F ||
           unit == 0x3000;
}

// Membership must match TranslationTextPolicy.kt exactly. A function-local set
// avoids depending on sorted literal order.
const std::unordered_set<std::string> &display_fields() {
    static const std::unordered_set<std::string> fields = {
        "content", "dialog", "dlg", "teller",
        "name", "nameWithTitle", "nickName", "longName", "specialName",
        "shortName", "abName", "abnormalityName", "panicName", "skinItemTitle",
        "desc", "description", "simpleDesc", "summary", "flavor", "rawDesc", "behaveDesc",
        "eventDesc", "prevDesc", "subDesc", "successDesc", "failureDesc",
        "message", "messageDesc", "result", "panicDescription",
        "lowMoraleDescription", "skinItemDesc", "acquisitionMethod",
        "title", "oneLineTitle", "place", "codeName", "clue", "sentence",
        "text", "subText", "mainText", "story",
        "openCondition", "openConditionNumber", "relatedChapterText", "askLevelUp",
        "company", "area", "chapter", "chapterNumber", "chaptertitle", "parttitle",
        "timeline", "teacher", "add", "min", "variation", "variation2",
        "speaker", "displayName", "statText", "goalDescription1", "goalDescription2",
        "goalDescription3", "goalDescription4", "goalDescription5", "goalDescription6",
        "goalDescription7", "goalDescription8",
    };
    return fields;
}

const std::unordered_set<std::string> &string_array_fields() {
    static const std::unordered_set<std::string> fields = {"successDesc", "failureDesc", "result"};
    return fields;
}

const std::unordered_set<std::string> &term_fields() {
    static const std::unordered_set<std::string> fields = {
        "content", "name", "shortName", "abName", "title", "summary"};
    return fields;
}

const std::unordered_set<std::string> &structured_description_fields() {
    static const std::unordered_set<std::string> fields = {
        "desc", "description", "simpleDesc", "rawDesc", "behaveDesc",
        "eventDesc", "subDesc", "successDesc", "failureDesc"};
    return fields;
}

} // namespace

bool is_display_field(const std::string &field) {
    return display_fields().find(field) != display_fields().end();
}

bool is_string_array_field(const std::string &field) {
    return string_array_fields().find(field) != string_array_fields().end();
}

bool is_term_field(const std::string &field) {
    return term_fields().find(field) != term_fields().end() && is_display_field(field);
}

bool is_structured_description_field(const std::string &field) {
    return structured_description_fields().find(field) != structured_description_fields().end() &&
           is_display_field(field);
}

bool is_usable_display_text(const std::string &value) {
    std::u16string units;
    // A string that cannot be re-encoded as UTF-16 cannot be written into an
    // IL2CPP managed string either, so it is unusable by definition.
    if (!shared::utf8_to_utf16(value, &units)) return false;
    for (const char16_t unit : units) {
        const uint32_t code = static_cast<uint32_t>(unit);
        if (code == 0xFFFD) return false;
        if (code < 0x20 && code != u'\n' && code != u'\r' && code != u'\t') return false;
        if (code >= 0x7F && code <= 0x9F) return false;
    }
    return true;
}

bool is_blank(const std::string &value) {
    std::u16string units;
    // Invalid UTF-8 cannot be Kotlin's all-whitespace string: it carries bytes
    // that no whitespace character maps to.
    if (!shared::utf8_to_utf16(value, &units)) return value.empty();
    for (const char16_t unit : units) {
        if (!is_kotlin_space(static_cast<uint32_t>(unit))) return false;
    }
    return true;
}

} // namespace bybus::translation
