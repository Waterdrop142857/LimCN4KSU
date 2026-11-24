// Method-signature parsing shared by the IL2CPP method lookup and the hook
// installer.
//
// The reference runtime never compares a literal type name: it concatenates the
// parameter types the runtime actually reports and compares the whole string.
// IL2CPP reports the same type in more than one spelling depending on the Unity
// version, so a raw strcmp against one spelling silently matches nothing:
//
//   Dictionary`2<A,B>  ==  Dictionary<A,B>     (generic arity suffix)
//   NS.Type/Axis       ==  NS.Type.Axis        (nested type separator)
//   "A, B"             ==  "A,B"               (spacing)
//
// normalize() folds those spellings; split() tokenises a parameter list at
// generic depth 0 so a comma inside System.Collections.Generic.Dictionary<A,B>
// is not a parameter separator. Header-only so the host tests include it
// directly.
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace bybus::runtime::type_signature {

// Removes spacing, maps the nested-type separator '/' to '.' and drops a generic
// arity suffix (Dictionary`2 -> Dictionary). The array suffix "[]" and the
// by-reference marker '&' are preserved because they distinguish overloads.
inline std::string normalize(const char *raw) {
    std::string result;
    if (!raw) return result;
    for (const char *cursor = raw; *cursor != '\0'; ++cursor) {
        const char character = *cursor;
        if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
            continue;
        }
        if (character == '/') {
            result.push_back('.');
            continue;
        }
        if (character == '`') {
            // Skip the arity digits that follow; the loop increment then lands
            // on the first character after them.
            ++cursor;
            while (*cursor >= '0' && *cursor <= '9') ++cursor;
            if (*cursor == '\0') break;
            --cursor;
            continue;
        }
        result.push_back(character);
    }
    return result;
}

// Splits a comma-separated parameter list at generic depth 0 and normalizes each
// token. Returns false when the list is malformed: an empty token (",," / a
// trailing comma), unbalanced angle brackets, or more than `limit` parameters.
// An empty `text` succeeds and yields zero parameters.
inline bool split(const char *text, std::vector<std::string> *out, size_t limit = 16) {
    if (!out) return false;
    out->clear();
    if (!text) return false;
    if (*text == '\0') return true;
    std::vector<std::string> tokens;
    std::string token;
    int depth = 0;
    for (const char *cursor = text;; ++cursor) {
        const char character = *cursor;
        if (character == '\0' || (character == ',' && depth == 0)) {
            const std::string normalized = normalize(token.c_str());
            if (normalized.empty()) return false;
            token.clear();
            if (tokens.size() == limit) return false;
            tokens.push_back(normalized);
            if (character == '\0') break;
            continue;
        }
        if (character == '<') {
            ++depth;
        } else if (character == '>') {
            if (depth == 0) return false;
            --depth;
        }
        token.push_back(character);
    }
    if (depth != 0) return false;
    *out = std::move(tokens);
    return true;
}

} // namespace bybus::runtime::type_signature
