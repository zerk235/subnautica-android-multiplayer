#include "hooks.h"

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "il2cpp_api.h"
#include "log.h"
#include "managed_hook.h"
#include "probe.h"

#define ORIGINAL_MAIN "libmain_orig.so"
#define IL2CPP_LIB "libil2cpp.so"

// Offsets inside libil2cpp.so taken from the IL2CPP global block that every api
// entry point touches. They are read directly on purpose: calling il2cpp_domain_get()
// before il2cpp_init() *creates* a half-initialized domain instead of reporting one.
#define IL2CPP_DOMAIN_PTR 0x5a0a9a8ul  // Il2CppDomain* s_Il2Cpp.domain
#define IL2CPP_THREAD_KEY 0x5a0aa88ul  // pthread_key_t s_Il2Cpp.thread_attached
#define IL2CPP_WARMUP_SECONDS 20

typedef void* (*dlsym_t)(void*, const char*);
typedef int (*jni_onload_t)(void*, void*);

static dlsym_t original_dlsym = nullptr;
static jni_onload_t original_jni_onload = nullptr;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static uintptr_t module_base(const char* needle) {
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) return 0;

    char line[1024];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), maps)) {
        if (!strstr(line, needle)) continue;

        uintptr_t start = strtoull(line, nullptr, 16);
        if (!base || start < base) base = start;

        if (strchr(line, '/')) break;
    }

    fclose(maps);
    return base;
}

bool mem_patch_write(void* addr, const void* data, size_t size) {
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)addr & ~((uintptr_t)page - 1);
    uintptr_t end = ((uintptr_t)addr + size + page - 1) & ~((uintptr_t)page - 1);
    size_t span = (size_t)(end - start);

    // Android never allows RWX on a file-backed mapping: ask for RW, patch, then RX.
    if (mprotect((void*)start, span, PROT_READ | PROT_WRITE) != 0) {
        LOG_ERROR("mprotect RW %p+%lu failed: %s", (void*)start, (unsigned long)span,
                  strerror(errno));
        return false;
    }

    memcpy(addr, data, size);
    __builtin___clear_cache((char*)addr, (char*)addr + size);

    // Android denies PROT_EXEC on file-backed pages for untrusted apps, so the page
    // may stay writable. That is harmless here: nothing else maps it.
    if (mprotect((void*)start, span, PROT_READ | PROT_EXEC) != 0) {
        LOG_WARN("page %p stays writable: %s", (void*)start, strerror(errno));
    }
    return true;
}

static void** lookup_got_slot(const char* lib, const char* sym, void** current) {
    uintptr_t base = module_base(lib);
    if (!base) {
        LOG_ERROR("cannot locate %s in memory", lib);
        return nullptr;
    }

    const Elf64_Ehdr* ehdr = reinterpret_cast<const Elf64_Ehdr*>(base);
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        LOG_ERROR("%s has no valid ELF header", lib);
        return nullptr;
    }

    const Elf64_Dyn* dyn = nullptr;
    const Elf64_Rela* jmprel = nullptr;
    const Elf64_Sym* symtab = nullptr;
    const char* strtab = nullptr;
    size_t pltrelsz = 0;

    const Elf64_Phdr* phdr = reinterpret_cast<const Elf64_Phdr*>(base + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<const Elf64_Dyn*>(base + phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) return nullptr;

    for (const Elf64_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_JMPREL: jmprel = reinterpret_cast<const Elf64_Rela*>(base + d->d_un.d_ptr); break;
            case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
            case DT_SYMTAB: symtab = reinterpret_cast<const Elf64_Sym*>(base + d->d_un.d_ptr); break;
            case DT_STRTAB: strtab = reinterpret_cast<const char*>(base + d->d_un.d_ptr); break;
            default: break;
        }
    }
    if (!jmprel || !symtab || !strtab) return nullptr;

    for (size_t i = 0; i < pltrelsz / sizeof(Elf64_Rela); i++) {
        uint32_t index = (uint32_t)(jmprel[i].r_info >> 32);
        const char* name = strtab + symtab[index].st_name;
        if (strcmp(name, sym) != 0) continue;

        void** slot = reinterpret_cast<void**>(base + jmprel[i].r_offset);
        if (current) *current = *slot;
        return slot;
    }

    LOG_ERROR("%s: GOT slot for %s not found", lib, sym);
    return nullptr;
}

