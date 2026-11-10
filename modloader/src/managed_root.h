#pragma once
#include <cstdint>

// Native globals are not managed GC roots. Retain objects through the runtime's
// handle table and resolve the current target before calling a managed method.
class ManagedRoot {
public:
    using Handle = std::uintptr_t;
    using Create = Handle (*)(void*);
    using Get = void* (*)(Handle);
    using Free = void (*)(Handle);
    ManagedRoot() = default;
    ManagedRoot(const ManagedRoot&) = delete;
    ManagedRoot& operator=(const ManagedRoot&) = delete;
    ~ManagedRoot() { clear(); }
    bool retain(void* object, Create create, Get get, Free release) {
        if (!object || !create || !get || !release) return false;
        const auto next = create(object);
        if (!next) return false; // Preserve the previous root on allocation failure.
        if (get(next) != object) { release(next); return false; }
        clear(); handle_ = next; get_ = get; free_ = release; return true;
    }
    void* get() const { return handle_ && get_ ? get_(handle_) : nullptr; }
    void clear() {
        const auto old = handle_; const auto release = free_;
        handle_ = 0; get_ = nullptr; free_ = nullptr;
        if (old && release) release(old);
    }
private:
    Handle handle_ = 0;
    Get get_ = nullptr;
    Free free_ = nullptr;
};
