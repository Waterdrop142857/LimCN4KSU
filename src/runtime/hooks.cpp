#include "runtime/hooks.h"

#include <dlfcn.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "CydiaSubstrate.h"
#include "runtime/il2cpp.h"
#include "runtime/log.h"
#include "runtime/state.h"
#include "runtime/text_router.h"
#include "shared/sha256.h"
#include "shared/utf.h"

namespace bybus::runtime {
namespace {

typedef void *(*load_localize_fn)(void *instance, void *path, const void *method);
typedef void *(*text_asset_text_fn)(void *instance, const void *method);
typedef void (*ui_set_text_fn)(void *instance, void *text, const void *method);
typedef void (*tmp_set_text_fn)(void *instance, void *text, const void *method);
typedef void (*announcer_set_text_fn)(void *instance, void *text, const void *method);

load_localize_fn g_original_load_localize = nullptr;
text_asset_text_fn g_original_text_asset_text = nullptr;
ui_set_text_fn g_original_ui_set_text = nullptr;
tmp_set_text_fn g_original_tmp_set_text = nullptr;
announcer_set_text_fn g_original_announcer_set_text = nullptr;
void *g_text_asset_class = nullptr;
void *g_legacy_font = nullptr;
void *g_legacy_font_handle = nullptr;
const void *g_ui_get_font = nullptr;
const void *g_ui_set_font = nullptr;
const void *g_get_instance_id = nullptr;
pthread_t g_unity_main_thread{};
struct SavedFont {
    void *font_handle = nullptr;
    void *weak_ui_handle = nullptr;
};
std::unordered_map<int32_t, SavedFont> g_original_ui_fonts;
thread_local int g_translation_depth = 0;
pthread_mutex_t g_font_mutex = PTHREAD_MUTEX_INITIALIZER;
void *g_font_handle = nullptr;
// Latched only after a genuine failure on the Unity main thread; a call from any
// other thread returns early without consuming the single attempt.
bool g_font_attempted = false;

// Depth counted so a nested scope cannot clear the guard of its caller.
struct TranslateScope {
    TranslateScope() { ++g_translation_depth; }
    ~TranslateScope() { --g_translation_depth; }
};

bool looks_like_json(const std::string &text) {
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        return c == '{' || c == '[';
    }
    return false;
}

// Font selection is based on glyphs required by the rendered label. It never
// decides whether a translation is accepted or rejected.
bool needs_han_font(const std::string &text) {
    std::u16string units;
    if (!shared::utf8_to_utf16(text, &units)) return false;
    for (size_t index = 0; index < units.size(); ++index) {
        uint32_t point = units[index];
        if (point >= 0xd800 && point <= 0xdbff && index + 1 < units.size()) {
            const uint32_t low = units[index + 1];
            if (low >= 0xdc00 && low <= 0xdfff) {
                point = 0x10000 + ((point - 0xd800) << 10) + (low - 0xdc00);
                ++index;
            }
        }
        if ((point >= 0x3400 && point <= 0x9fff) ||
            (point >= 0xf900 && point <= 0xfaff) ||
            (point >= 0x20000 && point <= 0x3ffff)) return true;
    }
    return false;
}

bool first_param_is(const Il2CppApi &api, const void *method, const char *expected) {
    if (!method || !api.method_get_param || !api.type_get_name || !api.free_memory) return false;
    const void *parameter = api.method_get_param(method, 0);
    if (!parameter) return false;
    char *name = api.type_get_name(parameter);
    if (!name) return false;
    const bool matches = ::strcmp(name, expected) == 0;
    api.free_memory(name);
    return matches;
}

// UnityEngine.Object.m_Name, walking the class chain because the field is
// declared on UnityEngine.Object, not on TextAsset.
std::string unity_object_name(const Il2CppApi &api, void *instance, void *klass) {
    if (!instance || !klass || !api.class_get_field_from_name || !api.field_get_value ||
        !api.class_get_parent) {
        return std::string();
    }
    void *current = klass;
    for (int depth = 0; current != nullptr && depth < 8; ++depth) {
        const void *field = api.class_get_field_from_name(current, "m_Name");
        if (field) {
            void *value = nullptr;
            api.field_get_value(instance, field, &value);
            std::string text;
            if (!value || !managed_string_to_utf8(api, value, &text)) return std::string();
            return text;
        }
        current = api.class_get_parent(current);
    }
    return std::string();
}

// Unity font/TMP APIs are main-thread only, so the gate is retried from the
// text callbacks that run there. False means "keep returning the game text".
bool try_enable_translation() {
    RuntimeState &runtime = state();
    if (runtime.enabled.load()) return true;
    if (!runtime.hooks_installed.load()) return false;
    if (g_translation_depth != 0) return runtime.enabled.load();
    TranslateScope scope;
    if (!prepare_font()) return false;
    runtime.font_ready.store(true);
    runtime.enabled.store(true);
    // The background updater persists the new state. Never wait for companion
    // IPC in a Unity text callback.
    return true;
}

void *translate_text(const std::string &resource_key, void *original) {
    RuntimeState &runtime = state();
    if (!original || g_translation_depth != 0) return original;
    if (!try_enable_translation()) return original;
    std::string text;
    if (!managed_string_to_utf8(runtime.api, original, &text)) return original;
    if (text.empty() || !looks_like_json(text)) return original;
    const std::string digest = shared::Sha256::hex(text);
    std::string merged;
    std::string error;
    TranslateScope scope;
    if (!runtime.reader.lookup(resource_key, digest, &merged, &error)) {
        runtime.misses.fetch_add(1);
        return original;
    }
    if (merged.empty() || !looks_like_json(merged)) {
        runtime.rejects.fetch_add(1);
        return original;
    }
    void *replacement = managed_string_from_utf8(runtime.api, merged);
    if (!replacement) {
        runtime.rejects.fetch_add(1);
        return original;
    }
    runtime.hits.fetch_add(1);
    return replacement;
}

void *replacement_load_localize(void *instance, void *path, const void *method) {
    void *original =
        g_original_load_localize ? g_original_load_localize(instance, path, method) : nullptr;
    RuntimeState &runtime = state();
    if (!original || !path || !runtime.api.string_chars) return original;
    std::string key;
    if (!managed_string_to_utf8(runtime.api, path, &key)) return original;
    return translate_text(key, original);
}

void *replacement_text_asset_text(void *instance, const void *method) {
    void *original =
        g_original_text_asset_text ? g_original_text_asset_text(instance, method) : nullptr;
    RuntimeState &runtime = state();
    if (!original || !instance) return original;
    const std::string key = unity_object_name(runtime.api, instance, g_text_asset_class);
    if (key.empty()) {
        runtime.misses.fetch_add(1);
        return original;
    }
    return translate_text(key, original);
}

void replacement_ui_set_text(void *instance, void *managed_text, const void *method) {
    RuntimeState &runtime = state();
    // Ui.Text callbacks are main-thread work: the cheapest place to retry the
    // font gate that the monitor thread is not allowed to run.
    if (runtime.hooks_installed.load() && !runtime.enabled.load()) try_enable_translation();
    if (instance && managed_text && runtime.enabled.load()) {
        managed_text = translate_managed_string(managed_text, nullptr, nullptr, nullptr);
    }
    if (instance && managed_text && runtime.enabled.load() && g_legacy_font &&
        pthread_equal(pthread_self(), g_unity_main_thread)) {
        std::string text;
        void *exception = nullptr;
        void *id_box = runtime.api.runtime_invoke(g_get_instance_id, instance, nullptr, &exception);
        const void *id_value = !exception && id_box ? runtime.api.object_unbox(id_box) : nullptr;
        const int32_t instance_id = id_value ? *static_cast<const int32_t *>(id_value) : 0;
        if (instance_id != 0 && managed_string_to_utf8(runtime.api, managed_text, &text)) {
            const bool translated = needs_han_font(text);
            auto prior = g_original_ui_fonts.find(instance_id);
            if (translated && prior == g_original_ui_fonts.end() &&
                g_original_ui_fonts.size() < 1024) {
                exception = nullptr;
                void *font = runtime.api.runtime_invoke(g_ui_get_font, instance, nullptr, &exception);
                if (!exception && font && font != g_legacy_font) {
                    void *const font_handle = runtime.api.gchandle_new(font, false);
                    void *const weak_handle = runtime.api.gchandle_new_weakref(instance, false);
                    if (font_handle != nullptr && weak_handle != nullptr) {
                        void *args[1] = {g_legacy_font};
                        exception = nullptr;
                        runtime.api.runtime_invoke(g_ui_set_font, instance, args, &exception);
                        if (!exception) g_original_ui_fonts.emplace(instance_id,
                                                                    SavedFont{font_handle, weak_handle});
                    }
                    if (exception || font_handle == nullptr || weak_handle == nullptr) {
                        if (font_handle) runtime.api.gchandle_free(font_handle);
                        if (weak_handle) runtime.api.gchandle_free(weak_handle);
                    }
                }
            } else if (!translated && prior != g_original_ui_fonts.end()) {
                void *font = runtime.api.gchandle_get_target(prior->second.font_handle);
                if (font) {
                    void *args[1] = {font};
                    exception = nullptr;
                    runtime.api.runtime_invoke(g_ui_set_font, instance, args, &exception);
                }
                runtime.api.gchandle_free(prior->second.font_handle);
                runtime.api.gchandle_free(prior->second.weak_ui_handle);
                g_original_ui_fonts.erase(prior);
            }
            if (g_original_ui_fonts.size() > 512) {
                for (auto it = g_original_ui_fonts.begin(); it != g_original_ui_fonts.end();) {
                    if (runtime.api.gchandle_get_target(it->second.weak_ui_handle)) {
                        ++it;
                    } else {
                        runtime.api.gchandle_free(it->second.font_handle);
                        runtime.api.gchandle_free(it->second.weak_ui_handle);
                        it = g_original_ui_fonts.erase(it);
                    }
                }
            }
        }
    }
    if (g_original_ui_set_text) g_original_ui_set_text(instance, managed_text, method);
}

void replacement_tmp_set_text(void *instance, void *managed_text, const void *method) {
    RuntimeState &runtime = state();
    if (instance && managed_text && runtime.enabled.load()) {
        managed_text = translate_managed_string(managed_text, nullptr, nullptr, nullptr);
    }
    if (g_original_tmp_set_text) g_original_tmp_set_text(instance, managed_text, method);
}

void replacement_announcer_set_description(void *instance, void *managed_text,
                                           const void *method) {
    RuntimeState &runtime = state();
    if (instance && managed_text && runtime.enabled.load()) {
        managed_text = translate_managed_string(managed_text, nullptr, nullptr, nullptr);
    }
    if (g_original_announcer_set_text) {
        g_original_announcer_set_text(instance, managed_text, method);
    }
}

bool hook_method(RuntimeState &runtime, const void *method, void *replacement, void **original) {
    if (!method || !replacement || !original) return false;
    void *target = nullptr;
    ::memcpy(&target, method, sizeof(target));
    if (!target) return false;
    const size_t width = hook_width(reinterpret_cast<uintptr_t>(target),
                                    reinterpret_cast<uintptr_t>(replacement));
    Dl_info info = {};
    uintptr_t module_base = reinterpret_cast<uintptr_t>(runtime.api.base);
    if (::dladdr(target, &info) != 0 && info.dli_fbase) {
        module_base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    }
    if (!runtime.ledger.record(module_base, reinterpret_cast<uintptr_t>(target), width)) {
        return false;
    }
    MSHookFunction(target, replacement, original);
    runtime.ledger.commit(reinterpret_cast<uintptr_t>(target), width);
    return *original != nullptr;
}

} // namespace

