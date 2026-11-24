#include "runtime/hooks.h"

#include <dlfcn.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "CydiaSubstrate.h"
#include "runtime/flow_text_proof.h"
#include "runtime/font_materials.h"
#include "runtime/font_proof.h"
#include "runtime/font_scale.h"
#include "runtime/rich_text.h"
#include "runtime/text_provenance.h"
#include "runtime/il2cpp.h"
#include "runtime/log.h"
#include "runtime/method_flags.h"
#include "runtime/state.h"
#include "runtime/text_router.h"
#include "runtime/type_signature.h"
#include "shared/sha256.h"
#include "shared/utf.h"
#include "translation/text_policy.h"

namespace bybus::runtime {
namespace {

typedef void *(*load_localize_fn)(void *instance, void *path, const void *method);
typedef void *(*text_asset_text_fn)(void *instance, const void *method);
typedef void (*ui_set_text_fn)(void *instance, void *text, const void *method);
typedef void (*tmp_set_text_fn)(void *instance, void *text, const void *method);
typedef void (*tmp_set_text_string_fn)(void *instance, void *text, bool sync_input,
                                       const void *method);
typedef void (*announcer_set_text_fn)(void *instance, void *text, const void *method);
typedef void (*story_dialog_ctor_fn)(void *instance, int32_t arg0, int32_t arg1, void *script,
                                     void *effects, float time, const void *method);
typedef void *(*story_scenario_fn)(void *instance, void *scenario_id, int32_t language,
                                   const void *method);
typedef void *(*rpg_format_fn)(void *format, void *arguments, const void *method);
typedef void *(*rpg_dialogue_text_fn)(void *instance, int32_t index, const void *method);
typedef void (*choice_set_text_fn)(void *instance, void *text, const void *method);
typedef void (*choice_total_fn)(void *instance, void *list, const void *method);
typedef void (*choice_read_content_fn)(void *instance, void *text, bool instant,
                                       const void *method);
typedef void (*choice_void_fn)(void *instance, const void *method);
typedef void (*notice_set_data_fn)(void *instance, void *text, const void *method);
typedef void (*choice_button_fn)(void *instance, int32_t selection, void *message, void *result,
                                 const void *method);
typedef void *(*tmp_get_object_fn)(void *instance, const void *method);
typedef void (*tmp_set_object_fn)(void *instance, void *value, const void *method);
typedef void *(*static_material_pair_fn)(void *source, void *target, const void *method);
typedef void (*static_void_fn)(void *value, const void *method);
typedef float (*material_float_getter_fn)(void *instance, void *name, const void *method);
typedef void (*material_float_setter_fn)(void *instance, void *name, float value,
                                         const void *method);
typedef void *(*material_texture_fn)(void *instance, void *name, const void *method);
typedef void (*material_writer_fn)(void *instance, const void *method);
typedef int32_t (*add_material_fn)(void *material, void *font, void **references,
                                   void *lookup, const void *method);
typedef void *(*rich_text_one_fn)(void *text, const void *method);
typedef void *(*rich_text_all_fn)(void *text, bool gray, const void *method);
typedef void *(*rich_text_color_fn)(void *text, void *color, const void *method);
typedef void *(*rich_text_skill_fn)(void *text, bool gray, bool highlight, const void *method);
typedef void *(*ruby_format_fn)(void *instance, void *text, const void *method);

load_localize_fn g_original_load_localize = nullptr;
text_asset_text_fn g_original_text_asset_text = nullptr;
ui_set_text_fn g_original_ui_set_text = nullptr;
tmp_set_text_fn g_original_tmp_set_text = nullptr;
tmp_set_text_string_fn g_original_tmp_set_text_string = nullptr;
announcer_set_text_fn g_original_announcer_set_text = nullptr;
story_dialog_ctor_fn g_original_story_dialog_ctor = nullptr;
story_scenario_fn g_original_story_scenario = nullptr;
rpg_format_fn g_original_rpg_format = nullptr;
thread_local std::string g_story_scene_key;
rpg_dialogue_text_fn g_original_rpg_dialogue_text = nullptr;
choice_set_text_fn g_original_choice_set_text = nullptr;
choice_total_fn g_original_choice_total = nullptr;
choice_read_content_fn g_original_choice_read_content = nullptr;
choice_void_fn g_original_choice_clear = nullptr;
choice_void_fn g_original_choice_disable = nullptr;
notice_set_data_fn g_original_notice_set_data = nullptr;
choice_button_fn g_original_choice_button = nullptr;
void *g_tmp_font_asset = nullptr;
const void *g_tmp_set_font_method = nullptr;
tmp_set_object_fn g_original_tmp_set_font = nullptr;
const void *g_tmp_get_font_method = nullptr;
const void *g_tmp_get_shared_material_method = nullptr;
const void *g_tmp_set_shared_material_method = nullptr;
const void *g_tmp_set_material_method = nullptr;
tmp_set_object_fn g_original_tmp_set_shared_material = nullptr;
tmp_set_object_fn g_original_tmp_set_material = nullptr;
const void *g_tmp_get_text_method = nullptr;
const void *g_tmp_get_font_size_method = nullptr;
const void *g_tmp_get_overflow_method = nullptr;
const void *g_tmp_set_overflow_method = nullptr;
const void *g_tmp_get_wrapping_method = nullptr;
const void *g_tmp_set_wrapping_method = nullptr;
const void *g_tmp_get_word_wrapping_method = nullptr;
const void *g_tmp_set_word_wrapping_method = nullptr;
const void *g_tmp_get_rect_transform_method = nullptr;
const void *g_rect_get_rect_method = nullptr;
const void *g_font_get_material_method = nullptr;
const void *g_font_get_face_info_method = nullptr;
const void *g_face_get_point_size_method = nullptr;
const void *g_material_manager_get_fallback = nullptr;
const void *g_material_manager_add_reference = nullptr;
const void *g_material_manager_release = nullptr;
const void *g_material_get_float_method = nullptr;
const void *g_material_set_float_method = nullptr;
const void *g_material_get_texture_method = nullptr;
const void *g_material_writer_get_text_method = nullptr;
const void *g_material_writer_write_method = nullptr;
material_writer_fn g_original_material_writer_write = nullptr;
add_material_fn g_original_add_material = nullptr;
thread_local void *g_active_tmp_text = nullptr;
thread_local bool g_inside_tmp_material = false;
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
// Counted only for a real TMP call on the Unity main thread; a call from another
// thread, or a failure before the first runtime_invoke, is not an attempt.
unsigned g_font_attempts = 0;
// True once the localization entry hooks (LoadLocalizeFileSync and
// TextAsset.get_text) own a trampoline. Partial hook failures never clear it.
bool g_text_entry_installed = false;

// Per-component restoration data. Strong handles keep the game's original font
// and material alive while a component uses the Chinese fallback; the weak
// component handle lets recycled UI objects be detected without pinning them.
struct TmpComponentState {
    void *component = nullptr;
    void *weak_component = nullptr;
    void *font_handle = nullptr;
    void *material_handle = nullptr;
    void *converted_handle = nullptr;
    void *converted = nullptr;
    void *conversion_source = nullptr;
    bool holds_conversion = false;
    bool applied = false;
    bool font_known = false;
    bool material_known = false;
    int32_t wrapping = 0;
    int32_t overflow = 0;
    bool has_wrapping = false;
    bool has_overflow = false;
    bool layout_saved = false;
    bool overflow_changed = false;
    bool wrapping_changed = false;
    int32_t applied_overflow = 0;
    int32_t applied_wrapping = 0;
    limbus_font_proof::FlowProof flow;
};
std::unordered_map<void *, TmpComponentState> g_tmp_states;
// Converted material presets can be shared by several labels (selected,
// disabled, inline style). Keep their source identity and TMP reference count.
struct MaterialConversion {
    void *source_handle = nullptr;
    void *converted_handle = nullptr;
    unsigned references = 0;
};
std::unordered_map<void *, MaterialConversion> g_material_conversions;

// Depth counted so a nested scope cannot clear the guard of its caller.
struct TranslateScope {
    TranslateScope() { ++g_translation_depth; }
    ~TranslateScope() { --g_translation_depth; }
};

// TMP's parser runs inside set_text. Recording the component here lets the
// MaterialReference hook convert inline <font>/<style> presets for exactly that
// label instead of replacing globally cached game materials.
struct ActiveTmpScope {
    void *previous;
    explicit ActiveTmpScope(void *instance) : previous(g_active_tmp_text) {
        g_active_tmp_text = instance;
    }
    ~ActiveTmpScope() { g_active_tmp_text = previous; }
};

struct MaterialScope {
    bool previous;
    MaterialScope() : previous(g_inside_tmp_material) { g_inside_tmp_material = true; }
    ~MaterialScope() { g_inside_tmp_material = previous; }
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
bool first_param_is(const Il2CppApi &api, const void *method, const char *expected) {
    if (!method || !expected || !api.method_get_param || !api.type_get_name ||
        !api.free_memory) {
        return false;
    }
    const void *parameter = api.method_get_param(method, 0);
    if (!parameter) return false;
    char *name = api.type_get_name(parameter);
    if (!name) return false;
    const bool matches =
        type_signature::normalize(name) == type_signature::normalize(expected);
    api.free_memory(name);
    return matches;
}

bool first_param_contains(const Il2CppApi &api, const void *method, const char *needle) {
    if (!method || !needle || !api.method_get_param || !api.type_get_name ||
        !api.free_memory) {
        return false;
    }
    const void *parameter = api.method_get_param(method, 0);
    if (!parameter) return false;
    char *name = api.type_get_name(parameter);
    if (!name) return false;
    const bool matches = type_signature::normalize(name).find(
                             type_signature::normalize(needle)) != std::string::npos;
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
    if (!prepare_font()) {
        // Only a verified snapshot can reach the live component stage. A
        // missing verification or a non-main-thread call is not a component
        // rejection and must not be reported as one.
        if (runtime.font_digest_verified.load() && g_font_attempts > 0)
            runtime.font_component_rejected.store(true);
        return false;
    }
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

void *invoke_object_getter(const void *method, void *instance) {
    RuntimeState &runtime = state();
    if (!method || !runtime.api.runtime_invoke) return nullptr;
    void *exception = nullptr;
    void *value = runtime.api.runtime_invoke(method, instance, nullptr, &exception);
    return exception ? nullptr : value;
}

bool invoke_object_setter(const void *method, tmp_set_object_fn original, void *instance,
                          void *value) {
    if (!method || !instance) return false;
    if (original) {
        original(instance, value, method);
        return true;
    }
    void *target = nullptr;
    ::memcpy(&target, method, sizeof(target));
    if (!target) return false;
    reinterpret_cast<tmp_set_object_fn>(target)(instance, value, method);
    return true;
}

bool invoke_scalar_getter(const void *method, void *instance, void *out, size_t size) {
    RuntimeState &runtime = state();
    if (!method || !instance || !out || !runtime.api.runtime_invoke ||
        !runtime.api.object_unbox) return false;
    void *exception = nullptr;
    void *boxed = runtime.api.runtime_invoke(method, instance, nullptr, &exception);
    const void *value = !exception && boxed ? runtime.api.object_unbox(boxed) : nullptr;
    if (!value) return false;
    ::memcpy(out, value, size);
    return true;
}

bool invoke_scalar_setter(const void *method, void *instance, const void *value, size_t size) {
    RuntimeState &runtime = state();
    if (!method || !instance || !value || !runtime.api.runtime_invoke) return false;
    void *argument = const_cast<void *>(value);
    void *args[1] = {argument};
    void *exception = nullptr;
    runtime.api.runtime_invoke(method, instance, args, &exception);
    (void)size;
    return exception == nullptr;
}

struct ComponentRect {
    float width = 0;
    float height = 0;
    bool valid = false;
};

ComponentRect component_rect(void *instance) {
    ComponentRect result;
    RuntimeState &runtime = state();
    if (!instance || !g_tmp_get_rect_transform_method || !g_rect_get_rect_method ||
        !runtime.api.runtime_invoke || !runtime.api.object_unbox) return result;
    void *transform = invoke_object_getter(g_tmp_get_rect_transform_method, instance);
    if (!transform) return result;
    void *exception = nullptr;
    void *boxed = runtime.api.runtime_invoke(g_rect_get_rect_method, transform, nullptr,
                                             &exception);
    const void *rect = !exception && boxed ? runtime.api.object_unbox(boxed) : nullptr;
    if (!rect) return result;
    float values[4] = {};
    ::memcpy(values, rect, sizeof(values));
    result.width = values[2];
    result.height = values[3];
    result.valid = result.width > 0 && result.width < 100000 && result.height > 0 &&
                   result.height < 100000;
    return result;
}

bool read_tmp_wrapping(void *instance, int32_t *mode) {
    if (g_tmp_get_wrapping_method && g_tmp_set_wrapping_method) {
        return invoke_scalar_getter(g_tmp_get_wrapping_method, instance, mode, sizeof(*mode)) &&
               *mode >= 0 && *mode <= 3;
    }
    bool enabled = false;
    if (!g_tmp_get_word_wrapping_method ||
        !invoke_scalar_getter(g_tmp_get_word_wrapping_method, instance, &enabled,
                              sizeof(enabled))) return false;
    *mode = enabled ? 1 : 0;
    return true;
}

void write_tmp_wrapping(void *instance, int32_t mode) {
    if (g_tmp_set_wrapping_method) {
        invoke_scalar_setter(g_tmp_set_wrapping_method, instance, &mode, sizeof(mode));
    } else if (g_tmp_set_word_wrapping_method) {
        const bool enabled = mode == 1 || mode == 2;
        invoke_scalar_setter(g_tmp_set_word_wrapping_method, instance, &enabled, sizeof(enabled));
    }
}

bool material_number(void *material, const char *property, float *value) {
    RuntimeState &runtime = state();
    if (!material || !property || !value || !g_material_get_float_method ||
        !runtime.api.runtime_invoke || !runtime.api.object_unbox) return false;
    void *name = managed_string_from_utf8(runtime.api, property);
    if (!name) return false;
    void *args[1] = {name};
    void *exception = nullptr;
    void *boxed = runtime.api.runtime_invoke(g_material_get_float_method, material, args, &exception);
    const void *unboxed = !exception && boxed ? runtime.api.object_unbox(boxed) : nullptr;
    if (!unboxed) return false;
    ::memcpy(value, unboxed, sizeof(*value));
    return std::isfinite(*value);
}

bool material_set_number(void *material, const char *property, float value) {
    RuntimeState &runtime = state();
    if (!material || !property || !g_material_set_float_method || !runtime.api.runtime_invoke)
        return false;
    void *name = managed_string_from_utf8(runtime.api, property);
    if (!name) return false;
    void *args[2] = {name, &value};
    void *exception = nullptr;
    runtime.api.runtime_invoke(g_material_set_float_method, material, args, &exception);
    return exception == nullptr;
}

void *material_texture(void *material) {
    RuntimeState &runtime = state();
    if (!material || !g_material_get_texture_method || !runtime.api.runtime_invoke) return nullptr;
    void *name = managed_string_from_utf8(runtime.api, "_MainTex");
    if (!name) return nullptr;
    void *args[1] = {name};
    void *exception = nullptr;
    void *texture = runtime.api.runtime_invoke(g_material_get_texture_method, material, args,
                                               &exception);
    return exception ? nullptr : texture;
}

float font_point_size(void *font) {
    RuntimeState &runtime = state();
    if (!font || !g_font_get_face_info_method || !g_face_get_point_size_method ||
        !runtime.api.runtime_invoke || !runtime.api.object_unbox) return -1;
    void *exception = nullptr;
    void *boxed_face = runtime.api.runtime_invoke(g_font_get_face_info_method, font, nullptr,
                                                   &exception);
    void *face = !exception && boxed_face ? runtime.api.object_unbox(boxed_face) : nullptr;
    if (!face) return -1;
    // A value-type instance method receives the unboxed data pointer.
    exception = nullptr;
    void *boxed = runtime.api.runtime_invoke(g_face_get_point_size_method, face, nullptr,
                                             &exception);
    const void *value = !exception && boxed ? runtime.api.object_unbox(boxed) : nullptr;
    float point = -1;
    if (!value) return -1;
    ::memcpy(&point, value, sizeof(point));
    return std::isfinite(point) && point > 0 ? point : -1;
}

bool static_material_reference(const void *method, void *material, bool add) {
    RuntimeState &runtime = state();
    if (!method || !material || !runtime.api.runtime_invoke) return false;
    void *args[1] = {material};
    void *exception = nullptr;
    runtime.api.runtime_invoke(method, nullptr, args, &exception);
    if (exception) return false;
    (void)add;
    return true;
}

void free_state(TmpComponentState *saved) {
    RuntimeState &runtime = state();
    if (!saved || !runtime.api.gchandle_free) return;
    if (saved->weak_component) runtime.api.gchandle_free(saved->weak_component);
    if (saved->font_handle) runtime.api.gchandle_free(saved->font_handle);
    if (saved->material_handle) runtime.api.gchandle_free(saved->material_handle);
    saved->weak_component = nullptr;
    saved->font_handle = nullptr;
    saved->material_handle = nullptr;
}

bool on_unity_main_thread();

void release_conversion(void *source) {
    RuntimeState &runtime = state();
    auto found = g_material_conversions.find(source);
    if (found == g_material_conversions.end()) return;
    void *converted = found->second.converted_handle
                          ? runtime.api.gchandle_get_target(found->second.converted_handle)
                          : nullptr;
    if (found->second.references > 1) {
        --found->second.references;
        if (g_material_manager_release && converted)
            static_material_reference(g_material_manager_release, converted, false);
        return;
    }
    if (g_material_manager_release && converted)
        static_material_reference(g_material_manager_release, converted, false);
    if (found->second.source_handle) runtime.api.gchandle_free(found->second.source_handle);
    if (found->second.converted_handle)
        runtime.api.gchandle_free(found->second.converted_handle);
    g_material_conversions.erase(found);
}

bool convert_material(void *source, void **out_converted, void **out_font_material) {
    RuntimeState &runtime = state();
    if (out_converted) *out_converted = nullptr;
    if (out_font_material) *out_font_material = nullptr;
    if (!source || !state().enabled.load() || !on_unity_main_thread() || !g_tmp_font_asset ||
        !g_font_get_material_method || !g_material_manager_get_fallback ||
        !g_material_manager_add_reference || !runtime.api.runtime_invoke) return false;
    void *font_material = invoke_object_getter(g_font_get_material_method, g_tmp_font_asset);
    if (out_font_material) *out_font_material = font_material;
    if (!font_material || font_material == source) return false;
    void *source_atlas = material_texture(source);
    void *font_atlas = material_texture(font_material);
    if (source_atlas && font_atlas && source_atlas == font_atlas) return false;
    if (g_material_conversions.size() >= 4096) return false;
    auto found = g_material_conversions.find(source);
    if (found != g_material_conversions.end()) {
        void *object = runtime.api.gchandle_get_target(found->second.converted_handle);
        if (object && runtime.api.gchandle_get_target(found->second.source_handle) == source) {
            ++found->second.references;
            static_material_reference(g_material_manager_add_reference, object, true);
            if (out_converted) *out_converted = object;
            return true;
        }
        release_conversion(source);
    }
    void *args[2] = {source, font_material};
    void *exception = nullptr;
    void *converted = runtime.api.runtime_invoke(g_material_manager_get_fallback, nullptr, args,
                                                 &exception);
    if (exception || !converted) return false;
    if (!static_material_reference(g_material_manager_add_reference, converted, true))
        return false;
    MaterialConversion record;
    record.source_handle = runtime.api.gchandle_new(source, false);
    record.converted_handle = runtime.api.gchandle_new(converted, false);
    record.references = 1;
    if (!record.source_handle || !record.converted_handle) {
        if (record.source_handle) runtime.api.gchandle_free(record.source_handle);
        if (record.converted_handle) runtime.api.gchandle_free(record.converted_handle);
        static_material_reference(g_material_manager_release, converted, false);
        return false;
    }
    const auto inserted = g_material_conversions.emplace(source, record);
    if (!inserted.second) {
        if (record.source_handle) runtime.api.gchandle_free(record.source_handle);
        if (record.converted_handle) runtime.api.gchandle_free(record.converted_handle);
        static_material_reference(g_material_manager_release, converted, false);
        return false;
    }
    if (out_converted) *out_converted = converted;
    return true;
}

void bind_material_ratios(void *source, void *target, void *source_font, void *target_font) {
    if (!source || !target || !source_font || !target_font) return;
    float source_gradient = 0, target_gradient = 0;
    if (!material_number(source, "_GradientScale", &source_gradient) ||
        !material_number(target, "_GradientScale", &target_gradient) ||
        source_gradient <= 1 || target_gradient <= 1) return;
    limbus_font_scale::Basis basis;
    basis.point = font_point_size(source_font);
    basis.target_point = font_point_size(target_font);
    basis.gradient = source_gradient;
    basis.normal = material_number(source, "_WeightNormal", &basis.normal) ? basis.normal : 0;
    basis.bold = material_number(source, "_WeightBold", &basis.bold) ? basis.bold : 0;
    if (!basis.valid()) return;
    limbus_font_scale::Style style;
    material_number(source, "_FaceDilate", &style.face);
    material_number(source, "_OutlineWidth", &style.outline);
    material_number(source, "_OutlineSoftness", &style.softness);
    material_number(source, "_GlowOffset", &style.glow_offset);
    material_number(source, "_GlowOuter", &style.glow_outer);
    material_number(source, "_UnderlayOffsetX", &style.shadow_x);
    material_number(source, "_UnderlayOffsetY", &style.shadow_y);
    material_number(source, "_UnderlayDilate", &style.shadow_dilate);
    material_number(source, "_UnderlaySoftness", &style.shadow_softness);
    const auto ratios = limbus_font_scale::calculate(basis, style, target_gradient);
    if (!std::isfinite(ratios.a) || !std::isfinite(ratios.b) || !std::isfinite(ratios.c)) return;
    material_set_number(target, "_ScaleRatioA", ratios.a);
    material_set_number(target, "_ScaleRatioB", ratios.b);
    material_set_number(target, "_ScaleRatioC", ratios.c);
}

void *conversion_source_for(void *converted);

bool on_unity_main_thread() {
    const RuntimeState &runtime = state();
    return runtime.main_thread_known.load() && pthread_equal(pthread_self(), runtime.main_thread);
}

bool find_tmp_state(void *instance, TmpComponentState *out) {
    RuntimeState &runtime = state();
    if (!instance || !runtime.api.gchandle_get_target) return false;
    auto found = g_tmp_states.find(instance);
    if (found == g_tmp_states.end()) return false;
    if (runtime.api.gchandle_get_target(found->second.weak_component) != instance) {
        if (found->second.holds_conversion && found->second.conversion_source)
            release_conversion(found->second.conversion_source);
        free_state(&found->second);
        g_tmp_states.erase(found);
        return false;
    }
    if (out) *out = found->second;
    return true;
}

bool capture_tmp_state(void *instance, TmpComponentState *out) {
    RuntimeState &runtime = state();
    if (!instance || !on_unity_main_thread() || !g_tmp_font_asset || !g_tmp_get_font_method ||
        !g_tmp_get_shared_material_method || !runtime.api.gchandle_new ||
        !runtime.api.gchandle_new_weakref) return false;
    TmpComponentState current;
    current.component = instance;
    current.weak_component = runtime.api.gchandle_new_weakref(instance, false);
    void *font = invoke_object_getter(g_tmp_get_font_method, instance);
    void *material = invoke_object_getter(g_tmp_get_shared_material_method, instance);
    // If state was lost after a previous switch, the live values are already
    // ours. Never record the Chinese fallback as the label's original font.
    if (font == g_tmp_font_asset) font = nullptr;
    void *chinese_material = invoke_object_getter(g_font_get_material_method, g_tmp_font_asset);
    if (material && (material == chinese_material || conversion_source_for(material)))
        material = nullptr;
    current.font_handle = font ? runtime.api.gchandle_new(font, false) : nullptr;
    current.material_handle = material ? runtime.api.gchandle_new(material, false) : nullptr;
    current.font_known = font != nullptr;
    current.material_known = material != nullptr;
    if (!current.weak_component || (font && !current.font_handle) ||
        (material && !current.material_handle)) {
        free_state(&current);
        return false;
    }
    auto previous = g_tmp_states.find(instance);
    if (previous != g_tmp_states.end()) {
        if (previous->second.holds_conversion && previous->second.conversion_source)
            release_conversion(previous->second.conversion_source);
        free_state(&previous->second);
        g_tmp_states.erase(previous);
    }
    // Recycled view objects are detected through the weak component handle;
    // dead entries are swept before the cap can silently stop new captures.
    if (g_tmp_states.size() >= 1024) {
        for (auto it = g_tmp_states.begin(); it != g_tmp_states.end();) {
            if (!runtime.api.gchandle_get_target(it->second.weak_component)) {
                if (it->second.holds_conversion && it->second.conversion_source)
                    release_conversion(it->second.conversion_source);
                free_state(&it->second);
                it = g_tmp_states.erase(it);
            } else ++it;
        }
    }
    if (g_tmp_states.size() >= 4096) {
        free_state(&current);
        return false;
    }
    g_tmp_states.emplace(instance, current);
    if (out) *out = current;
    return true;
}

bool set_tmp_font_object(void *instance, void *font) {
    return invoke_object_setter(g_tmp_set_font_method, g_original_tmp_set_font, instance, font);
}

bool set_tmp_shared_material(void *instance, void *material) {
    return invoke_object_setter(g_tmp_set_shared_material_method,
                                g_original_tmp_set_shared_material, instance, material);
}

void *conversion_source_for(void *converted) {
    RuntimeState &runtime = state();
    if (!converted || !runtime.api.gchandle_get_target) return nullptr;
    for (const auto &entry : g_material_conversions) {
        if (entry.second.converted_handle &&
            runtime.api.gchandle_get_target(entry.second.converted_handle) == converted)
            return entry.first;
    }
    return nullptr;
}

bool restore_tmp_font(void *instance) {
    RuntimeState &runtime = state();
    if (!instance || !on_unity_main_thread()) return false;
    TmpComponentState saved;
    if (!find_tmp_state(instance, &saved)) return true;
    void *current_font = invoke_object_getter(g_tmp_get_font_method, instance);
    if (current_font != g_tmp_font_asset) {
        auto stale = g_tmp_states.find(instance);
        if (stale != g_tmp_states.end()) {
            if (stale->second.holds_conversion && stale->second.conversion_source)
                release_conversion(stale->second.conversion_source);
            stale->second.holds_conversion = false;
            stale->second.converted = nullptr;
            stale->second.conversion_source = nullptr;
            stale->second.overflow_changed = false;
            stale->second.wrapping_changed = false;
        }
        return false;
    }
    void *font = saved.font_handle ? runtime.api.gchandle_get_target(saved.font_handle) : nullptr;
    void *material = saved.material_handle
                         ? runtime.api.gchandle_get_target(saved.material_handle)
                         : nullptr;
    if (saved.font_known && font) set_tmp_font_object(instance, font);
    if (saved.material_known && material) set_tmp_shared_material(instance, material);
    auto found = g_tmp_states.find(instance);
    if (found != g_tmp_states.end()) {
        int32_t current_overflow = 0;
        if (found->second.has_overflow && found->second.overflow_changed &&
            g_tmp_get_overflow_method && g_tmp_set_overflow_method &&
            invoke_scalar_getter(g_tmp_get_overflow_method, instance, &current_overflow,
                                 sizeof(current_overflow)) &&
            current_overflow == found->second.applied_overflow) {
            invoke_scalar_setter(g_tmp_set_overflow_method, instance, &found->second.overflow,
                                 sizeof(found->second.overflow));
        }
        int32_t current_wrapping = 0;
        if (found->second.has_wrapping && found->second.wrapping_changed &&
            read_tmp_wrapping(instance, &current_wrapping) &&
            current_wrapping == found->second.applied_wrapping) {
            write_tmp_wrapping(instance, found->second.wrapping);
        }
        if (found->second.holds_conversion && found->second.conversion_source)
            release_conversion(found->second.conversion_source);
        found->second.holds_conversion = false;
        found->second.converted = nullptr;
        found->second.conversion_source = nullptr;
        found->second.applied = false;
        found->second.overflow_changed = false;
        found->second.wrapping_changed = false;
    }
    return true;
}

bool apply_tmp_font(void *instance) {
    if (!instance || !state().enabled.load() || !on_unity_main_thread() || !g_tmp_font_asset ||
        !g_tmp_set_font_method) return false;
    TmpComponentState saved;
    if (!find_tmp_state(instance, &saved) && !capture_tmp_state(instance, &saved)) return false;
    void *current_font = invoke_object_getter(g_tmp_get_font_method, instance);
    if (current_font != g_tmp_font_asset && !set_tmp_font_object(instance, g_tmp_font_asset))
        return false;
    void *current_material = invoke_object_getter(g_tmp_get_shared_material_method, instance);
    void *font_material = invoke_object_getter(g_font_get_material_method, g_tmp_font_asset);
    void *assigned = current_material;
    // A previous conversion is still installed. Keep its lease instead of
    // treating it as the game's source style and converting it recursively.
    void *conversion_source = conversion_source_for(current_material);
    auto found = g_tmp_states.find(instance);
    if (conversion_source) {
        const bool held = found != g_tmp_states.end() &&
                          found->second.holds_conversion &&
                          found->second.conversion_source == conversion_source &&
                          found->second.converted == current_material;
        if (!held) {
            void *again = nullptr;
            void *fallback_material = nullptr;
            const bool acquired = convert_material(conversion_source, &again, &fallback_material);
            if (!acquired || again != current_material) {
                if (acquired) release_conversion(conversion_source);
                conversion_source = nullptr;
            }
        }
    }
    if (!conversion_source && font_material) {
        if (!current_material) {
            assigned = font_material;
            set_tmp_shared_material(instance, assigned);
        } else if (current_material != font_material) {
            void *source_atlas = material_texture(current_material);
            void *target_atlas = material_texture(font_material);
            if (!source_atlas || !target_atlas || source_atlas != target_atlas) {
                void *converted = nullptr;
                void *fallback_material = nullptr;
                if (convert_material(current_material, &converted, &fallback_material) &&
                    converted && converted != current_material) {
                    assigned = converted;
                    set_tmp_shared_material(instance, assigned);
                    bind_material_ratios(current_material, assigned,
                                         saved.font_handle ? state().api.gchandle_get_target(saved.font_handle) : nullptr,
                                         g_tmp_font_asset);
                    conversion_source = current_material;
                } else if (fallback_material && fallback_material != current_material) {
                    // Conversion failed: bind the font's own material so glyphs
                    // sample the right atlas, but never write shared ratios into
                    // it and never claim a lease we do not hold.
                    assigned = fallback_material;
                    set_tmp_shared_material(instance, assigned);
                }
            }
        }
    }
    if (found != g_tmp_states.end()) {
        if (found->second.holds_conversion &&
            found->second.conversion_source != conversion_source)
            release_conversion(found->second.conversion_source);
        found->second.holds_conversion = conversion_source != nullptr;
        found->second.converted = found->second.holds_conversion ? assigned : nullptr;
        found->second.conversion_source = conversion_source;
        found->second.applied = true;
    }
    return true;
}

void refresh_tmp_material(void *instance) {
    if (!instance || !on_unity_main_thread() || !g_tmp_font_asset) return;
    TmpComponentState saved;
    if (!find_tmp_state(instance, &saved)) return;
    if (invoke_object_getter(g_tmp_get_font_method, instance) == g_tmp_font_asset)
        apply_tmp_font(instance);
}

bool text_needs_fallback(const std::string &text) {
    std::u16string units;
    if (!shared::utf8_to_utf16(text, &units)) return false;
    bool han = false;
    for (size_t index = 0; index < units.size(); ++index) {
        uint32_t point = units[index];
        if (point >= 0xd800 && point <= 0xdbff && index + 1 < units.size()) {
            const uint32_t low = units[index + 1];
            if (low >= 0xdc00 && low <= 0xdfff) {
                point = 0x10000 + ((point - 0xd800) << 10) + (low - 0xdc00);
                ++index;
            }
        }
        if ((point >= 0x3040 && point <= 0x30ff) || (point >= 0x31f0 && point <= 0x31ff))
            return false;
        if ((point >= 0x3400 && point <= 0x9fff) ||
            (point >= 0xf900 && point <= 0xfaff) ||
            (point >= 0x20000 && point <= 0x3ffff)) han = true;
    }
    return han;
}

bool managed_text_needs_fallback(void *managed) {
    std::string text;
    return managed && managed_string_to_utf8(state().api, managed, &text) &&
           text_needs_fallback(text);
}

bool translated_non_ascii(void *managed) {
    if (!managed || !was_translated_text(managed)) return false;
    std::string text;
    if (!managed_string_to_utf8(state().api, managed, &text)) return false;
    for (unsigned char byte : text) {
        if (byte >= 0x80) return true;
    }
    return false;
}

void note_translated_flow(void *instance, const std::string &text) {
    if (!on_unity_main_thread()) return;
    TmpComponentState saved;
    if (!find_tmp_state(instance, &saved) && !capture_tmp_state(instance, &saved)) return;
    auto found = g_tmp_states.find(instance);
    if (found != g_tmp_states.end()) found->second.flow.assign(text, true, false);
}

bool matches_translated_flow(void *instance, const std::string &text) {
    if (!on_unity_main_thread()) return false;
    auto found = g_tmp_states.find(instance);
    return found != g_tmp_states.end() && found->second.flow.matches(text);
}

void *translate_if_enabled(void *managed) {
    return state().enabled.load()
               ? translate_managed_string(managed, nullptr, nullptr, nullptr)
               : managed;
}

void capture_tmp_layout(void *instance) {
    TmpComponentState saved;
    if (!on_unity_main_thread() || !find_tmp_state(instance, &saved)) return;
    if (saved.layout_saved) return;
    auto found = g_tmp_states.find(instance);
    if (found == g_tmp_states.end()) return;
    TmpComponentState &state_entry = found->second;
    state_entry.has_overflow = g_tmp_get_overflow_method &&
        invoke_scalar_getter(g_tmp_get_overflow_method, instance, &state_entry.overflow,
                             sizeof(state_entry.overflow));
    state_entry.has_wrapping = read_tmp_wrapping(instance, &state_entry.wrapping);
    state_entry.layout_saved = true;
}

void apply_tmp_layout(void *instance, void *managed_text, bool translated) {
    if (!instance || !managed_text || !translated || !on_unity_main_thread()) return;
    std::string text;
    if (!managed_string_to_utf8(state().api, managed_text, &text)) return;
    auto found = g_tmp_states.find(instance);
    if (found == g_tmp_states.end() || !found->second.layout_saved) return;
    const size_t visible = limbus_rich_text::visible_codepoints(text);
    const bool explicit_break = limbus_text_provenance::explicit_line_break(text);
    float size = 0;
    const bool has_size = g_tmp_get_font_size_method &&
        invoke_scalar_getter(g_tmp_get_font_size_method, instance, &size, sizeof(size)) &&
        std::isfinite(size) && size > 0;
    const ComponentRect rect = component_rect(instance);
    const bool body = explicit_break || visible > 48 ||
                      (has_size && rect.valid && rect.height > size * 1.8f);
    int32_t overflow = 0;
    const bool has_overflow = g_tmp_get_overflow_method &&
        invoke_scalar_getter(g_tmp_get_overflow_method, instance, &overflow, sizeof(overflow));
    if (body && !(has_overflow && overflow >= 4)) {
        // Wrapping, ScrollRect, Page and linked text own their flow. Only
        // ellipsis/truncate modes are widened so a longer Chinese body is not
        // cut to one line; the original mode is restored with the font.
        if (has_overflow && (overflow == 1 || overflow == 3) && g_tmp_set_overflow_method) {
            const int32_t none = 0;
            if (invoke_scalar_setter(g_tmp_set_overflow_method, instance, &none, sizeof(none))) {
                found->second.overflow_changed = true;
                found->second.applied_overflow = none;
            }
        }
        int32_t wrapping = 0;
        if (read_tmp_wrapping(instance, &wrapping) && (wrapping == 0 || wrapping == 3)) {
            const int32_t desired = found->second.wrapping >= 2 ? 2 : 1;
            write_tmp_wrapping(instance, desired);
            found->second.wrapping_changed = true;
            found->second.applied_wrapping = desired;
        }
    }
}

std::string lower_field(const char *name) {
    if (!name) return {};
    std::string field(name);
    for (char &c : field) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    return field;
}

void translate_object_strings(void *instance, bool story_dialog = false,
                              const char *context = nullptr);

void translate_string_list(void *list, const char *context, const char *field_name) {
    RuntimeState &runtime = state();
    const Il2CppApi &api = runtime.api;
    if (!list || !api.object_get_class || !api.class_get_method_from_name ||
        !api.runtime_invoke || !api.object_unbox) return;
    void *klass = api.object_get_class(list);
    const void *count_method = klass ? api.class_get_method_from_name(klass, "get_Count", 0) : nullptr;
    const void *get_method = klass ? api.class_get_method_from_name(klass, "get_Item", 1) : nullptr;
    const void *set_method = klass ? api.class_get_method_from_name(klass, "set_Item", 2) : nullptr;
    if (!count_method || !get_method || !set_method) return;
    void *exception = nullptr;
    void *boxed_count = api.runtime_invoke(count_method, list, nullptr, &exception);
    const void *count_value = !exception && boxed_count ? api.object_unbox(boxed_count) : nullptr;
    if (!count_value) return;
    int32_t count = 0;
    ::memcpy(&count, count_value, sizeof(count));
    if (count <= 0 || count > 64) return;
    for (int32_t index = 0; index < count; ++index) {
        void *args[1] = {&index};
        exception = nullptr;
        void *value = api.runtime_invoke(get_method, list, args, &exception);
        if (exception || !value) continue;
        void *replacement = translate_managed_string(value, nullptr, context, field_name);
        if (replacement == value) continue;
        void *set_args[2] = {&index, replacement};
        exception = nullptr;
        api.runtime_invoke(set_method, list, set_args, &exception);
    }
}

void translate_object_list(void *list) {
    RuntimeState &runtime = state();
    const Il2CppApi &api = runtime.api;
    if (!list || !api.object_get_class || !api.class_get_method_from_name ||
        !api.runtime_invoke || !api.object_unbox) return;
    void *klass = api.object_get_class(list);
    const void *count_method = klass ? api.class_get_method_from_name(klass, "get_Count", 0) : nullptr;
    const void *get_method = klass ? api.class_get_method_from_name(klass, "get_Item", 1) : nullptr;
    if (!count_method || !get_method) return;
    void *exception = nullptr;
    void *boxed_count = api.runtime_invoke(count_method, list, nullptr, &exception);
    const void *count_value = !exception && boxed_count ? api.object_unbox(boxed_count) : nullptr;
    if (!count_value) return;
    int32_t count = 0;
    ::memcpy(&count, count_value, sizeof(count));
    if (count <= 0 || count > 128) return;
    for (int32_t index = 0; index < count; ++index) {
        void *args[1] = {&index};
        exception = nullptr;
        void *item = api.runtime_invoke(get_method, list, args, &exception);
        if (!exception && item) translate_object_strings(item);
    }
}

std::string read_object_id(void *instance, const char *field_name) {
    RuntimeState &runtime = state();
    const Il2CppApi &api = runtime.api;
    if (!instance || !field_name || !api.object_get_class || !api.class_get_parent ||
        !api.class_get_field_from_name || !api.field_get_value || !api.field_get_type ||
        !api.type_get_name || !api.free_memory) return {};
    void *klass = api.object_get_class(instance);
    for (int depth = 0; klass && depth < 8; ++depth, klass = api.class_get_parent(klass)) {
        const void *field = api.class_get_field_from_name(klass, field_name);
        if (!field) continue;
        const void *type = api.field_get_type(field);
        char *type_name = type ? api.type_get_name(type) : nullptr;
        if (!type_name) continue;
        const std::string name(type_name);
        api.free_memory(type_name);
        if (name == "System.Int32") {
            int32_t id = 0;
            api.field_get_value(instance, field, &id);
            return std::to_string(id);
        }
        if (name == "System.String") {
            void *value = nullptr;
            api.field_get_value(instance, field, &value);
            std::string text;
            if (value && managed_string_to_utf8(api, value, &text)) return text;
        }
        return {};
    }
    return {};
}

void translate_object_strings(void *instance, bool story_dialog,
                              const char *context) {
    RuntimeState &runtime = state();
    const Il2CppApi &api = runtime.api;
    if (!instance || !api.object_get_class || !api.class_get_fields || !api.field_get_type ||
        !api.field_get_value || !api.field_set_value || !api.type_get_name || !api.free_memory ||
        !api.class_get_parent) {
        return;
    }
    void *klass = api.object_get_class(instance);
    int visited = 0;
    for (int depth = 0; klass && depth < 8; ++depth, klass = api.class_get_parent(klass)) {
        void *iterator = nullptr;
        int fields = 0;
        while (const void *field = api.class_get_fields(klass, &iterator)) {
            if (++fields > 512 || ++visited > 2048) break;
            if (api.field_get_flags && (api.field_get_flags(field) & 0x0010u) != 0) continue;
            const char *raw_name = api.field_get_name ? api.field_get_name(field) : nullptr;
            if (!raw_name) continue;
            const std::string exact_name(raw_name);
            const std::string lower_name = lower_field(raw_name);
            const bool story_field = story_dialog &&
                (lower_name == "content" || lower_name == "title" ||
                 lower_name == "teller" || lower_name == "place");
            const bool choice_log = !story_dialog && lower_name == "_resultlog";
            const bool display = story_field || choice_log ||
                                 translation::is_display_field(exact_name) ||
                                 translation::is_display_field(lower_name);
            if (!display) continue;
            // text_policy keeps camelCase keys exactly as the JSON pairer emits
            // them (nameWithTitle, successDesc, goalDescription1...), while the
            // game's PascalCase fields are matched through the lowercase form.
            const char *field_name = translation::is_display_field(exact_name)
                                         ? raw_name
                                         : lower_name.c_str();
            const void *type = api.field_get_type(field);
            char *type_name = type ? api.type_get_name(type) : nullptr;
            if (!type_name) continue;
            const bool is_string = ::strcmp(type_name, "System.String") == 0;
            const bool is_string_array = ::strcmp(type_name, "System.String[]") == 0;
            const bool is_string_list =
                ::strstr(type_name, "System.Collections.Generic.List") != nullptr &&
                ::strstr(type_name, "System.String") != nullptr;
            api.free_memory(type_name);
            if (is_string_list && translation::is_string_array_field(field_name)) {
                void *list = nullptr;
                api.field_get_value(instance, field, &list);
                translate_string_list(list, context, field_name);
                continue;
            }
            if (is_string_array && api.array_length && api.array_get && api.array_set) {
                void *array = nullptr;
                api.field_get_value(instance, field, &array);
                const uint32_t count = array ? api.array_length(array) : 0;
                if (array && count > 0 && count <= 2048) {
                    for (uint32_t index = 0; index < count; ++index) {
                        void *value = api.array_get(array, index);
                        if (!value) continue;
                        void *replacement = translate_managed_string(
                            value, nullptr, context, field_name);
                        if (replacement != value) api.array_set(array, index, replacement);
                    }
                }
                continue;
            }
            if (!is_string) continue;
            void *value = nullptr;
            api.field_get_value(instance, field, &value);
            if (!value) continue;
            void *replacement = translate_managed_string(
                value, nullptr, context, field_name);
            if (replacement != value) api.field_set_value(instance, field, &replacement);
        }
    }
}

void replacement_story_dialog_ctor(void *instance, int32_t arg0, int32_t arg1, void *script,
                                   void *effects, float time, const void *method) {
    if (g_original_story_dialog_ctor) {
        g_original_story_dialog_ctor(instance, arg0, arg1, script, effects, time, method);
    }
    if (!state().enabled.load()) return;
    std::string context = g_story_scene_key;
    if (!context.empty()) {
        std::string id = read_object_id(instance, "OriginalId");
        if (id.empty()) id = std::to_string(arg0 >= 0 ? arg0 : arg1);
        const std::string scoped = limbus_text_provenance::story_dialog_key(context, id);
        if (!scoped.empty()) context = scoped;
    }
    translate_object_strings(instance, true, context.empty() ? nullptr : context.c_str());
}

void *replacement_story_scenario(void *instance, void *scenario_id, int32_t language,
                                 const void *method) {
    std::string identity;
    if (scenario_id) managed_string_to_utf8(state().api, scenario_id, &identity);
    const std::string scene = limbus_text_provenance::story_scene_key(identity);
    limbus_text_provenance::ScopedTextKey scope(g_story_scene_key, scene);
    return g_original_story_scenario
               ? g_original_story_scenario(instance, scenario_id, language, method)
               : nullptr;
}

void *replacement_rpg_dialogue_text(void *instance, int32_t index, const void *method) {
    const bool enabled = state().enabled.load();
    std::string context;
    if (enabled && instance && index >= 0) {
        const std::string root =
            limbus_text_provenance::rpg_dialogue_root_key(read_object_id(instance, "key"));
        context = limbus_text_provenance::rpg_dialogue_text_key(root, std::to_string(index));
    }
    if (enabled) translate_object_strings(instance, false, context.empty() ? nullptr : context.c_str());
    void *result = g_original_rpg_dialogue_text
                       ? g_original_rpg_dialogue_text(instance, index, method)
                       : nullptr;
    if (enabled)
        translate_object_strings(result, false, context.empty() ? nullptr : context.c_str());
    return result;
}

void *replacement_rpg_format(void *format, void *arguments, const void *method) {
    void *prepared = translate_if_enabled(format);
    return g_original_rpg_format ? g_original_rpg_format(prepared, arguments, method) : prepared;
}

void replacement_choice_set_text(void *instance, void *managed_text, const void *method) {
    RuntimeState &runtime = state();
    if (instance && managed_text && runtime.enabled.load()) {
        managed_text = translate_managed_string(managed_text, nullptr, nullptr, nullptr);
    }
    if (g_original_choice_set_text) g_original_choice_set_text(instance, managed_text, method);
}

void replacement_choice_total(void *instance, void *list, const void *method) {
    if (state().enabled.load()) translate_object_list(list);
    if (g_original_choice_total) g_original_choice_total(instance, list, method);
}

void replacement_choice_read_content(void *instance, void *managed_text, bool instant,
                                     const void *method) {
    if (state().enabled.load() && managed_text) {
        managed_text = translate_managed_string(managed_text, nullptr, nullptr, nullptr);
    }
    if (g_original_choice_read_content)
        g_original_choice_read_content(instance, managed_text, instant, method);
}

void clear_choice_proof(void *instance) {
    if (!on_unity_main_thread()) return;
    auto found = g_tmp_states.find(instance);
    if (found != g_tmp_states.end()) found->second.flow.clear();
}

void replacement_choice_clear(void *instance, const void *method) {
    clear_choice_proof(instance);
    if (g_original_choice_clear) g_original_choice_clear(instance, method);
}

void replacement_choice_disable(void *instance, const void *method) {
    clear_choice_proof(instance);
    if (g_original_choice_disable) g_original_choice_disable(instance, method);
}

void replacement_notice_set_data(void *instance, void *managed_text, const void *method) {
    RuntimeState &runtime = state();
    if (instance && managed_text && runtime.enabled.load()) {
        managed_text = translate_managed_string(managed_text, nullptr, nullptr, nullptr);
    }
    if (g_original_notice_set_data) g_original_notice_set_data(instance, managed_text, method);
}

void replacement_choice_button(void *instance, int32_t selection, void *message, void *result,
                               const void *method) {
    RuntimeState &runtime = state();
    if (runtime.enabled.load()) {
        if (message) message = translate_managed_string(message, nullptr, nullptr, nullptr);
        if (result) result = translate_managed_string(result, nullptr, nullptr, nullptr);
    }
    if (g_original_choice_button) {
        g_original_choice_button(instance, selection, message, result, method);
    }
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
        void *exception = nullptr;
        void *id_box = runtime.api.runtime_invoke(g_get_instance_id, instance, nullptr, &exception);
        const void *id_value = !exception && id_box ? runtime.api.object_unbox(id_box) : nullptr;
        const int32_t instance_id = id_value ? *static_cast<const int32_t *>(id_value) : 0;
        if (instance_id != 0) {
            const bool translated = translated_non_ascii(managed_text) ||
                                    managed_text_needs_fallback(managed_text);
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
    bool changed = false;
    bool han = false;
    std::string final_text;
    bool decoded = false;
    if (instance && managed_text && runtime.enabled.load()) {
        void *translated = translate_managed_string(managed_text, nullptr, nullptr, nullptr);
        changed = translated != managed_text;
        managed_text = translated;
        decoded = managed_string_to_utf8(runtime.api, managed_text, &final_text);
        if (decoded && changed) note_translated_flow(instance, final_text);
        const bool routed = was_translated_text(managed_text);
        const bool flow = decoded && !changed && matches_translated_flow(instance, final_text);
        bool non_ascii = false;
        for (unsigned char byte : final_text) non_ascii |= byte >= 0x80;
        han = non_ascii && (changed || routed || flow || text_needs_fallback(final_text));
    }
    if (instance && han) {
        // Capture the game's own font/material/layout before the setter can
        // rewrite them; apply_tmp_font must not record the Chinese asset as the
        // original state.
        TmpComponentState saved;
        if (find_tmp_state(instance, &saved) || capture_tmp_state(instance, &saved))
            capture_tmp_layout(instance);
    }
    {
        ActiveTmpScope scope(instance);
        if (g_original_tmp_set_text) g_original_tmp_set_text(instance, managed_text, method);
    }
    if (instance && (changed || han)) {
        apply_tmp_font(instance);
        apply_tmp_layout(instance, managed_text, changed || han);
    } else if (instance) {
        restore_tmp_font(instance);
    }
}

void replacement_tmp_set_text_string(void *instance, void *managed_text, bool sync_input,
                                     const void *method) {
    RuntimeState &runtime = state();
    bool changed = false;
    bool han = false;
    std::string final_text;
    bool decoded = false;
    if (instance && managed_text && runtime.enabled.load()) {
        void *translated = translate_managed_string(managed_text, nullptr, nullptr, nullptr);
        changed = translated != managed_text;
        managed_text = translated;
        decoded = managed_string_to_utf8(runtime.api, managed_text, &final_text);
        if (decoded && changed) note_translated_flow(instance, final_text);
        const bool routed = was_translated_text(managed_text);
        const bool flow = decoded && !changed && matches_translated_flow(instance, final_text);
        bool non_ascii = false;
        for (unsigned char byte : final_text) non_ascii |= byte >= 0x80;
        han = non_ascii && (changed || routed || flow || text_needs_fallback(final_text));
    }
    if (instance && han) {
        TmpComponentState saved;
        if (find_tmp_state(instance, &saved) || capture_tmp_state(instance, &saved))
            capture_tmp_layout(instance);
    }
    {
        ActiveTmpScope scope(instance);
        if (g_original_tmp_set_text_string) {
            g_original_tmp_set_text_string(instance, managed_text, sync_input, method);
        }
    }
    if (instance && (changed || han)) {
        apply_tmp_font(instance);
        apply_tmp_layout(instance, managed_text, changed || han);
    } else if (instance) {
        restore_tmp_font(instance);
    }
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

bool translated_tmp_material(void *instance, void *material, void **out_material) {
    if (out_material) *out_material = material;
    if (!instance || !material || !state().enabled.load() || !g_tmp_font_asset ||
        !g_tmp_get_font_method || g_inside_tmp_material || !on_unity_main_thread())
        return false;
    if (invoke_object_getter(g_tmp_get_font_method, instance) != g_tmp_font_asset) return false;
    MaterialScope scope;
    void *font_material = invoke_object_getter(g_font_get_material_method, g_tmp_font_asset);
    if (!font_material || font_material == material) return false;
    void *source_atlas = material_texture(material);
    void *target_atlas = material_texture(font_material);
    if (source_atlas && target_atlas && source_atlas == target_atlas) return false;
    auto existing = g_tmp_states.find(instance);
    if (existing != g_tmp_states.end() && existing->second.holds_conversion &&
        existing->second.conversion_source == material && existing->second.converted) {
        // The same source preset is being assigned again. Reuse the lease the
        // component already holds instead of adding another TMP reference.
        if (out_material) *out_material = existing->second.converted;
        return false;
    }
    void *converted = nullptr;
    void *fallback_material = nullptr;
    if (!convert_material(material, &converted, &fallback_material) || !converted) {
        if (out_material) *out_material = fallback_material ? fallback_material : material;
        return false;
    }
    if (out_material) *out_material = converted;
    return converted != material;
}

void replacement_tmp_set_font(void *instance, void *font, const void *method) {
    if (g_original_tmp_set_font) g_original_tmp_set_font(instance, font, method);
    if (!instance || font != g_tmp_font_asset || !g_tmp_get_text_method) return;
    void *text = invoke_object_getter(g_tmp_get_text_method, instance);
    if (translated_non_ascii(text) || managed_text_needs_fallback(text))
        apply_tmp_font(instance);
}

void replacement_tmp_set_shared_material(void *instance, void *material, const void *method) {
    void *converted = material;
    const bool leased = translated_tmp_material(instance, material, &converted);
    if (g_original_tmp_set_shared_material) {
        g_original_tmp_set_shared_material(instance, converted, method);
    }
    TmpComponentState saved;
    if (leased && find_tmp_state(instance, &saved)) {
        auto found = g_tmp_states.find(instance);
        if (found != g_tmp_states.end()) {
            if (found->second.holds_conversion &&
                found->second.conversion_source != material)
                release_conversion(found->second.conversion_source);
            found->second.holds_conversion = true;
            found->second.converted = converted;
            found->second.conversion_source = material;
        }
    }
}

void replacement_tmp_set_material(void *instance, void *material, const void *method) {
    void *converted = material;
    translated_tmp_material(instance, material, &converted);
    if (g_original_tmp_set_material) {
        g_original_tmp_set_material(instance, converted, method);
    }
}

void replacement_material_write(void *instance, const void *method) {
    if (g_original_material_writer_write)
        g_original_material_writer_write(instance, method);
    if (!instance || !state().enabled.load() || !g_material_writer_get_text_method ||
        !g_tmp_get_text_method) return;
    // get_Text intentionally returns a TMP_Text component, not an
    // Il2CppString. Decode the component's text before any string API call.
    void *component = invoke_object_getter(g_material_writer_get_text_method, instance);
    if (!component) return;
    void *text = invoke_object_getter(g_tmp_get_text_method, component);
    if (text && (translated_non_ascii(text) || managed_text_needs_fallback(text)))
        refresh_tmp_material(component);
}

int32_t replacement_add_material(void *material, void *font, void **references, void *lookup,
                                 const void *method) {
    if (!g_original_add_material) {
        log_warn("AddMaterialReference trampoline missing");
        return 0;
    }
    void *converted = material;
    if (font && font == g_tmp_font_asset && g_active_tmp_text) {
        translated_tmp_material(g_active_tmp_text, material, &converted);
    }
    return g_original_add_material(converted, font, references, lookup, method);
}

rich_text_one_fn g_original_rich_one[3] = {};
rich_text_all_fn g_original_rich_all = nullptr;
rich_text_color_fn g_original_rich_color = nullptr;
rich_text_color_fn g_original_rich_event = nullptr;
rich_text_skill_fn g_original_rich_skill = nullptr;
ruby_format_fn g_original_ruby_format[3] = {};
void *g_ruby_classes[3] = {};
thread_local bool g_inside_ruby = false;

template <size_t Slot>
void *replacement_rich_one(void *text, const void *method) {
    void *prepared = translate_if_enabled(text);
    return g_original_rich_one[Slot] ? g_original_rich_one[Slot](prepared, method) : prepared;
}

void *replacement_rich_all(void *text, bool gray, const void *method) {
    void *prepared = translate_if_enabled(text);
    return g_original_rich_all ? g_original_rich_all(prepared, gray, method) : prepared;
}

void *replacement_rich_color(void *text, void *color, const void *method) {
    void *prepared = translate_if_enabled(text);
    return g_original_rich_color ? g_original_rich_color(prepared, color, method) : prepared;
}

void *replacement_rich_event(void *text, void *arguments, const void *method) {
    void *prepared = translate_if_enabled(text);
    return g_original_rich_event ? g_original_rich_event(prepared, arguments, method) : prepared;
}

void *replacement_rich_skill(void *text, bool gray, bool highlight, const void *method) {
    void *prepared = translate_if_enabled(text);
    return g_original_rich_skill
               ? g_original_rich_skill(prepared, gray, highlight, method)
               : prepared;
}

template <size_t Slot>
void *replacement_ruby_format(void *instance, void *text, const void *method) {
    if (!g_original_ruby_format[Slot]) return text;
    if (!instance || !text || g_inside_ruby) return g_original_ruby_format[Slot](instance, text, method);
    struct Scope {
        Scope() { g_inside_ruby = true; }
        ~Scope() { g_inside_ruby = false; }
    } scope;
    void *prepared = translate_if_enabled(text);
    if (state().enabled.load() &&
        (translated_non_ascii(prepared) || managed_text_needs_fallback(prepared)))
        apply_tmp_font(instance);
    return g_original_ruby_format[Slot](instance, prepared, method);
}

// And64InlineHook rewrites 16 or 20 bytes at the target. A method body that sits
// next to another method in the same class cannot be patched without corrupting
// the neighbour, so the sorted address table decides containment. The table only
// covers the classes the text hooks touch; an address outside it is *unknown*
// space, not proven space, and hook_method stays fail-open on that verdict.
std::vector<uintptr_t> g_managed_method_addresses;

void collect_class_methods(const Il2CppApi &api, void *klass) {
    if (!klass || !api.class_get_methods) return;
    void *iterator = nullptr;
    while (const void *method = api.class_get_methods(klass, &iterator)) {
        if (!method) break;
        void *pointer = nullptr;
        ::memcpy(&pointer, method, sizeof(pointer));
        if (pointer) g_managed_method_addresses.push_back(reinterpret_cast<uintptr_t>(pointer));
    }
}

// Walks the class chain because TMP and the game's UI types inherit the hooked
// virtuals from up to eight ancestors (TextMeshProUGUI -> TMP_Text -> ...).
void collect_class_tree(const Il2CppApi &api, void *klass) {
    for (int depth = 0; klass && depth < 8; ++depth) {
        collect_class_methods(api, klass);
        if (!api.class_get_parent) return;
        klass = api.class_get_parent(klass);
    }
}

bool has_inline_hook_space(const void *method) {
    if (!method) return false;
    void *pointer = nullptr;
    ::memcpy(&pointer, method, sizeof(pointer));
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    if (address == 0) return false;
    const auto position = std::lower_bound(g_managed_method_addresses.begin(),
                                           g_managed_method_addresses.end(), address);
    if (position == g_managed_method_addresses.end() || *position != address) return false;
    if (position + 1 == g_managed_method_addresses.end()) return false;
    return *(position + 1) - address >= ((address & 7u) ? 20u : 16u);
}

bool hook_method(RuntimeState &runtime, const void *method, void *replacement, void **original) {
    if (!method || !replacement || !original) return false;
    void *target = nullptr;
    ::memcpy(&target, method, sizeof(target));
    if (!target) return false;
    if (!has_inline_hook_space(method)) {
        // Deliberate deviation from the reference's skip: the address table is
        // not device-verified yet, so an unknown verdict must not switch every
        // hook off at once.
        log_warn("hook space unknown");
    }
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

const void *find_verified_inherited(const Il2CppApi &api, void *klass,
                                    const MethodSpec &spec) {
    for (int depth = 0; klass && depth < 8; ++depth) {
        if (const void *method = find_verified_method(api, klass, spec)) return method;
        if (!api.class_get_parent) break;
        klass = api.class_get_parent(klass);
    }
    return nullptr;
}

bool install_text_hooks() {
    RuntimeState &runtime = state();
    g_text_entry_installed = false;
    if (!runtime.api.ready()) return false;
    // Collect the method addresses of every class the hooks below touch before
    // the first patch, so hook_method can tell a safe slot from an adjacent
    // method body. A class that does not exist yet contributes nothing.
    {
        struct ClassName {
            const char *name_space;
            const char *name;
        };
        const ClassName hooked_classes[] = {
            {"TextAssetPatch", "TextAssetPatchManager"},
            {"UnityEngine", "TextAsset"},
            {"UnityEngine.UI", "Text"},
            {"TMPro", "TMP_Text"},
            {"TMPro", "TMP_FontAsset"},
            {"UnityEngine", "Font"},
            {"UnityEngine", "Material"},
            {"TMPro", "TMP_MaterialManager"},
            {"UI.Utility", "TMProStringMatcher"},
            {"", "TextMeshProMaterialSetter"},
            {"TMPro", "MaterialReference"},
            {"TMPro", "CustomRubyTextMeshProUGUI"},
            {"TMPro", "RubyTextMeshProUGUI"},
            {"TMPro", "RubyTextMeshPro"},
            {"BattleUI.Announcer", "BattleAnnouncerUI"},
            {"BattleUI", "Announcer"},
            {"StorySystem", "Dialog"},
            {"StorySystem", "StoryData"},
            {"RPGSystem", "RpgLocText_Dialogue"},
            {"RPGSystem", "RpgLocFormat"},
            {"", "ChoiceScrollViewBox"},
            {"MainUI.NoticeUI", "NoticeUIContentText"},
            {"", "ActionChoiceButton"},
        };
        for (const ClassName &entry : hooked_classes) {
            collect_class_tree(runtime.api,
                               find_class(runtime.api, entry.name_space, entry.name));
        }
        std::sort(g_managed_method_addresses.begin(), g_managed_method_addresses.end());
        g_managed_method_addresses.erase(
            std::unique(g_managed_method_addresses.begin(), g_managed_method_addresses.end()),
            g_managed_method_addresses.end());
        log_info("method addresses=%zu", g_managed_method_addresses.size());
    }
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
        // Entry hook: only an entry failure rolls back; optional hooks below are
        // merely logged and never undo the rest of the installation.
        runtime.ledger.restore_module(reinterpret_cast<uintptr_t>(runtime.api.base));
        return false;
    }
    if (!hook_method(runtime, text_method, reinterpret_cast<void *>(replacement_text_asset_text),
                     reinterpret_cast<void **>(&g_original_text_asset_text))) {
        // Entry hook: same rule as above.
        runtime.ledger.restore_module(reinterpret_cast<uintptr_t>(runtime.api.base));
        g_original_load_localize = nullptr;
        return false;
    }
    if (ui_method && g_ui_get_font && g_ui_set_font && g_get_instance_id) {
        if (!hook_method(runtime, ui_method, reinterpret_cast<void *>(replacement_ui_set_text),
                         reinterpret_cast<void **>(&g_original_ui_set_text))) {
            // UGUI is optional: a rejected Ui.Text font swap only costs the font
            // transfer, so it is logged and the installation continues.
            log_warn("ui font hook rejected");
        }
    } else {
        log_warn("ui font hook unavailable");
    }
    void *tmp_text = find_class(runtime.api, "TMPro", "TMP_Text");
    const MethodSpec tmp_spec{"set_text", "System.String", "System.Void", false};
    const void *tmp_method = tmp_text ? find_verified_method(runtime.api, tmp_text, tmp_spec) : nullptr;
    const MethodSpec tmp_font_spec{"set_font", "TMPro.TMP_FontAsset", "System.Void", false};
    g_tmp_set_font_method =
        tmp_text ? find_verified_method(runtime.api, tmp_text, tmp_font_spec) : nullptr;
    const MethodSpec tmp_get_font_spec{"get_font", "", "TMPro.TMP_FontAsset", false};
    g_tmp_get_font_method =
        tmp_text ? find_verified_method(runtime.api, tmp_text, tmp_get_font_spec) : nullptr;
    const MethodSpec tmp_get_material_spec{"get_sharedMaterial", "", "UnityEngine.Material", false};
    g_tmp_get_shared_material_method =
        tmp_text ? find_verified_method(runtime.api, tmp_text, tmp_get_material_spec) : nullptr;
    const MethodSpec tmp_set_material_spec{"set_sharedMaterial", "UnityEngine.Material",
                                           "System.Void", false};
    g_tmp_set_shared_material_method =
        tmp_text ? find_verified_method(runtime.api, tmp_text, tmp_set_material_spec) : nullptr;
    const MethodSpec tmp_set_font_material_spec{"set_material", "UnityEngine.Material",
                                                "System.Void", false};
    g_tmp_set_material_method =
        tmp_text ? find_verified_method(runtime.api, tmp_text, tmp_set_font_material_spec) : nullptr;
    const MethodSpec tmp_get_text_spec{"get_text", "", "System.String", false};
    g_tmp_get_text_method =
        tmp_text ? find_verified_method(runtime.api, tmp_text, tmp_get_text_spec) : nullptr;
    if (tmp_text) {
        g_tmp_get_font_size_method = find_method(runtime.api, tmp_text, "get_fontSize",
                                                 "System.Single", 0);
        g_tmp_get_overflow_method = find_method(runtime.api, tmp_text, "get_overflowMode", nullptr, 0);
        g_tmp_set_overflow_method = find_method(runtime.api, tmp_text, "set_overflowMode",
                                                "System.Void", 1);
        g_tmp_get_wrapping_method = find_method(runtime.api, tmp_text, "get_textWrappingMode",
                                                nullptr, 0);
        g_tmp_set_wrapping_method = find_method(runtime.api, tmp_text, "set_textWrappingMode",
                                                "System.Void", 1);
        if (!g_tmp_get_wrapping_method || !g_tmp_set_wrapping_method) {
            g_tmp_get_word_wrapping_method = find_method(runtime.api, tmp_text,
                                                         "get_enableWordWrapping",
                                                         "System.Boolean", 0);
            g_tmp_set_word_wrapping_method = find_method(runtime.api, tmp_text,
                                                         "set_enableWordWrapping",
                                                         "System.Void", 1);
        }
        g_tmp_get_rect_transform_method = find_method(runtime.api, tmp_text, "get_rectTransform",
                                                      "UnityEngine.RectTransform", 0);
    }
    void *rect_transform_class = find_class(runtime.api, "UnityEngine", "RectTransform");
    g_rect_get_rect_method =
        rect_transform_class ? find_method(runtime.api, rect_transform_class, "get_rect",
                                           "UnityEngine.Rect", 0)
                             : nullptr;
    if (!runtime.api.array_get || !runtime.api.array_set) {
        log_warn("il2cpp array accessors unavailable; string[] fields stay untranslated");
    }
    void *tmp_font_class = find_class(runtime.api, "TMPro", "TMP_FontAsset");
    const MethodSpec font_get_material_spec{"get_material", "", "UnityEngine.Material", false};
    g_font_get_material_method = tmp_font_class
                                     ? find_verified_method(runtime.api, tmp_font_class,
                                                            font_get_material_spec)
                                     : nullptr;
    const MethodSpec font_face_spec{"get_faceInfo", "", "UnityEngine.TextCore.FaceInfo", false};
    g_font_get_face_info_method = tmp_font_class
                                      ? find_verified_method(runtime.api, tmp_font_class,
                                                             font_face_spec)
                                      : nullptr;
    if (g_font_get_face_info_method && runtime.api.method_get_return_type &&
        runtime.api.class_from_type) {
        const void *face_type = runtime.api.method_get_return_type(g_font_get_face_info_method);
        void *face_class = face_type ? runtime.api.class_from_type(face_type) : nullptr;
        const MethodSpec point_spec{"get_pointSize", "", "System.Single", false};
        g_face_get_point_size_method =
            face_class ? find_verified_method(runtime.api, face_class, point_spec) : nullptr;
    }
    void *material_class = find_class(runtime.api, "UnityEngine", "Material");
    const MethodSpec material_get_float_spec{"GetFloat", "System.String", "System.Single", false};
    g_material_get_float_method =
        material_class ? find_verified_method(runtime.api, material_class, material_get_float_spec)
                       : nullptr;
    const MethodSpec material_set_float_spec{"SetFloat", "System.String,System.Single",
                                             "System.Void", false};
    g_material_set_float_method =
        material_class ? find_verified_method(runtime.api, material_class, material_set_float_spec)
                       : nullptr;
    const MethodSpec material_texture_spec{"GetTexture", "System.String", "UnityEngine.Texture",
                                           false};
    g_material_get_texture_method =
        material_class ? find_verified_method(runtime.api, material_class, material_texture_spec)
                       : nullptr;
    void *material_manager = find_class(runtime.api, "TMPro", "TMP_MaterialManager");
    const MethodSpec fallback_spec{"GetFallbackMaterial", "UnityEngine.Material,UnityEngine.Material",
                                   "UnityEngine.Material", true};
    g_material_manager_get_fallback =
        material_manager ? find_verified_method(runtime.api, material_manager, fallback_spec)
                         : nullptr;
    const MethodSpec add_reference_spec{"AddFallbackMaterialReference", "UnityEngine.Material",
                                        "System.Void", true};
    g_material_manager_add_reference =
        material_manager ? find_verified_method(runtime.api, material_manager, add_reference_spec)
                         : nullptr;
    const MethodSpec release_reference_spec{"ReleaseFallbackMaterial", "UnityEngine.Material",
                                            "System.Void", true};
    g_material_manager_release =
        material_manager ? find_verified_method(runtime.api, material_manager, release_reference_spec)
                         : nullptr;
    if (tmp_method) {
        if (!hook_method(runtime, tmp_method, reinterpret_cast<void *>(replacement_tmp_set_text),
                         reinterpret_cast<void **>(&g_original_tmp_set_text))) {
            log_warn("tmp set_text hook rejected");
        }
    } else {
        log_warn("tmp text hook unavailable");
    }
    const MethodSpec tmp_string_spec{"set_text_string", "System.String,System.Boolean",
                                     "System.Void", false};
    const void *tmp_string_method =
        tmp_text ? find_verified_method(runtime.api, tmp_text, tmp_string_spec) : nullptr;
    if (tmp_string_method) {
        if (!hook_method(runtime, tmp_string_method,
                         reinterpret_cast<void *>(replacement_tmp_set_text_string),
                         reinterpret_cast<void **>(&g_original_tmp_set_text_string))) {
            log_warn("tmp set_text_string hook rejected");
        }
    }
    if (g_tmp_set_font_method) {
        if (!hook_method(runtime, g_tmp_set_font_method,
                         reinterpret_cast<void *>(replacement_tmp_set_font),
                         reinterpret_cast<void **>(&g_original_tmp_set_font))) {
            log_warn("tmp set_font hook rejected");
        }
    }
    if (g_tmp_set_shared_material_method) {
        if (!hook_method(runtime, g_tmp_set_shared_material_method,
                         reinterpret_cast<void *>(replacement_tmp_set_shared_material),
                         reinterpret_cast<void **>(&g_original_tmp_set_shared_material))) {
            log_warn("tmp set_sharedMaterial hook rejected");
        }
    }
    if (g_tmp_set_material_method) {
        if (!hook_method(runtime, g_tmp_set_material_method,
                         reinterpret_cast<void *>(replacement_tmp_set_material),
                         reinterpret_cast<void **>(&g_original_tmp_set_material))) {
            log_warn("tmp set_material hook rejected");
        }
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
            log_warn("announcer hook rejected");
        }
    } else {
        log_warn("announcer hook unavailable");
    }
    void *story_dialog = find_class(runtime.api, "StorySystem", "Dialog");
    const MethodSpec story_spec{
        ".ctor", "System.Int32,System.Int32,SimpleJSON.JSONNode,SimpleJSON.JSONNode,System.Single",
        "System.Void", false};
    const void *story_method =
        story_dialog ? find_verified_method(runtime.api, story_dialog, story_spec) : nullptr;
    if (story_method) {
        if (!hook_method(runtime, story_method,
                         reinterpret_cast<void *>(replacement_story_dialog_ctor),
                         reinterpret_cast<void **>(&g_original_story_dialog_ctor))) {
            log_warn("story dialog hook rejected");
        }
    } else {
        log_warn("story dialog hook unavailable");
    }
    void *story_data = find_class(runtime.api, "StorySystem", "StoryData");
    const void *scenario_method = story_data
        ? find_method(runtime.api, story_data, "GetScenario", "StorySystem.Scenario", 2)
        : nullptr;
    if (scenario_method && first_param_is(runtime.api, scenario_method, "System.String") &&
        limbus_method::is_instance_method(scenario_method, runtime.api.method_get_flags)) {
        if (!hook_method(runtime, scenario_method,
                         reinterpret_cast<void *>(replacement_story_scenario),
                         reinterpret_cast<void **>(&g_original_story_scenario))) {
            log_warn("story scenario hook rejected");
        }
    }
    void *rpg_dialogue = find_class(runtime.api, "RPGSystem", "RpgLocText_Dialogue");
    const MethodSpec rpg_spec{"GetTextData", "System.Int32", "RPGSystem.RpgLocDialogueText",
                              false};
    const void *rpg_method =
        rpg_dialogue ? find_verified_method(runtime.api, rpg_dialogue, rpg_spec) : nullptr;
    if (rpg_method) {
        if (!hook_method(runtime, rpg_method,
                         reinterpret_cast<void *>(replacement_rpg_dialogue_text),
                         reinterpret_cast<void **>(&g_original_rpg_dialogue_text))) {
            log_warn("rpg dialogue hook rejected");
        }
    } else {
        log_warn("rpg dialogue hook unavailable");
    }
    void *rpg_format = find_class(runtime.api, "RPGSystem", "RpgLocFormat");
    const MethodSpec rpg_format_spec{"Apply", "System.String,System.Object[]", "System.String",
                                     true};
    const void *rpg_format_method =
        rpg_format ? find_verified_method(runtime.api, rpg_format, rpg_format_spec) : nullptr;
    if (rpg_format_method) {
        if (!hook_method(runtime, rpg_format_method,
                         reinterpret_cast<void *>(replacement_rpg_format),
                         reinterpret_cast<void **>(&g_original_rpg_format))) {
            log_warn("rpg format hook rejected");
        }
    }
    void *choice_scroll = find_class(runtime.api, "", "ChoiceScrollViewBox");
    const MethodSpec choice_text_spec{"SetText", "System.String", "System.Void", false};
    const void *choice_text_method =
        choice_scroll ? find_verified_method(runtime.api, choice_scroll, choice_text_spec) : nullptr;
    if (choice_text_method) {
        if (!hook_method(runtime, choice_text_method,
                         reinterpret_cast<void *>(replacement_choice_set_text),
                         reinterpret_cast<void **>(&g_original_choice_set_text))) {
            log_warn("choice text hook rejected");
        }
    }
    const void *choice_total = choice_scroll
        ? find_method(runtime.api, choice_scroll, "SetTotalContent", "System.Void", 1)
        : nullptr;
    if (choice_total && first_param_contains(runtime.api, choice_total, "ChoiceEventResultLog") &&
        limbus_method::is_instance_method(choice_total, runtime.api.method_get_flags)) {
        if (!hook_method(runtime, choice_total,
                         reinterpret_cast<void *>(replacement_choice_total),
                         reinterpret_cast<void **>(&g_original_choice_total))) {
            log_warn("choice total-content hook rejected");
        }
    }
    if (choice_scroll) {
        const void *read_content = find_method(runtime.api, choice_scroll, "ReadingContent",
                                               "System.Void", 2);
        if (read_content && first_param_is(runtime.api, read_content, "System.String") &&
            limbus_method::is_instance_method(read_content, runtime.api.method_get_flags)) {
            if (!hook_method(runtime, read_content,
                             reinterpret_cast<void *>(replacement_choice_read_content),
                             reinterpret_cast<void **>(&g_original_choice_read_content))) {
                log_warn("choice ReadingContent hook rejected");
            }
        }
        const void *clear_method = find_method(runtime.api, choice_scroll, "Clear",
                                               "System.Void", 0);
        if (clear_method &&
            limbus_method::is_instance_method(clear_method, runtime.api.method_get_flags)) {
            if (!hook_method(runtime, clear_method, reinterpret_cast<void *>(replacement_choice_clear),
                             reinterpret_cast<void **>(&g_original_choice_clear))) {
                log_warn("choice Clear hook rejected");
            }
        }
        const MethodSpec disable_spec{"OnDisable", "", "System.Void", false};
        const void *disable_method =
            find_verified_inherited(runtime.api, choice_scroll, disable_spec);
        if (disable_method &&
            limbus_method::is_instance_method(disable_method, runtime.api.method_get_flags)) {
            if (!hook_method(runtime, disable_method,
                             reinterpret_cast<void *>(replacement_choice_disable),
                             reinterpret_cast<void **>(&g_original_choice_disable))) {
                log_warn("choice OnDisable hook rejected");
            }
        }
    }
    void *notice_content = find_class(runtime.api, "MainUI.NoticeUI", "NoticeUIContentText");
    const MethodSpec notice_spec{"SetData", "System.String", "System.Void", false};
    const void *notice_method =
        notice_content ? find_verified_method(runtime.api, notice_content, notice_spec) : nullptr;
    if (notice_method) {
        if (!hook_method(runtime, notice_method,
                         reinterpret_cast<void *>(replacement_notice_set_data),
                         reinterpret_cast<void **>(&g_original_notice_set_data))) {
            log_warn("notice data hook rejected");
        }
    }
    void *choice_button = find_class(runtime.api, "", "ActionChoiceButton");
    const MethodSpec button_spec{"SetButtonData", "System.Int32,System.String,System.String",
                                 "System.Void", false};
    const void *button_method =
        choice_button ? find_verified_method(runtime.api, choice_button, button_spec) : nullptr;
    if (button_method) {
        if (!hook_method(runtime, button_method,
                         reinterpret_cast<void *>(replacement_choice_button),
                         reinterpret_cast<void **>(&g_original_choice_button))) {
            log_warn("choice button hook rejected");
        }
    }
    void *rich_matcher = find_class(runtime.api, "UI.Utility", "TMProStringMatcher");
    auto hook_optional = [&](void *klass, const MethodSpec &spec, void *replacement,
                             void **original, bool required) {
        (void)required;
        const void *method = klass ? find_verified_inherited(runtime.api, klass, spec) : nullptr;
        if (!method) {
            log_warn("optional method unavailable: %s", spec.name ? spec.name : "?");
            return false;
        }
        if (!hook_method(runtime, method, replacement, original)) {
            log_warn("optional hook rejected: %s", spec.name ? spec.name : "?");
            return false;
        }
        return true;
    };
    if (rich_matcher) {
        hook_optional(rich_matcher,
                      MethodSpec{"GetAppliedAllRichText", "System.String,System.Boolean", "System.String", true},
                      reinterpret_cast<void *>(replacement_rich_all),
                      reinterpret_cast<void **>(&g_original_rich_all), false);
        hook_optional(rich_matcher,
                      MethodSpec{"GetHighlightedEgoGiftString", "System.String", "System.String", true},
                      reinterpret_cast<void *>(replacement_rich_one<0>),
                      reinterpret_cast<void **>(&g_original_rich_one[0]), false);
        hook_optional(rich_matcher,
                      MethodSpec{"GetApplySpriteAndLinkAndColorString", "System.String", "System.String", true},
                      reinterpret_cast<void *>(replacement_rich_one<1>),
                      reinterpret_cast<void **>(&g_original_rich_one[1]), false);
        hook_optional(rich_matcher,
                      MethodSpec{"GetColorMappedLinkedString", "System.String,System.String", "System.String", true},
                      reinterpret_cast<void *>(replacement_rich_color),
                      reinterpret_cast<void **>(&g_original_rich_color), false);
        hook_optional(rich_matcher,
                      MethodSpec{"GetTagMatchedAbChoiceLogString", "System.String,ArgumentForChoice[]", "System.String", true},
                      reinterpret_cast<void *>(replacement_rich_event),
                      reinterpret_cast<void **>(&g_original_rich_event), false);
        hook_optional(rich_matcher,
                      MethodSpec{"GetBuffMatchedString", "System.String", "System.String", true},
                      reinterpret_cast<void *>(replacement_rich_one<2>),
                      reinterpret_cast<void **>(&g_original_rich_one[2]), false);
        hook_optional(rich_matcher,
                      MethodSpec{"GetSkillTagMatchedString", "System.String,System.Boolean,System.Boolean", "System.String", true},
                      reinterpret_cast<void *>(replacement_rich_skill),
                      reinterpret_cast<void **>(&g_original_rich_skill), false);
    }
    void *material_writer = find_class(runtime.api, "", "TextMeshProMaterialSetter");
    const MethodSpec writer_get_text{"get_Text", "", "TMPro.TMP_Text", false};
    g_material_writer_get_text_method =
        material_writer ? find_verified_method(runtime.api, material_writer, writer_get_text)
                        : nullptr;
    g_material_writer_write_method = material_writer
                                         ? find_verified_method(runtime.api, material_writer,
                                                                MethodSpec{"WriteMaterialProperty", "",
                                                                           "System.Void", false})
                                         : nullptr;
    if (g_material_writer_get_text_method && g_material_writer_write_method) {
        hook_optional(material_writer,
                      MethodSpec{"WriteMaterialProperty", "", "System.Void", false},
                      reinterpret_cast<void *>(replacement_material_write),
                      reinterpret_cast<void **>(&g_original_material_writer_write), false);
    }
    void *material_reference = find_class(runtime.api, "TMPro", "MaterialReference");
    if (material_reference) {
        // Measured signature: the third parameter is a by-reference array, and
        // normalize() already accepts both generic spellings, so one spec covers
        // Dictionary`2<...> and Dictionary<...>.
        const MethodSpec add_spec{
            "AddMaterialReference",
            "UnityEngine.Material,TMPro.TMP_FontAsset,TMPro.MaterialReference[]&,"
            "System.Collections.Generic.Dictionary<System.Int32,System.Int32>",
            "System.Int32", true};
        if (!hook_optional(material_reference, add_spec,
                           reinterpret_cast<void *>(replacement_add_material),
                           reinterpret_cast<void **>(&g_original_add_material), false)) {
            log_warn("material reference hook unavailable");
        }
    }
    const char *ruby_classes[] = {"CustomRubyTextMeshProUGUI", "RubyTextMeshProUGUI",
                                  "RubyTextMeshPro"};
    // Three ruby base types can resolve to the same ReplaceRubyTags body; a
    // second hook_method on that address fails on the overlapping ledger range
    // and would leave the slot permanently empty, so share the trampoline.
    std::unordered_map<void *, void *> installed_ruby;
    for (size_t slot = 0; slot < 3; ++slot) {
        void *klass = find_class(runtime.api, "TMPro", ruby_classes[slot]);
        if (!klass) continue;
        const MethodSpec ruby_spec{"ReplaceRubyTags", "System.String", "System.String", false};
        const void *method = find_verified_method(runtime.api, klass, ruby_spec);
        if (!method) continue;
        void *pointer = nullptr;
        ::memcpy(&pointer, method, sizeof(pointer));
        if (!pointer) continue;
        const auto alias = installed_ruby.find(pointer);
        if (alias != installed_ruby.end()) {
            g_original_ruby_format[slot] = reinterpret_cast<ruby_format_fn>(alias->second);
            g_ruby_classes[slot] = klass;
            continue;
        }
        void *replacement = slot == 0 ? reinterpret_cast<void *>(replacement_ruby_format<0>)
                                      : slot == 1 ? reinterpret_cast<void *>(replacement_ruby_format<1>)
                                                  : reinterpret_cast<void *>(replacement_ruby_format<2>);
        if (hook_method(runtime, method, replacement,
                        reinterpret_cast<void **>(&g_original_ruby_format[slot]))) {
            g_ruby_classes[slot] = klass;
            installed_ruby.emplace(pointer,
                                   reinterpret_cast<void *>(g_original_ruby_format[slot]));
        }
    }
    g_text_asset_class = text_asset;
    // The reference records an audit and keeps running: a mismatch here is
    // reported, never repaired, and only the three entry hooks decide the result.
    uint32_t text_crc = 0;
    uint32_t managed_crc = 0;
    size_t owned = 0;
    const bool audited = verify_owned_sections(reinterpret_cast<uintptr_t>(runtime.api.base),
                                               runtime.profile, &runtime.ledger, &text_crc,
                                               &managed_crc, &owned);
    log_info("hook audit owned=%zu/%zu text=%08x/%08x managed=%08x/%08x valid=%d", owned,
             runtime.ledger.substitute_count(reinterpret_cast<uintptr_t>(runtime.api.base)),
             text_crc, runtime.profile.text_crc, managed_crc, runtime.profile.managed_crc,
             audited ? 1 : 0);
    g_text_entry_installed = g_original_load_localize != nullptr &&
                             g_original_text_asset_text != nullptr &&
                             g_original_tmp_set_text != nullptr;
    log_info("text hooks installed=%d", g_text_entry_installed ? 1 : 0);
    return g_text_entry_installed;
}

bool text_entry_installed() { return g_text_entry_installed; }

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
    // Three attempts are the whole budget for a generation; the reference counts
    // them per real TMP call, not per callback.
    if (g_font_attempts >= 3) return false;
    void *exception = nullptr;
    pthread_mutex_lock(&g_font_mutex);
    if (g_font_handle != nullptr) {
        pthread_mutex_unlock(&g_font_mutex);
        return true;
    }
    if (g_font_attempts >= 3) {
        pthread_mutex_unlock(&g_font_mutex);
        return false;
    }
    // One prepare_font call spends at most one attempt, and only when it reached
    // a real TMP creation or registration invoke.
    struct FontAttemptScope {
        bool spent = false;
        ~FontAttemptScope() {
            if (spent) ++g_font_attempts;
        }
    } attempt;
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
        pthread_mutex_unlock(&g_font_mutex);
        return false;
    }
    void *const name_handle = api.gchandle_new(name, false);
    if (name_handle == nullptr) {
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
    attempt.spent = true;
    void *asset = api.runtime_invoke(create, nullptr, arguments, &exception);
    api.gchandle_free(name_handle);
    if (exception || !asset) {
        pthread_mutex_unlock(&g_font_mutex);
        log_warn("TMP font creation failed");
        return false;
    }
    void *const handle = api.gchandle_new(asset, false);
    if (handle == nullptr) {
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
        pthread_mutex_unlock(&g_font_mutex);
        log_warn("legacy Chinese font validation failed");
        return false;
    }
    void *add_arguments[1] = {asset};
    exception = nullptr;
    attempt.spent = true;
    api.runtime_invoke(add, fallbacks, add_arguments, &exception);
    if (exception) {
        api.gchandle_free(legacy_handle);
        api.gchandle_free(handle);
        pthread_mutex_unlock(&g_font_mutex);
        log_warn("TMP fallback registration failed");
        return false;
    }
    g_font_handle = handle;
    g_legacy_font = legacy;
    g_legacy_font_handle = legacy_handle;
    g_tmp_font_asset = asset;
    g_unity_main_thread = runtime.main_thread;
    pthread_mutex_unlock(&g_font_mutex);
    log_info("TMP Chinese font registered");
    return true;
}

} // namespace bybus::runtime

