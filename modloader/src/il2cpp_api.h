#pragma once

#include <cstddef>
#include <cstdint>


struct Il2CppDomain;
struct Il2CppAssembly;
struct Il2CppImage;
struct Il2CppClass;
struct Il2CppMethodInfo;
struct Il2CppObject;
struct Il2CppString;
struct Il2CppThread;
struct Il2CppFieldInfo;

bool resolve_il2cpp_api();

extern struct Il2CppDomain* (*il2cpp_domain_get)();
extern struct Il2CppAssembly** (*il2cpp_domain_get_assemblies)(struct Il2CppDomain* domain, size_t* size);
extern struct Il2CppImage* (*il2cpp_assembly_get_image)(struct Il2CppAssembly* assembly);
extern const char* (*il2cpp_image_get_name)(struct Il2CppImage* image);
extern struct Il2CppClass* (*il2cpp_class_from_name)(struct Il2CppImage* image, const char* namespaze, const char* name);
extern struct Il2CppMethodInfo* (*il2cpp_class_get_method_from_name)(struct Il2CppClass* klass, const char* name, int argsCount);
extern struct Il2CppObject* (*il2cpp_runtime_invoke)(struct Il2CppMethodInfo* method, void* obj, void** params, void** exc);
extern struct Il2CppString* (*il2cpp_string_new)(const char* str);
extern struct Il2CppThread* (*il2cpp_thread_attach)(struct Il2CppDomain* domain);
extern struct Il2CppObject* (*il2cpp_object_new)(struct Il2CppClass* klass);
extern struct Il2CppArray* (*il2cpp_array_new)(struct Il2CppClass* klass, unsigned int length);
extern unsigned int (*il2cpp_gchandle_new)(struct Il2CppObject* obj, bool pinned);
extern struct Il2CppClass* (*il2cpp_object_get_class)(struct Il2CppObject* obj);
extern int (*il2cpp_field_get_offset)(void* field);
extern struct Il2CppFieldInfo* (*il2cpp_class_get_fields)(struct Il2CppClass* klass, void** iter);
extern const char* (*il2cpp_field_get_name)(void* field);
extern void (*il2cpp_field_get_value)(struct Il2CppObject* obj, void* field, void* value);
extern void (*il2cpp_field_set_value)(struct Il2CppObject* obj, void* field, void* value);
extern void (*il2cpp_field_set_value_object)(struct Il2CppObject* obj, void* field, struct Il2CppObject* value);
extern void (*il2cpp_field_static_get_value)(void* field, void* value);
extern void (*il2cpp_field_static_set_value)(void* field, void* value);
extern struct Il2CppClass* (*il2cpp_field_get_parent)(void* field);
extern struct Il2CppMethodInfo* (*il2cpp_class_get_methods)(struct Il2CppClass* klass, void** iter);
extern const char* (*il2cpp_method_get_name)(struct Il2CppMethodInfo* method);
extern struct Il2CppImage* (*il2cpp_get_corlib)();
extern const void* (*il2cpp_class_get_type)(struct Il2CppClass* klass);
extern struct Il2CppObject* (*il2cpp_type_get_object)(const void* type);
extern unsigned int (*il2cpp_method_get_param_count)(struct Il2CppMethodInfo* method);
extern const void* (*il2cpp_method_get_param)(struct Il2CppMethodInfo* method, unsigned int index);
extern const void* (*il2cpp_field_get_type)(void* field);
extern struct Il2CppClass* (*il2cpp_class_get_parent)(struct Il2CppClass* klass);
extern const char* (*il2cpp_class_get_name)(struct Il2CppClass* klass);
extern const char* (*il2cpp_class_get_namespace)(struct Il2CppClass* klass);
extern int (*il2cpp_image_get_class_count)(struct Il2CppImage* image);
extern struct Il2CppClass* (*il2cpp_image_get_class)(struct Il2CppImage* image, int index);

struct Il2CppImage* find_assembly_image(const char* name);
struct Il2CppClass* find_class(struct Il2CppImage* image, const char* namespaze, const char* name);
struct Il2CppClass* find_class_in_domain(const char* namespaze, const char* name);
size_t enumerate_image_classes(struct Il2CppImage* image,
                               void (*visit)(struct Il2CppClass*, void*), void* ctx);
