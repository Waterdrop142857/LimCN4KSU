// UTF-8 / UTF-16 conversion and validation used by the runtime, the archive
// importer and the WebUI bridge. Conversions are bounded and report failure
// instead of substituting replacement characters.
#pragma once

#include <cstdint>
#include <string>

namespace bybus::shared {

// True when the bytes are well formed UTF-8 (no overlong, no surrogate, no
// truncation). A leading BOM is allowed and left in place.
bool valid_utf8(const std::string &text);

// Decodes UTF-16LE code units. Returns false on unpaired surrogates.
bool utf16_to_utf8(const uint16_t *units, int32_t length, std::string *out);
// Encodes UTF-8 into UTF-16 code units. Returns false on invalid input.
bool utf8_to_utf16(const std::string &text, std::u16string *out);

} // namespace bybus::shared
