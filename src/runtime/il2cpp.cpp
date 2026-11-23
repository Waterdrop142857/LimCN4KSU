#include "runtime/il2cpp.h"

#include <dlfcn.h>
#include <string.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/log.h"
#include "shared/utf.h"

namespace bybus::runtime {
namespace {

constexpr int32_t kMaxManagedString = INT32_MAX;

bool is_il2cpp_owner(void *symbol, void *expected_base) {
    Dl_info info{};
    if (!symbol || ::dladdr(symbol, &info) == 0) return false;
    if (!info.dli_fname || !info.dli_fbase) return false;
    const char *name = ::strrchr(info.dli_fname, '/');
    name = name ? name + 1 : info.dli_fname;
    if (::strcmp(name, "libil2cpp.so") != 0) return false;
    return expected_base == nullptr || info.dli_fbase == expected_base;
}

template <typename T>
bool bind(void *handle, const char *name, T *slot) {
    void *symbol = ::dlsym(handle, name);
    if (!symbol) return false;
    if (!is_il2cpp_owner(symbol, nullptr)) return false;
    *slot = reinterpret_cast<T>(symbol);
    return true;
}

template <typename T>
void bind_optional(void *handle, const char *name, T *slot) {
    void *symbol = ::dlsym(handle, name);
    if (!symbol || !is_il2cpp_owner(symbol, nullptr)) return;
    *slot = reinterpret_cast<T>(symbol);
}

std::string type_name_of(const Il2CppApi &api, const void *type) {
    if (!type || !api.type_get_name) return std::string();
    char *owned = api.type_get_name(type);
    if (!owned) return std::string();
    std::string name(owned);
    if (api.free_memory) api.free_memory(owned);
    return name;
}

} // namespace

bool resolve_il2cpp(Il2CppApi *api) {
    if (!api) return false;
    if (api->ready()) return true;
    void *handle = ::dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    if (!handle) return false;
    Il2CppApi resolved;
    resolved.handle = handle;
    bool ok = bind(handle, "il2cpp_domain_get", &resolved.domain_get) &&
              bind(handle, "il2cpp_domain_get_assemblies", &resolved.domain_get_assemblies) &&
              bind(handle, "il2cpp_assembly_get_image", &resolved.assembly_get_image) &&
              bind(handle, "il2cpp_class_from_name", &resolved.class_from_name) &&
              bind(handle, "il2cpp_class_get_method_from_name",
                   &resolved.class_get_method_from_name) &&
              bind(handle, "il2cpp_class_get_methods", &resolved.class_get_methods) &&
              bind(handle, "il2cpp_method_get_name", &resolved.method_get_name) &&
              bind(handle, "il2cpp_method_get_param_count", &resolved.method_get_param_count) &&
              bind(handle, "il2cpp_method_get_param", &resolved.method_get_param) &&
              bind(handle, "il2cpp_method_get_return_type", &resolved.method_get_return_type) &&
              bind(handle, "il2cpp_type_get_name", &resolved.type_get_name) &&
              bind(handle, "il2cpp_free", &resolved.free_memory) &&
              bind(handle, "il2cpp_string_length", &resolved.string_length) &&
              bind(handle, "il2cpp_string_chars", &resolved.string_chars) &&
              bind(handle, "il2cpp_string_new_utf16", &resolved.string_new_utf16) &&
              bind(handle, "il2cpp_thread_attach", &resolved.thread_attach) &&
              bind(handle, "il2cpp_thread_detach", &resolved.thread_detach) &&
              bind(handle, "il2cpp_runtime_invoke", &resolved.runtime_invoke) &&
              bind(handle, "il2cpp_object_unbox", &resolved.object_unbox) &&
              bind(handle, "il2cpp_object_get_class", &resolved.object_get_class) &&
              bind(handle, "il2cpp_object_new", &resolved.object_new) &&
              bind(handle, "il2cpp_gchandle_new", &resolved.gchandle_new) &&
              bind(handle, "il2cpp_gchandle_new_weakref", &resolved.gchandle_new_weakref) &&
              bind(handle, "il2cpp_gchandle_free", &resolved.gchandle_free) &&
              bind(handle, "il2cpp_gchandle_get_target", &resolved.gchandle_get_target) &&
              bind(handle, "il2cpp_class_get_field_from_name",
                   &resolved.class_get_field_from_name) &&
              bind(handle, "il2cpp_field_get_value", &resolved.field_get_value) &&
              bind(handle, "il2cpp_class_get_parent", &resolved.class_get_parent);
    if (ok) {
        bind_optional(handle, "il2cpp_class_get_fields", &resolved.class_get_fields);
        bind_optional(handle, "il2cpp_class_get_name", &resolved.class_get_name);
        bind_optional(handle, "il2cpp_class_get_namespace", &resolved.class_get_namespace);
        bind_optional(handle, "il2cpp_class_is_enum", &resolved.class_is_enum);
        bind_optional(handle, "il2cpp_class_from_type", &resolved.class_from_type);
        bind_optional(handle, "il2cpp_class_get_type", &resolved.class_get_type);
        bind_optional(handle, "il2cpp_type_get_object", &resolved.type_get_object);
        bind_optional(handle, "il2cpp_field_get_name", &resolved.field_get_name);
        bind_optional(handle, "il2cpp_field_get_type", &resolved.field_get_type);
        bind_optional(handle, "il2cpp_field_get_flags", &resolved.field_get_flags);
        bind_optional(handle, "il2cpp_field_set_value", &resolved.field_set_value);
        bind_optional(handle, "il2cpp_field_static_get_value",
                      &resolved.field_static_get_value);
        bind_optional(handle, "il2cpp_method_get_class", &resolved.method_get_class);
        bind_optional(handle, "il2cpp_method_is_generic", &resolved.method_is_generic);
        bind_optional(handle, "il2cpp_method_get_flags", &resolved.method_get_flags);
    }
    if (!ok) {
        ::dlclose(handle);
        return false;
    }
    Dl_info info{};
    if (::dladdr(reinterpret_cast<void *>(resolved.domain_get), &info) == 0) {
        ::dlclose(handle);
        return false;
    }
    resolved.base = info.dli_fbase;
    *api = resolved;
    return true;
}

Il2CppThreadScope::Il2CppThreadScope(const Il2CppApi &api) : api_(api) {
    if (!api_.domain_get || !api_.thread_attach) return;
    domain_ = api_.domain_get();
    if (!domain_) return;
    api_.thread_attach(domain_);
    attached_ = true;
}

Il2CppThreadScope::~Il2CppThreadScope() {
    if (attached_ && api_.thread_detach && domain_) api_.thread_detach(domain_);
}

void *find_class(const Il2CppApi &api, const char *namespace_name, const char *name) {
    if (!api.ready() || !namespace_name || !name) return nullptr;
    const void *domain = api.domain_get();
    if (!domain) return nullptr;
    size_t assembly_count = 0;
    const void **assemblies = api.domain_get_assemblies(domain, &assembly_count);
    if (!assemblies) return nullptr;
    if (assembly_count > 512) assembly_count = 512;
    for (size_t i = 0; i < assembly_count; ++i) {
        const void *image = api.assembly_get_image(assemblies[i]);
        if (!image) continue;
        void *klass = api.class_from_name(image, namespace_name, name);
        if (klass) return klass;
    }
    return nullptr;
}

const void *find_method(const Il2CppApi &api, void *klass, const char *name,
                        const char *return_type, uint32_t param_count) {
    if (!api.ready() || !klass || !name) return nullptr;
    void *iterator = nullptr;
    while (const void *method = api.class_get_methods(klass, &iterator)) {
        const char *method_name = api.method_get_name(method);
        if (!method_name || ::strcmp(method_name, name) != 0) continue;
        if (api.method_get_param_count(method) != param_count) continue;
        if (return_type != nullptr) {
            const std::string actual = type_name_of(api, api.method_get_return_type(method));
            if (actual != return_type) continue;
        }
        return method;
    }
    return nullptr;
}

namespace {

std::vector<std::string> split_parameters(const char *text) {
    std::vector<std::string> result;
    if (!text || !*text) return result;
    std::string token;
    for (const char *cursor = text;; ++cursor) {
        if (*cursor == ',' || *cursor == '\0') {
            size_t begin = 0;
            while (begin < token.size() && token[begin] == ' ') ++begin;
            size_t end = token.size();
            while (end > begin && token[end - 1] == ' ') --end;
            result.push_back(token.substr(begin, end - begin));
            token.clear();
            if (*cursor == '\0') break;
        } else {
            token.push_back(*cursor);
        }
    }
    return result;
}

bool method_is_instance(const Il2CppApi &api, const void *method, bool *is_static) {
    if (!api.method_get_flags || !method) return false;
    uint32_t implementation = 0;
    const uint32_t flags = api.method_get_flags(method, &implementation);
    *is_static = (flags & 0x0010u) != 0;
    return true;
}

} // namespace

const void *find_verified_method(const Il2CppApi &api, void *klass, const MethodSpec &spec) {
    if (!api.ready() || !klass || !spec.name || !api.class_get_methods ||
        !api.method_get_name || !api.method_get_param_count || !api.method_get_param ||
        !api.method_get_return_type || !api.type_get_name || !api.free_memory ||
        !api.method_get_class || !api.method_is_generic || !api.method_get_flags) {
        return nullptr;
    }
    const std::vector<std::string> parameters = split_parameters(spec.parameters);
    void *iterator = nullptr;
    while (const void *method = api.class_get_methods(klass, &iterator)) {
        if (!method) break;
        if (api.method_get_name(method) == nullptr ||
            ::strcmp(api.method_get_name(method), spec.name) != 0) {
            continue;
        }
        if (api.method_get_class(method) != klass || api.method_is_generic(method)) continue;
        bool is_static = false;
        if (!method_is_instance(api, method, &is_static) || is_static != spec.is_static) continue;
        if (static_cast<size_t>(api.method_get_param_count(method)) != parameters.size()) continue;
        if (spec.returns != nullptr) {
            const std::string actual = type_name_of(api, api.method_get_return_type(method));
            if (actual != spec.returns) continue;
        }
        bool matches = true;
        for (size_t index = 0; index < parameters.size(); ++index) {
            const std::string actual =
                type_name_of(api, api.method_get_param(method, static_cast<uint32_t>(index)));
            if (actual != parameters[index]) {
                matches = false;
                break;
            }
        }
        if (matches) return method;
    }
    return nullptr;
}

bool managed_string_to_utf8(const Il2CppApi &api, void *managed, std::string *out) {
    if (!api.string_length || !api.string_chars || !managed) return false;
    const int32_t length = api.string_length(managed);
    if (length <= 0 || length > kMaxManagedString) return false;
    const uint16_t *units = api.string_chars(managed);
    if (!units) return false;
    return shared::utf16_to_utf8(units, length, out);
}

void *managed_string_from_utf8(const Il2CppApi &api, const std::string &utf8) {
    if (!api.string_new_utf16 || utf8.empty()) return nullptr;
    std::u16string units;
    if (!shared::utf8_to_utf16(utf8, &units)) return nullptr;
    if (units.size() > static_cast<size_t>(kMaxManagedString)) return nullptr;
    return api.string_new_utf16(reinterpret_cast<const uint16_t *>(units.data()),
                                static_cast<int32_t>(units.size()));
}

} // namespace bybus::runtime
