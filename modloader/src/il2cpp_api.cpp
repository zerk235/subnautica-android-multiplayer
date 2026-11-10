#include "il2cpp_api.h"

#include <dlfcn.h>
#include <cstring>

#include "log.h"

struct Il2CppDomain* (*il2cpp_domain_get)() = nullptr;
struct Il2CppAssembly** (*il2cpp_domain_get_assemblies)(struct Il2CppDomain*, size_t*) = nullptr;
struct Il2CppImage* (*il2cpp_assembly_get_image)(struct Il2CppAssembly*) = nullptr;
const char* (*il2cpp_image_get_name)(struct Il2CppImage*) = nullptr;
struct Il2CppClass* (*il2cpp_class_from_name)(struct Il2CppImage*, const char*, const char*) = nullptr;
struct Il2CppMethodInfo* (*il2cpp_class_get_method_from_name)(struct Il2CppClass*, const char*, int) = nullptr;
struct Il2CppObject* (*il2cpp_runtime_invoke)(struct Il2CppMethodInfo*, void*, void**, void**) = nullptr;
struct Il2CppString* (*il2cpp_string_new)(const char*) = nullptr;
struct Il2CppThread* (*il2cpp_thread_attach)(struct Il2CppDomain*) = nullptr;
struct Il2CppObject* (*il2cpp_object_new)(struct Il2CppClass*) = nullptr;
struct Il2CppArray* (*il2cpp_array_new)(struct Il2CppClass*, unsigned int) = nullptr;
Il2CppGCHandle (*il2cpp_gchandle_new)(struct Il2CppObject*, bool) = nullptr;
struct Il2CppObject* (*il2cpp_gchandle_get_target)(Il2CppGCHandle) = nullptr;
void (*il2cpp_gchandle_free)(Il2CppGCHandle) = nullptr;
struct Il2CppClass* (*il2cpp_object_get_class)(struct Il2CppObject*) = nullptr;
int (*il2cpp_field_get_offset)(void*) = nullptr;
struct Il2CppFieldInfo* (*il2cpp_class_get_fields)(struct Il2CppClass*, void**) = nullptr;
const char* (*il2cpp_field_get_name)(void*) = nullptr;
void (*il2cpp_field_get_value)(struct Il2CppObject*, void*, void*) = nullptr;
void (*il2cpp_field_set_value)(struct Il2CppObject*, void*, void*) = nullptr;
void (*il2cpp_field_set_value_object)(struct Il2CppObject*, void*, struct Il2CppObject*) = nullptr;
void (*il2cpp_field_static_get_value)(void*, void*) = nullptr;
void (*il2cpp_field_static_set_value)(void*, void*) = nullptr;
struct Il2CppClass* (*il2cpp_field_get_parent)(void*) = nullptr;
struct Il2CppMethodInfo* (*il2cpp_class_get_methods)(struct Il2CppClass*, void**) = nullptr;
const char* (*il2cpp_method_get_name)(struct Il2CppMethodInfo*) = nullptr;
struct Il2CppImage* (*il2cpp_get_corlib)() = nullptr;
const void* (*il2cpp_class_get_type)(struct Il2CppClass*) = nullptr;
struct Il2CppObject* (*il2cpp_type_get_object)(const void*) = nullptr;
unsigned int (*il2cpp_method_get_param_count)(struct Il2CppMethodInfo*) = nullptr;
const void* (*il2cpp_method_get_param)(struct Il2CppMethodInfo*, unsigned int) = nullptr;
const void* (*il2cpp_field_get_type)(void*) = nullptr;
struct Il2CppClass* (*il2cpp_class_get_parent)(struct Il2CppClass*) = nullptr;
const char* (*il2cpp_class_get_name)(struct Il2CppClass*) = nullptr;

const char* (*il2cpp_class_get_namespace)(struct Il2CppClass*) = nullptr;
int (*il2cpp_image_get_class_count)(struct Il2CppImage*) = nullptr;
struct Il2CppClass* (*il2cpp_image_get_class)(struct Il2CppImage*, int) = nullptr;

#define RESOLVE(var, name)                                              \
    var = reinterpret_cast<decltype(var)>(dlsym(handle, name));         \
    if (!var) {                                                         \
        LOG_ERROR("cannot resolve %s", name);                           \
        missing++;                                                      \
    }

