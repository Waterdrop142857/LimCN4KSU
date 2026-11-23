// IL2CPP discovery for the official game process. Only exported il2cpp_* APIs
// are used; no ELF section walking and no assumptions about internal layouts.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace bybus::runtime {

struct Il2CppApi {
    void *handle = nullptr;
    void *base = nullptr;

    void *(*domain_get)() = nullptr;
    const void **(*domain_get_assemblies)(const void *, size_t *) = nullptr;
    const void *(*assembly_get_image)(const void *) = nullptr;
    void *(*class_from_name)(const void *, const char *, const char *) = nullptr;
    const void *(*class_get_method_from_name)(void *, const char *, int) = nullptr;
    const void *(*class_get_methods)(void *, void **) = nullptr;
    const void *(*class_get_fields)(void *, void **) = nullptr;
    const void *(*class_get_field_from_name)(void *, const char *) = nullptr;
    void *(*class_get_parent)(void *) = nullptr;
    const char *(*class_get_name)(void *) = nullptr;
    const char *(*class_get_namespace)(void *) = nullptr;
    bool (*class_is_enum)(void *) = nullptr;
    void *(*class_from_type)(const void *) = nullptr;
    void *(*class_get_type)(void *) = nullptr;
    void *(*type_get_object)(const void *) = nullptr;
    const char *(*field_get_name)(const void *) = nullptr;
    const void *(*field_get_type)(const void *) = nullptr;
    uint32_t (*field_get_flags)(const void *) = nullptr;
    void (*field_get_value)(void *, const void *, void *) = nullptr;
    void (*field_set_value)(void *, const void *, void *) = nullptr;
    void (*field_static_get_value)(const void *, void *) = nullptr;
    const char *(*method_get_name)(const void *) = nullptr;
    uint32_t (*method_get_param_count)(const void *) = nullptr;
    const void *(*method_get_param)(const void *, uint32_t) = nullptr;
    const void *(*method_get_return_type)(const void *) = nullptr;
    const void *(*method_get_class)(const void *) = nullptr;
    bool (*method_is_generic)(const void *) = nullptr;
    uint32_t (*method_get_flags)(const void *, uint32_t *) = nullptr;
    char *(*type_get_name)(const void *) = nullptr;
    void (*free_memory)(void *) = nullptr;
    int32_t (*string_length)(void *) = nullptr;
    const uint16_t *(*string_chars)(void *) = nullptr;
    void *(*string_new_utf16)(const uint16_t *, int32_t) = nullptr;
    void *(*thread_attach)(void *) = nullptr;
    void (*thread_detach)(void *) = nullptr;
    void *(*runtime_invoke)(const void *, void *, void **, void **) = nullptr;
    void *(*object_unbox)(void *) = nullptr;
    void *(*object_get_class)(void *) = nullptr;
    void *(*object_new)(void *) = nullptr;
    // Unity 6 exposes Il2CppGCHandle as an opaque pointer; truncating it to a
    // table index silently loses retained component identity on arm64.
    void *(*gchandle_new)(void *, bool) = nullptr;
    void *(*gchandle_new_weakref)(void *, bool) = nullptr;
    void (*gchandle_free)(void *) = nullptr;
    void *(*gchandle_get_target)(void *) = nullptr;

    bool ready() const { return handle != nullptr && base != nullptr; }
};

// Resolves the exports from the already loaded libil2cpp.so (RTLD_NOLOAD).
// Returns false while Unity has not loaded the library yet or exports are
// missing; callers retry with a bounded deadline.
bool resolve_il2cpp(Il2CppApi *api);

// Attaches the calling thread to the IL2CPP domain (required before any API call
// from a non-Unity thread) and detaches again on scope exit.
class Il2CppThreadScope {
public:
    explicit Il2CppThreadScope(const Il2CppApi &api);
    ~Il2CppThreadScope();
    Il2CppThreadScope(const Il2CppThreadScope &) = delete;
    Il2CppThreadScope &operator=(const Il2CppThreadScope &) = delete;

private:
    const Il2CppApi &api_;
    void *domain_ = nullptr;
    bool attached_ = false;
};

void *find_class(const Il2CppApi &api, const char *namespace_name, const char *name);

// Matches name, declared return type and parameter count; returns nullptr when
// the shape differs, so an unverified signature is never hooked.
const void *find_method(const Il2CppApi &api, void *klass, const char *name,
                        const char *return_type, uint32_t param_count);

// Strictly verified overload: declaring class, parameter types, return type,
// generic status and static/instance flags must all match.
struct MethodSpec {
    const char *name = nullptr;
    const char *parameters = "";  // comma-separated IL2CPP type names
    const char *returns = nullptr;
    bool is_static = false;
};
const void *find_verified_method(const Il2CppApi &api, void *klass, const MethodSpec &spec);

bool managed_string_to_utf8(const Il2CppApi &api, void *managed, std::string *out);
void *managed_string_from_utf8(const Il2CppApi &api, const std::string &utf8);

} // namespace bybus::runtime
