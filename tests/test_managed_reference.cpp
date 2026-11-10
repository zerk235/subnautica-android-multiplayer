#include "modloader/src/managed_reference.h"
#include <cassert>

struct Il2CppObject { Il2CppObject* reference = nullptr; };
static Il2CppObject* expected;
static int writes;
static void write_reference(Il2CppObject* owner, void*, Il2CppObject* value) {
    assert(value == expected); // The object itself, never &a_stack_pointer.
    owner->reference = value; ++writes;
}
static void read_reference(Il2CppObject* owner, void*, void* out) {
    *static_cast<Il2CppObject**>(out) = owner->reference;
}
static void reject_write(Il2CppObject*, void*, Il2CppObject*) {}
void (*il2cpp_field_set_value_object)(Il2CppObject*, void*, Il2CppObject*) = write_reference;
void (*il2cpp_field_get_value)(Il2CppObject*, void*, void*) = read_reference;
int main() {
    Il2CppObject owner, target; int field;
    expected = &target;
    assert(managed_reference_set_checked(&owner, &field, &target));
    assert(owner.reference == &target && writes == 1);
    assert(!managed_reference_set_checked(nullptr, &field, &target));
    assert(!managed_reference_set_checked(&owner, nullptr, &target));
    assert(writes == 1);
    expected = nullptr;
    assert(managed_reference_set_checked(&owner, &field, nullptr));
    il2cpp_field_set_value_object = reject_write;
    assert(!managed_reference_set_checked(&owner, &field, &target));
    il2cpp_field_set_value_object = nullptr;
    assert(!managed_reference_set_checked(&owner, &field, &target));
    il2cpp_field_set_value_object = write_reference;
    il2cpp_field_get_value = nullptr;
    assert(!managed_reference_set_checked(&owner, &field, &target));
}
