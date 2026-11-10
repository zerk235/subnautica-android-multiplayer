#pragma once
#include "il2cpp_api.h"

// Unity 6's field_set_value takes a reference value directly for reference fields,
// unlike the address used for scalar fields. Use the explicit object API and check
// the stored reference before letting Unity consume it on a later frame.
inline bool managed_reference_set_checked(Il2CppObject* owner, void* field,
                                          Il2CppObject* value) {
    if (!owner || !field || !il2cpp_field_set_value_object || !il2cpp_field_get_value)
        return false;
    il2cpp_field_set_value_object(owner, field, value);
    void* actual = nullptr;
    il2cpp_field_get_value(owner, field, &actual);
    return actual == value;
}