bool install_text_hooks() {
    RuntimeState &runtime = state();
    if (!runtime.api.ready()) return false;
    void *manager = find_class(runtime.api, "TextAssetPatch", "TextAssetPatchManager");
    void *text_asset = find_class(runtime.api, "UnityEngine", "TextAsset");
    // UGUI is optional: when the build has no UnityEngine.UI.Text the two text
    // hooks still work, only the Ui.Text font swap is skipped.
    void *ui_text = find_class(runtime.api, "UnityEngine.UI", "Text");
    if (!manager || !text_asset) {
        log_warn("localization classes missing");
        return false;
    }
    const void *load_method =
        find_method(runtime.api, manager, "LoadLocalizeFileSync", "System.String", 1);
    const void *text_method = find_method(runtime.api, text_asset, "get_text", "System.String", 0);
    const void *ui_method =
        ui_text ? find_method(runtime.api, ui_text, "set_text", "System.Void", 1) : nullptr;
    if (ui_method) {
        g_ui_get_font = find_method(runtime.api, ui_text, "get_font", "UnityEngine.Font", 0);
        g_ui_set_font = find_method(runtime.api, ui_text, "set_font", "System.Void", 1);
        void *unity_object = find_class(runtime.api, "UnityEngine", "Object");
        g_get_instance_id =
            find_method(runtime.api, unity_object, "GetInstanceID", "System.Int32", 0);
    }
    if (!load_method || !text_method) {
        log_warn("localization methods missing");
        return false;
    }
    uint32_t baseline_crc = 0;
    if (!verify_section(reinterpret_cast<uintptr_t>(runtime.api.base), runtime.profile,
                        nullptr, &baseline_crc)) {
        log_warn("integrity profile does not match live code; hooks skipped");
        return false;
    }
    if (!hook_method(runtime, load_method, reinterpret_cast<void *>(replacement_load_localize),
                     reinterpret_cast<void **>(&g_original_load_localize))) {
        runtime.ledger.restore_module(reinterpret_cast<uintptr_t>(runtime.api.base));
        return false;
    }
    if (!hook_method(runtime, text_method, reinterpret_cast<void *>(replacement_text_asset_text),
                     reinterpret_cast<void **>(&g_original_text_asset_text))) {
        runtime.ledger.restore_module(reinterpret_cast<uintptr_t>(runtime.api.base));
        g_original_load_localize = nullptr;
        return false;
    }
    if (ui_method && g_ui_get_font && g_ui_set_font && g_get_instance_id) {
        if (!hook_method(runtime, ui_method, reinterpret_cast<void *>(replacement_ui_set_text),
                         reinterpret_cast<void **>(&g_original_ui_set_text))) {
            // The patcher can fail after touching the target. The ledger only
            // supports whole-transaction rollback, so do not leave an unknown
            // optional patch beside otherwise active hooks.
            runtime.ledger.restore_module(reinterpret_cast<uintptr_t>(runtime.api.base));
            g_original_load_localize = nullptr;
            g_original_text_asset_text = nullptr;
            g_original_ui_set_text = nullptr;
            log_warn("ui font hook rejected");
            return false;
        }
    } else {
        log_warn("ui font hook unavailable");
    }
    void *tmp_text = find_class(runtime.api, "TMPro", "TMP_Text");
    const MethodSpec tmp_spec{"set_text", "System.String", "System.Void", false};
    const void *tmp_method = tmp_text ? find_verified_method(runtime.api, tmp_text, tmp_spec) : nullptr;
    if (tmp_method) {
        if (!hook_method(runtime, tmp_method, reinterpret_cast<void *>(replacement_tmp_set_text),
                         reinterpret_cast<void **>(&g_original_tmp_set_text))) {
            runtime.ledger.restore_module(reinterpret_cast<uintptr_t>(runtime.api.base));
            g_original_load_localize = nullptr;
            g_original_text_asset_text = nullptr;
            g_original_ui_set_text = nullptr;
            g_original_tmp_set_text = nullptr;
            log_warn("tmp text hook rejected");
            return false;
        }
    } else {
        log_warn("tmp text hook unavailable");
    }
    void *announcer = find_class(runtime.api, "BattleUI.Announcer", "BattleAnnouncerUI");
    if (!announcer) announcer = find_class(runtime.api, "BattleUI", "Announcer");
    const MethodSpec announcer_spec{"SetDescription", "System.String", "System.Void", false};
    const void *announcer_method =
        announcer ? find_verified_method(runtime.api, announcer, announcer_spec) : nullptr;
    if (announcer_method) {
        if (!hook_method(runtime, announcer_method,
                         reinterpret_cast<void *>(replacement_announcer_set_description),
                         reinterpret_cast<void **>(&g_original_announcer_set_text))) {
            runtime.ledger.restore_module(reinterpret_cast<uintptr_t>(runtime.api.base));
            g_original_load_localize = nullptr;
            g_original_text_asset_text = nullptr;
            g_original_ui_set_text = nullptr;
            g_original_tmp_set_text = nullptr;
            g_original_announcer_set_text = nullptr;
            log_warn("announcer hook rejected");
            return false;
        }
    } else {
        log_warn("announcer hook unavailable");
    }
    g_text_asset_class = text_asset;
    uint32_t live = 0;
    if (!verify_section(reinterpret_cast<uintptr_t>(runtime.api.base), runtime.profile,
                        &runtime.ledger, &live)) {
        // Our recorded pre-patch bytes did not match the installed APK profile.
        // Roll back our changes; this says nothing about an external checker.
        log_warn("local integrity check rejected hooks");
        runtime.ledger.restore_module(reinterpret_cast<uintptr_t>(runtime.api.base));
        g_original_load_localize = nullptr;
        g_original_text_asset_text = nullptr;
        g_original_ui_set_text = nullptr;
        g_original_tmp_set_text = nullptr;
        g_text_asset_class = nullptr;
        return false;
    }
    log_info("text hooks installed");
    return true;
}

