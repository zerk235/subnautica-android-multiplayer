#pragma once

#include <cstddef>

void install_init_hook();

// Copies bytes while retaining the mapping's original permissions. Writable
// runtime allocations must remain writable for subsequent IL2CPP initialization.
bool mem_patch_write(void* addr, const void* data, size_t size);
