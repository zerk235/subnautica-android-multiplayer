#pragma once
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/mman.h>
#include <unistd.h>

struct MappedPatchResult {
    bool written = false;
    int error = 0;
    int restore_error = 0;
};

// Method tables live in writable runtime allocations. Never turn their pages
// into RX: unrelated class initialization / allocator metadata shares them.
inline MappedPatchResult mapped_patch_write(void* address, const void* data, size_t size) {
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (!address || !data || !size || size > std::numeric_limits<std::uintptr_t>::max() - begin)
        return {false, EINVAL, 0};
    const auto end = begin + size;
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) return {false, errno, 0};
    int protection = -1;
    char line[1024];
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long low = 0, high = 0;
        char mode[5]{};
        if (std::sscanf(line, "%lx-%lx %4s", &low, &high, mode) != 3) continue;
        if (begin >= low && end <= high) {
            protection = (mode[0] == 'r' ? PROT_READ : 0) |
                         (mode[1] == 'w' ? PROT_WRITE : 0) |
                         (mode[2] == 'x' ? PROT_EXEC : 0);
            break;
        }
    }
    std::fclose(maps);
    // Refuse an unmapped range, unreadable memory or a range spanning mappings.
    // All native pointer patches fit inside one mapping.
    if (protection < 0 || !(protection & PROT_READ)) return {false, EFAULT, 0};
    if (protection & PROT_WRITE) {
        std::memcpy(address, data, size);
        __builtin___clear_cache(static_cast<char*>(address), static_cast<char*>(address) + size);
        return {true, 0, 0};
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return {false, EINVAL, 0};
    const auto page = static_cast<std::uintptr_t>(page_size);
    const auto first = begin - begin % page;
    const auto last = (end - 1) - (end - 1) % page;
    const size_t span = last - first + page;
    // Avoid requesting RWX. Restore the actual original permissions afterwards.
    if (mprotect(reinterpret_cast<void*>(first), span, (protection | PROT_WRITE) & ~PROT_EXEC))
        return {false, errno, 0};
    std::memcpy(address, data, size);
    __builtin___clear_cache(static_cast<char*>(address), static_cast<char*>(address) + size);
    if (mprotect(reinterpret_cast<void*>(first), span, protection))
        return {true, 0, errno};
    return {true, 0, 0};
}
