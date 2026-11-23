#include "shared/utf.h"

namespace bybus::shared {
namespace {

bool is_continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

} // namespace

bool valid_utf8(const std::string &text) {
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[i]);
        if (first < 0x80) {
            ++i;
            continue;
        }
        size_t length = 0;
        uint32_t codepoint = 0;
        uint32_t minimum = 0;
        if ((first & 0xE0) == 0xC0) {
            length = 2;
            codepoint = first & 0x1F;
            minimum = 0x80;
        } else if ((first & 0xF0) == 0xE0) {
            length = 3;
            codepoint = first & 0x0F;
            minimum = 0x800;
        } else if ((first & 0xF8) == 0xF0) {
            length = 4;
            codepoint = first & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (i + length > text.size()) return false;
        for (size_t k = 1; k < length; ++k) {
            const unsigned char next = static_cast<unsigned char>(text[i + k]);
            if (!is_continuation(next)) return false;
            codepoint = (codepoint << 6) | (next & 0x3F);
        }
        if (codepoint < minimum || codepoint > 0x10FFFF) return false;
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF) return false;
        i += length;
    }
    return true;
}

bool utf16_to_utf8(const uint16_t *units, int32_t length, std::string *out) {
    if (!units || length < 0 || !out) return false;
    out->clear();
    out->reserve(static_cast<size_t>(length));
    for (int32_t i = 0; i < length; ++i) {
        uint32_t codepoint = units[i];
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            if (i + 1 >= length) return false;
            const uint32_t low = units[i + 1];
            if (low < 0xDC00 || low > 0xDFFF) return false;
            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
            ++i;
        } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
            return false;
        }
        if (codepoint < 0x80) {
            out->push_back(static_cast<char>(codepoint));
        } else if (codepoint < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            out->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }
    return true;
}

bool utf8_to_utf16(const std::string &text, std::u16string *out) {
    if (!out) return false;
    out->clear();
    out->reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[i]);
        uint32_t codepoint = 0;
        size_t length = 0;
        if (first < 0x80) {
            codepoint = first;
            length = 1;
        } else if ((first & 0xE0) == 0xC0) {
            codepoint = first & 0x1F;
            length = 2;
        } else if ((first & 0xF0) == 0xE0) {
            codepoint = first & 0x0F;
            length = 3;
        } else if ((first & 0xF8) == 0xF0) {
            codepoint = first & 0x07;
            length = 4;
        } else {
            return false;
        }
        if (i + length > text.size()) return false;
        for (size_t k = 1; k < length; ++k) {
            const unsigned char next = static_cast<unsigned char>(text[i + k]);
            if (!is_continuation(next)) return false;
            codepoint = (codepoint << 6) | (next & 0x3F);
        }
        if (length == 1 && codepoint < 0x80) {
            out->push_back(static_cast<char16_t>(codepoint));
        } else if (length == 2 && codepoint >= 0x80) {
            out->push_back(static_cast<char16_t>(codepoint));
        } else if (length == 3 && codepoint >= 0x800) {
            out->push_back(static_cast<char16_t>(codepoint));
        } else if (length == 4 && codepoint >= 0x10000 && codepoint <= 0x10FFFF) {
            codepoint -= 0x10000;
            out->push_back(static_cast<char16_t>(0xD800 + (codepoint >> 10)));
            out->push_back(static_cast<char16_t>(0xDC00 + (codepoint & 0x3FF)));
        } else {
            return false;
        }
        i += length;
    }
    return true;
}

} // namespace bybus::shared
