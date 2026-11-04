#pragma once

#include <cstddef>

void install_init_hook();

// Copies bytes into a mapped page. Android denies PROT_EXEC on file-backed pages, so the
// page can end up staying writable; that is fine for data pages.
bool mem_patch_write(void* addr, const void* data, size_t size);
