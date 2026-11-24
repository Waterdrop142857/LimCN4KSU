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

// TranslationTextPolicy.isUsableDisplayText(): the text contains no encoding
// damage (no U+FFFD, no C0 control other than \n \r \t, no DEL/C1). Text is
// inspected per UTF-16 code unit, exactly like the Kotlin `Char` iteration.
bool is_usable_display_text(const std::string &value);

// Kotlin `String.isBlank()`: true when the text is empty or every character is
// Character.isWhitespace or Character.isSpaceChar. U+200B is not whitespace.
bool is_blank(const std::string &value);

} // namespace bybus::translation
