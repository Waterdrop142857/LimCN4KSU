// Display-field policy ported from app/src/main/java/com/example/limbuszhcn/
// container/TranslationTextPolicy.kt at bd32608. The same whitelist gates the
// pairer, the released-text catalogue and the runtime field checks, so it lives
// in one compiled unit.
#pragma once

#include <string>

namespace bybus::translation {

bool is_display_field(const std::string &field);
bool is_string_array_field(const std::string &field);
bool is_term_field(const std::string &field);
bool is_structured_description_field(const std::string &field);

} // namespace bybus::translation