static void sleep_seconds(int seconds) {
    struct timespec ts;
    ts.tv_sec = seconds;
    ts.tv_nsec = 0;
    nanosleep(&ts, nullptr);
}

static void* probe_thread(void*) {
    LOG_INFO("probe thread: waiting for the runtime");

    for (int i = 0; i < 600; i++) {
        if (resolve_il2cpp_api()) break;
        sleep_seconds(1);
    }
    if (!il2cpp_domain_get) {
        LOG_ERROR("probe thread: il2cpp api unavailable");
        return nullptr;
    }

    uintptr_t base = module_base(IL2CPP_LIB);
    if (!base) {
        LOG_ERROR("probe thread: cannot locate %s", IL2CPP_LIB);
        return nullptr;
    }

    for (int i = 0; i < 180; i++) {
        void* domain = *(void**)(base + IL2CPP_DOMAIN_PTR);
        uint32_t key = *(uint32_t*)(base + IL2CPP_THREAD_KEY);
        if (domain && key) {
            LOG_INFO("runtime is up: domain=%p thread_key=%u", domain, key);
            LOG_INFO("SNMod IL2CPP ready");
            break;
        }
        sleep_seconds(1);
    }

    void* domain = *(void**)(base + IL2CPP_DOMAIN_PTR);
    if (!domain) {
        LOG_ERROR("probe thread: domain never appeared");
        return nullptr;
    }

    LOG_INFO("probe thread: waiting %d s for the engine to settle", IL2CPP_WARMUP_SECONDS);
    for (int i = 0; i < IL2CPP_WARMUP_SECONDS; i++) {
        sleep_seconds(1);
        LOG_INFO("probe thread: warmup %d/%d", i + 1, IL2CPP_WARMUP_SECONDS);
    }

    // Touching the runtime from a thread Unity does not know about can block on the
    // runtime lock, so register this thread first.
    if (il2cpp_thread_attach) {
        LOG_INFO("probe thread: attaching to the runtime");
        il2cpp_thread_attach(reinterpret_cast<struct Il2CppDomain*>(domain));
        LOG_INFO("probe thread: attached");
    }

    LOG_INFO("probe thread: probing classes");
    probe_game();
    LOG_INFO("probe thread: classes done, installing hooks");
    install_managed_hooks();

    LOG_INFO("probe thread: done");
    return nullptr;
}

static void start_probe_thread() {
    pthread_t thread;
    if (pthread_create(&thread, nullptr, probe_thread, nullptr) == 0) {
        pthread_detach(thread);
    } else {
        LOG_ERROR("cannot start probe thread");
    }
}

static int hooked_jni_onload(void* vm, void* reserved) {
    LOG_INFO("libil2cpp.so!JNI_OnLoad(vm=%p)", vm);

    int result = original_jni_onload(vm, reserved);

    LOG_INFO("original JNI_OnLoad returned %d", result);

    start_probe_thread();
    return result;
}

static void* hooked_dlsym(void* handle, const char* name) {
    void* result = original_dlsym(handle, name);

    pthread_mutex_lock(&g_lock);
    bool intercept = name && strcmp(name, "JNI_OnLoad") == 0 && result && !original_jni_onload;
    if (intercept) {
        original_jni_onload = reinterpret_cast<jni_onload_t>(result);
        LOG_INFO("bootstrap dlsym(\"%s\") -> %p, intercepting", name, result);
    }
    pthread_mutex_unlock(&g_lock);

    if (intercept) return reinterpret_cast<void*>(hooked_jni_onload);
    return result;
}

void install_init_hook() {
    void* current = nullptr;
    void** slot = lookup_got_slot(ORIGINAL_MAIN, "dlsym", &current);
    if (!slot) {
        LOG_ERROR("cannot hook dlsym in %s", ORIGINAL_MAIN);
        return;
    }

    original_dlsym = reinterpret_cast<dlsym_t>(current);
    if (!original_dlsym) {
        LOG_ERROR("%s: dlsym GOT slot is null", ORIGINAL_MAIN);
        return;
    }

    void* hook = reinterpret_cast<void*>(hooked_dlsym);
    if (!mem_patch_write(slot, &hook, sizeof(void*))) return;

    LOG_INFO("dlsym hook installed in %s at %p", ORIGINAL_MAIN, (void*)slot);
    LOG_INFO("SNMod hooks installed (dlsym)");
}
