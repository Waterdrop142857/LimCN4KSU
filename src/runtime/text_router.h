// Shared managed-string translation path for every text hook. JSON tables use
// the existing snapshot lookup; prose uses the schema-9 unique/contextual/term
// index. A missing or rejected match returns the original object.
#pragma once

#include <string>

namespace bybus::runtime {

bool looks_like_json_text(const std::string &text);

// Returns the original pointer when no verified translation applies.
void *translate_managed_string(void *managed, const char *resource_hint,
                               const char *context_id, const char *field);

// Plain-string variant for callers that already decoded the text.
bool translate_text_value(const std::string &source, const char *resource_hint,
                          const char *context_id, const char *field,
                          std::string *translated);

// True when this exact managed string was produced by translate_managed_string
// and is still in the bounded recent-output ledger. Font selection uses this as
// provenance instead of assuming every Han glyph is a translated label.
bool was_translated_text(void *managed);

} // namespace bybus::runtime
