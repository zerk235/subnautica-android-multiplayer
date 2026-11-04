#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>

namespace base_geometry {
// This game's runtime differs from the generated il2cpp.h: method_count is
// +0x120 and interfaces_count +0x12C (verified in exported runtime accessors).
// vtable_count is +0x12A; +0x126 is event_count, often 1, not the table length.
inline void* completion_vtable_entry(void* klass, void* method, uint16_t* slot_out, uint16_t* count_out,
                                     uint16_t expected_slot = 13) {
    if (!klass || !method) return nullptr;
    auto* info = static_cast<uint8_t*>(method);
    auto* cls = static_cast<uint8_t*>(klass);
    uint16_t slot=0, count=0;
    std::memcpy(&slot, info + 0x50, sizeof(slot));
    std::memcpy(&count, cls + 0x12A, sizeof(count));
    *slot_out=slot; *count_out=count;
    if (slot != expected_slot || slot >= count) return nullptr;
    auto* entry = cls + 0x138 + size_t(slot) * 16;
    void *function=nullptr, *entry_method=nullptr, *direct=nullptr, *virt=nullptr;
    std::memcpy(&function, entry, sizeof(function));
    std::memcpy(&entry_method, entry+8, sizeof(entry_method));
    std::memcpy(&direct, info, sizeof(direct));
    std::memcpy(&virt, info+8, sizeof(virt));
    if (entry_method != method || !function || (function != direct && function != virt)) return nullptr;
    return entry;
}

// ProgressDeconstruction is invoked after the game subtracts construction progress.
// Only the explicit final step can announce removal; unloaded objects never qualify.
template<class Capture, class Transition, class Publish, class Error>
auto observe_deconstruction(float amount, bool constructed, bool remote, bool ready, bool preview,
                            Capture capture, Transition transition, Publish publish, Error error) {
    bool captured = false;
    if (std::isfinite(amount) && amount == 0.0f && !constructed && !remote && ready && !preview) {
        try { captured = capture(); }
        catch (...) { error("capture"); }
    }
    auto result = transition();
    if (captured && result) {
        try { publish(); }
        catch (...) { error("publish"); }
    }
    return result;
}

// Capture before SetState destroys the temporary construction object; publish only
// after the game confirms the transition. Preview/remote/load events cannot send.
template<class Capture, class Transition, class Publish, class Error>
uint8_t observe_completion(bool requested_complete, bool was_complete, bool preview,
                           bool remote, bool world_ready,
                           Capture capture, Transition transition, Publish publish, Error error) {
    bool captured = false;
    if (requested_complete && !was_complete && !preview && !remote && world_ready) {
        try { captured = capture(); }
        catch (...) { error("capture"); }
    }
    const uint8_t changed = transition();
    if (captured && changed) {
        try { publish(); }
        catch (...) { error("publish"); }
    }
    return changed;
}

// Builder.GetGhostModel can be a child of the Constructable, not its parent.
template<class Parent>
bool preview_related(void* object, void* preview, Parent parent) {
    if (!object || !preview) return false;
    auto contains = [&](void* node, void* ancestor) {
        for (unsigned depth = 0; node && depth < 256; ++depth, node = parent(node))
            if (node == ancestor) return true;
        return false;
    };
    return contains(object, preview) || contains(preview, object);
}
}
