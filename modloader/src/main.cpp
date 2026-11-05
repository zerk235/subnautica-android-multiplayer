#include <dlfcn.h>
#include <elf.h>
#include <jni.h>
#include <pthread.h>
#include <time.h>

#include "hooks.h"
#include "log.h"
#include "launch_role_bridge.h"

#define ORIGINAL_MAIN "libmain_orig.so"

static void* log_retry_thread(void*) {
    for (int i = 0; i < 120; i++) {
        if (mod_log_open()) return nullptr;

        struct timespec ts = {0, 500000000};
        nanosleep(&ts, nullptr);
    }
    return nullptr;
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    mod_log_open();
    LOG_INFO("=== Subnautica mod loader injected ===");
    LOG_INFO("SNMod bootstrap loaded");

    pthread_t retry;
    if (pthread_create(&retry, nullptr, log_retry_thread, nullptr) == 0) {
        pthread_detach(retry);
    }

    void* original = dlopen(ORIGINAL_MAIN, RTLD_NOW);
    if (!original) {
        LOG_ERROR("cannot load %s: %s", ORIGINAL_MAIN, dlerror());
        return JNI_VERSION_1_6;
    }

    install_init_hook();

    auto original_onload = reinterpret_cast<jint (*)(JavaVM*, void*)>(dlsym(original, "JNI_OnLoad"));
    if (original_onload) {
        original_onload(vm, reserved);
        LOG_INFO("original JNI_OnLoad returned");
    } else {
        LOG_ERROR("original JNI_OnLoad missing");
    }

    JNIEnv* env=nullptr;
    if(vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)==JNI_OK)
        launch_role_bridge_init(vm,env);
    return JNI_VERSION_1_6;
}