bool resolve_il2cpp_api() {
    void* handle = dlopen("libil2cpp.so", RTLD_NOW);
    if (!handle) {
        LOG_ERROR("dlopen libil2cpp.so failed: %s", dlerror());
        return false;
    }

    int missing = 0;
    RESOLVE(il2cpp_domain_get, "il2cpp_domain_get")
    RESOLVE(il2cpp_domain_get_assemblies, "il2cpp_domain_get_assemblies")
    RESOLVE(il2cpp_assembly_get_image, "il2cpp_assembly_get_image")
    RESOLVE(il2cpp_image_get_name, "il2cpp_image_get_name")
    RESOLVE(il2cpp_class_from_name, "il2cpp_class_from_name")
    RESOLVE(il2cpp_class_get_method_from_name, "il2cpp_class_get_method_from_name")
    RESOLVE(il2cpp_runtime_invoke, "il2cpp_runtime_invoke")
    RESOLVE(il2cpp_string_new, "il2cpp_string_new")
    RESOLVE(il2cpp_thread_attach, "il2cpp_thread_attach")
    RESOLVE(il2cpp_object_new, "il2cpp_object_new")
    RESOLVE(il2cpp_array_new, "il2cpp_array_new")
    RESOLVE(il2cpp_gchandle_new, "il2cpp_gchandle_new")
    RESOLVE(il2cpp_gchandle_get_target, "il2cpp_gchandle_get_target")
    RESOLVE(il2cpp_gchandle_free, "il2cpp_gchandle_free")
    RESOLVE(il2cpp_object_get_class, "il2cpp_object_get_class")
    RESOLVE(il2cpp_field_get_offset, "il2cpp_field_get_offset")
    RESOLVE(il2cpp_class_get_fields, "il2cpp_class_get_fields")
    RESOLVE(il2cpp_field_get_name, "il2cpp_field_get_name")
    RESOLVE(il2cpp_field_get_value, "il2cpp_field_get_value")
    RESOLVE(il2cpp_field_set_value, "il2cpp_field_set_value")
    RESOLVE(il2cpp_field_set_value_object, "il2cpp_field_set_value_object")
    RESOLVE(il2cpp_field_static_get_value, "il2cpp_field_static_get_value")
    RESOLVE(il2cpp_field_static_set_value, "il2cpp_field_static_set_value")
    RESOLVE(il2cpp_field_get_parent, "il2cpp_field_get_parent")
    RESOLVE(il2cpp_class_get_methods, "il2cpp_class_get_methods")
    RESOLVE(il2cpp_method_get_name, "il2cpp_method_get_name")
    RESOLVE(il2cpp_get_corlib, "il2cpp_get_corlib")
    RESOLVE(il2cpp_class_get_type, "il2cpp_class_get_type")
    RESOLVE(il2cpp_type_get_object, "il2cpp_type_get_object")
    RESOLVE(il2cpp_method_get_param_count, "il2cpp_method_get_param_count")
    RESOLVE(il2cpp_method_get_param, "il2cpp_method_get_param")
    RESOLVE(il2cpp_field_get_type, "il2cpp_field_get_type")
    RESOLVE(il2cpp_class_get_parent, "il2cpp_class_get_parent")
    RESOLVE(il2cpp_class_get_name, "il2cpp_class_get_name")
    RESOLVE(il2cpp_class_get_namespace, "il2cpp_class_get_namespace")
    RESOLVE(il2cpp_image_get_class_count, "il2cpp_image_get_class_count")
    RESOLVE(il2cpp_image_get_class, "il2cpp_image_get_class")

    LOG_INFO("il2cpp api resolved, missing symbols: %d", missing);
    return missing == 0;
}

struct Il2CppImage* find_assembly_image(const char* name) {
    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies) return nullptr;

    size_t count = 0;
    struct Il2CppAssembly** assemblies = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &count);
    if (!assemblies) return nullptr;

    for (size_t i = 0; i < count; i++) {
        struct Il2CppImage* image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        const char* image_name = il2cpp_image_get_name(image);
        if (image_name && strcmp(image_name, name) == 0) return image;
    }
    return nullptr;
}

struct Il2CppClass* find_class(struct Il2CppImage* image, const char* namespaze, const char* name) {
    if (!image || !il2cpp_class_from_name) return nullptr;
    return il2cpp_class_from_name(image, namespaze, name);
}

struct Il2CppClass* find_class_in_domain(const char* namespaze, const char* name) {
    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies) return nullptr;

    size_t count = 0;
    struct Il2CppAssembly** assemblies = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &count);
    if (!assemblies) return nullptr;

    for (size_t i = 0; i < count; i++) {
        struct Il2CppImage* image = il2cpp_assembly_get_image(assemblies[i]);
        struct Il2CppClass* klass = find_class(image, namespaze, name);
        if (klass) return klass;
    }
    return nullptr;
}

// il2cpp_class_from_name only sees types the runtime already materialised, so any class
// that is merely present in metadata still counts as missing. Walking the image index
// forces metadata registration and finds the whole catalogue.
size_t enumerate_image_classes(struct Il2CppImage* image,
                               void (*visit)(struct Il2CppClass*, void*), void* ctx) {
    if (!image || !il2cpp_image_get_class_count || !il2cpp_image_get_class) return 0;

    int total = il2cpp_image_get_class_count(image);
    size_t seen = 0;
    for (int i = 0; i < total; i++) {
        struct Il2CppClass* klass = il2cpp_image_get_class(image, i);
        if (!klass) continue;
        visit(klass, ctx);
        seen++;
    }
    return seen;
}
