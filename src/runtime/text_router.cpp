#include "runtime/text_router.h"

#include <cstddef>
#include <string>
#include <utility>

#include "runtime/il2cpp.h"
#include "runtime/state.h"
#include "runtime/translation_index.h"
#include "runtime/ui_labels.h"
#include "shared/sha256.h"

namespace bybus::runtime {
namespace {

thread_local int g_router_depth = 0;

struct RouterScope {
    RouterScope() { ++g_router_depth; }
    ~RouterScope() { --g_router_depth; }
};

bool usable(const std::string &text) {
    if (text.empty() || text.size() > 16ull * 1024 * 1024) return false;
    for (const char c : text) {
        if (static_cast<unsigned char>(c) == 0) return false;
    }
    return true;
}

} // namespace

bool looks_like_json_text(const std::string &text) {
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        return c == '{' || c == '[';
    }
    return false;
}

bool translate_text_value(const std::string &source, const char *resource_hint,
                          const char *context_id, const char *field,
                          std::string *translated) {
    if (!translated || !usable(source)) return false;
    RuntimeState &runtime = state();
    if (g_router_depth != 0) return false;
    RouterScope scope;
    if (resource_hint && *resource_hint && looks_like_json_text(source)) {
        const std::string digest = shared::Sha256::hex(source);
        std::string merged;
        std::string error;
        if (runtime.reader.lookup(resource_hint, digest, &merged, &error) &&
            usable(merged) && merged != source) {
            *translated = std::move(merged);
            return true;
        }
    }
    const TranslationIndex &index = runtime.reader.translation_index();
    const std::string label = limbus_ui_labels::translate_status(source);
    if (!label.empty() && label != source) {
        *translated = label;
        return true;
    }
    if (index.empty()) return false;
    if (context_id && *context_id && field && *field) {
        const std::string key = translation_context_key(context_id, field, source);
        if (!key.empty()) {
            const std::string *match = index.find_context(key);
            if (match && usable(*match)) {
                *translated = *match;
                return true;
            }
        }
    }
    const std::string *match = index.find(source);
    if (!match) match = index.find_term(source);
    if (!match || !usable(*match) || *match == source) return false;
    *translated = *match;
    return true;
}

void *translate_managed_string(void *managed, const char *resource_hint,
                               const char *context_id, const char *field) {
    RuntimeState &runtime = state();
    if (!managed || g_router_depth != 0 || !runtime.api.ready()) return managed;
    std::string source;
    if (!managed_string_to_utf8(runtime.api, managed, &source)) return managed;
    std::string translated;
    if (!translate_text_value(source, resource_hint, context_id, field, &translated)) {
        return managed;
    }
    void *replacement = managed_string_from_utf8(runtime.api, translated);
    return replacement ? replacement : managed;
}

} // namespace bybus::runtime