bool verify_font_snapshot() {
    RuntimeState &runtime = state();
    runtime.font_digest_verified.store(false);
    if (!runtime.reader.has_font() || runtime.reader.font_sha256().empty()) return false;
    const int fd = runtime.reader.font_fd();
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0) return false;
    shared::Sha256 digest;
    std::vector<uint8_t> buffer(64 * 1024);
    off_t offset = 0;
    while (offset < info.st_size) {
        const size_t wanted = std::min(static_cast<off_t>(buffer.size()), info.st_size - offset);
        const ssize_t got = ::pread(fd, buffer.data(), wanted, offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        digest.update(buffer.data(), static_cast<size_t>(got));
        offset += got;
    }
    uint8_t hash[32];
    digest.finish(hash);
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (const uint8_t byte : hash) {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 15]);
    }
    const bool valid = offset == info.st_size && hex == runtime.reader.font_sha256();
    runtime.font_digest_verified.store(valid);
    if (!valid) log_warn("font digest mismatch");
    return valid;
}

bool prepare_font() {
    RuntimeState &runtime = state();
    const Il2CppApi &api = runtime.api;
    if (!runtime.font_digest_verified.load() || !api.ready() ||
        !api.object_unbox || !api.object_get_class || !api.object_new || !api.gchandle_new ||
        !api.gchandle_new_weakref || !api.gchandle_free || !api.gchandle_get_target) {
        return false;
    }
    // Font and TMP calls are main-thread only. A call from the monitor thread is
    // not an attempt: the text callbacks retry on the real main thread.
    if (!runtime.main_thread_known.load() || !pthread_equal(pthread_self(), runtime.main_thread)) {
        return false;
    }
    void *exception = nullptr;
    pthread_mutex_lock(&g_font_mutex);
    if (g_font_handle != nullptr) {
        pthread_mutex_unlock(&g_font_mutex);
        return true;
    }
    if (g_font_attempted) {
        pthread_mutex_unlock(&g_font_mutex);
        return false;
    }
    void *settings = find_class(api, "TMPro", "TMP_Settings");
    void *font_class = find_class(api, "TMPro", "TMP_FontAsset");
    void *unity_font_class = find_class(api, "UnityEngine", "Font");
    const void *fallback_getter = find_method(api, settings, "get_fallbackFontAssets", nullptr, 0);
    const void *create = find_method(api, font_class, "CreateFontAsset", "TMPro.TMP_FontAsset", 9);
    const void *try_add = find_method(api, font_class, "TryAddCharacters", "System.Boolean", 1);
    const void *font_ctor = find_method(api, unity_font_class, ".ctor", "System.Void", 1);
    const void *font_dynamic = find_method(api, unity_font_class, "get_dynamic", "System.Boolean", 0);
    const void *has_character = find_method(api, unity_font_class, "HasCharacter", "System.Boolean", 1);
    if (!fallback_getter || !create || !try_add || !font_ctor || !font_dynamic ||
        !has_character || !first_param_is(api, create, "System.String") ||
        !first_param_is(api, try_add, "System.String") ||
        !first_param_is(api, font_ctor, "System.String") ||
        !first_param_is(api, has_character, "System.Int32")) {
        pthread_mutex_unlock(&g_font_mutex);
        return false;
    }
    exception = nullptr;
    void *fallbacks = api.runtime_invoke(fallback_getter, nullptr, nullptr, &exception);
    void *list_class = !exception && fallbacks ? api.object_get_class(fallbacks) : nullptr;
    const void *add = list_class ? api.class_get_method_from_name(list_class, "Add", 1) : nullptr;
    if (!add) {
        pthread_mutex_unlock(&g_font_mutex);
        return false;
    }

    const int fd = runtime.reader.font_fd();
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    void *name = managed_string_from_utf8(api, path);
    if (!name) {
        g_font_attempted = true;
        pthread_mutex_unlock(&g_font_mutex);
        return false;
    }
    void *const name_handle = api.gchandle_new(name, false);
    if (name_handle == nullptr) {
        g_font_attempted = true;
        pthread_mutex_unlock(&g_font_mutex);
        return false;
    }
    int32_t face_index = 0, sampling_size = 34, padding = 5;
    int32_t render_mode = 4169, atlas_width = 4096, atlas_height = 4096;
    int32_t population_mode = 1;
    uint8_t multi_atlas = 1;
    void *arguments[9] = {name, &face_index, &sampling_size, &padding, &render_mode,
                          &atlas_width, &atlas_height, &population_mode, &multi_atlas};
    exception = nullptr;
    void *asset = api.runtime_invoke(create, nullptr, arguments, &exception);
    api.gchandle_free(name_handle);
    if (exception || !asset) {
        g_font_attempted = true;
        pthread_mutex_unlock(&g_font_mutex);
        log_warn("TMP font creation failed");
        return false;
    }
    void *const handle = api.gchandle_new(asset, false);
    if (handle == nullptr) {
        g_font_attempted = true;
        pthread_mutex_unlock(&g_font_mutex);
        return false;
    }
    void *probe = managed_string_from_utf8(api, u8"\u6c49\u8bd1\u8111\u9176");
    void *const probe_handle = probe ? api.gchandle_new(probe, false) : nullptr;
    void *probe_arguments[1] = {probe};
    exception = nullptr;
    void *probe_result = probe_handle != nullptr
                             ? api.runtime_invoke(try_add, asset, probe_arguments, &exception)
                             : nullptr;
    if (probe_handle != nullptr) api.gchandle_free(probe_handle);
    const void *probe_value = !exception && probe_result ? api.object_unbox(probe_result) : nullptr;
    if (!probe_value || *static_cast<const uint8_t *>(probe_value) == 0) {
        api.gchandle_free(handle);
        g_font_attempted = true;
        pthread_mutex_unlock(&g_font_mutex);
        log_warn("TMP Chinese glyph probe failed");
        return false;
    }
    void *legacy = api.object_new(unity_font_class);
    void *const legacy_handle = legacy ? api.gchandle_new(legacy, false) : nullptr;
    void *legacy_name = managed_string_from_utf8(api, path);
    void *const legacy_name_handle = legacy_name ? api.gchandle_new(legacy_name, false) : nullptr;
    if (legacy_handle == nullptr || legacy_name_handle == nullptr) {
        if (legacy_name_handle) api.gchandle_free(legacy_name_handle);
        if (legacy_handle) api.gchandle_free(legacy_handle);
        api.gchandle_free(handle);
        g_font_attempted = true;
        pthread_mutex_unlock(&g_font_mutex);
        return false;
    }
    void *ctor_args[1] = {legacy_name};
    exception = nullptr;
    api.runtime_invoke(font_ctor, legacy, ctor_args, &exception);
    api.gchandle_free(legacy_name_handle);
    void *dynamic_box = !exception ? api.runtime_invoke(font_dynamic, legacy, nullptr, &exception)
                                   : nullptr;
    const void *dynamic_value = !exception && dynamic_box ? api.object_unbox(dynamic_box) : nullptr;
    bool legacy_valid = dynamic_value && *static_cast<const uint8_t *>(dynamic_value) != 0;
    for (int32_t glyph : {0x6c49, 0x8bd1, 0x8111, 0x9176}) {
        if (!legacy_valid) break;
        void *glyph_args[1] = {&glyph};
        exception = nullptr;
        void *glyph_box = api.runtime_invoke(has_character, legacy, glyph_args, &exception);
        const void *glyph_value = !exception && glyph_box ? api.object_unbox(glyph_box) : nullptr;
        legacy_valid = glyph_value && *static_cast<const uint8_t *>(glyph_value) != 0;
    }
    if (!legacy_valid) {
        api.gchandle_free(legacy_handle);
        api.gchandle_free(handle);
        g_font_attempted = true;
        pthread_mutex_unlock(&g_font_mutex);
        log_warn("legacy Chinese font validation failed");
        return false;
    }
    void *add_arguments[1] = {asset};
    exception = nullptr;
    api.runtime_invoke(add, fallbacks, add_arguments, &exception);
    if (exception) {
        api.gchandle_free(legacy_handle);
        api.gchandle_free(handle);
        g_font_attempted = true;
        pthread_mutex_unlock(&g_font_mutex);
        log_warn("TMP fallback registration failed");
        return false;
    }
    g_font_handle = handle;
    g_legacy_font = legacy;
    g_legacy_font_handle = legacy_handle;
    g_unity_main_thread = runtime.main_thread;
    pthread_mutex_unlock(&g_font_mutex);
    log_info("TMP Chinese font registered");
    return true;
}

} // namespace bybus::runtime

