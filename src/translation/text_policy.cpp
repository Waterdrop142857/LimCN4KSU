#include "translation/text_policy.h"

#include <unordered_set>

namespace bybus::translation {
namespace {

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

} // namespace bybus::translation
