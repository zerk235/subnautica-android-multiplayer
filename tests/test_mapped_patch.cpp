#include "modloader/src/mapped_patch.h"
#include <cassert>
#include <atomic>
#include <new>

int permissions(void* address) {
    FILE* maps = fopen("/proc/self/maps", "r");
    assert(maps);
    char line[1024];
    const auto pointer = reinterpret_cast<std::uintptr_t>(address);
    int result = -1;
    while (fgets(line, sizeof(line), maps)) {
        unsigned long low, high;
        char mode[5]{};
        if (sscanf(line, "%lx-%lx %4s", &low, &high, mode) == 3 && pointer >= low && pointer < high) {
            result = (mode[0] == 'r' ? PROT_READ : 0) |
                     (mode[1] == 'w' ? PROT_WRITE : 0) |
                     (mode[2] == 'x' ? PROT_EXEC : 0);
            break;
        }
    }
    fclose(maps);
    return result;
}

int main() {
    const size_t page = sysconf(_SC_PAGESIZE);
    auto* memory = static_cast<char*>(mmap(nullptr, page * 3, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(memory != MAP_FAILED);
    // Reproduce the live crash: IL2CPP atomically updates allocator/class data
    // on the same page AFTER a vtable pointer has been patched.
    auto* runtime_word = new (memory + 64) std::atomic<unsigned>(0);
    void* value = memory + 128;
    auto result = mapped_patch_write(memory, &value, sizeof(value));
    assert(result.written && !result.restore_error);
    assert(permissions(memory) == (PROT_READ | PROT_WRITE));
    unsigned expected = 0;
    assert(runtime_word->compare_exchange_strong(expected, 42));
    assert(runtime_word->load() == 42);
    assert(*reinterpret_cast<void**>(memory) == value);

    // A readonly data page is temporarily writable, then readonly again.
    assert(mprotect(memory + page, page, PROT_READ) == 0);
    result = mapped_patch_write(memory + page, &value, sizeof(value));
    assert(result.written && !result.restore_error);
    assert(permissions(memory + page) == PROT_READ);
    assert(permissions(memory) == (PROT_READ | PROT_WRITE));
    assert(*reinterpret_cast<void**>(memory + page) == value);
    expected = 42;
    assert(runtime_word->compare_exchange_strong(expected, 43));

    // Refuse crossing mappings BEFORE modifying either side or their modes.
    const char original = memory[page - 1];
    result = mapped_patch_write(memory + page - 1, &value, sizeof(value));
    assert(!result.written && memory[page - 1] == original);
    assert(permissions(memory) == (PROT_READ | PROT_WRITE));
    assert(permissions(memory + page) == PROT_READ);
    assert(mprotect(memory + page * 2, page, PROT_NONE) == 0);
    assert(!mapped_patch_write(memory + page * 2, &value, sizeof(value)).written);
    assert(permissions(memory + page * 2) == PROT_NONE);
    assert(!mapped_patch_write(nullptr, &value, sizeof(value)).written);
    assert(!mapped_patch_write(memory, &value, 0).written);
    assert(munmap(memory, page * 3) == 0);
    puts("memory patch preserves runtime write access and readonly protection");
}
