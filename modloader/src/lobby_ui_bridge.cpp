#include "lobby_ui_bridge.h"

#if defined(SNMOD_LOBBY_UI)
#include "lobby_dex.h"
#include "net.h"
#include "log.h"

#include <mutex>
#include <string>

namespace {

struct Action {
    jint kind = 0;
    uint32_t host = 0;
    uint32_t room = 0;
};

std::mutex action_lock;
Action pending;
jclass panel = nullptr;
jmethodID show = nullptr;
jweak last_activity = nullptr;
std::string last_view;

void JNICALL action(JNIEnv*, jclass, jint kind, jint host, jint room) {
    std::lock_guard<std::mutex> lock(action_lock);
    pending = {kind, static_cast<uint32_t>(host), static_cast<uint32_t>(room)};
}

void consume_action(bool at_menu) {
    Action input;
    {
        std::lock_guard<std::mutex> lock(action_lock);
        input = pending;
        pending = {};
    }
    if (!at_menu) return;
    switch (input.kind) {
        case 1: net_lobby_open(); break;
        case 2: net_lobby_create(); break;
        case 3: net_lobby_enter(input.host, input.room); break;
        case 4: net_lobby_toggle_ready(); break;
        case 5: net_lobby_start(); break;
        case 6: net_lobby_close(); break;
    }
}

bool clear_exception(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

bool load_panel(JNIEnv* env, jobject activity) {
    const auto buffer_type = env->FindClass("java/nio/ByteBuffer");
    const auto dex_type = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    const auto activity_type = env->GetObjectClass(activity);
    if (clear_exception(env) || !buffer_type || !dex_type || !activity_type) return false;

    const auto construct = env->GetMethodID(dex_type, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    const auto load = env->GetMethodID(dex_type, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    const auto parent_method = env->GetMethodID(activity_type, "getClassLoader", "()Ljava/lang/ClassLoader;");
    if (clear_exception(env) || !construct || !load || !parent_method) return false;

    // The buffer references static bytes in this library for the class loader's lifetime.
    jobject buffer = env->NewDirectByteBuffer(const_cast<unsigned char*>(snmp_lobby_dex), sizeof(snmp_lobby_dex));
    jobject parent = env->CallObjectMethod(activity, parent_method);
    if (clear_exception(env) || !buffer || !parent) return false;
    jobject loader = env->NewObject(dex_type, construct, buffer, parent);
    if (clear_exception(env) || !loader) return false;
    auto type = static_cast<jclass>(env->CallObjectMethod(loader, load, env->NewStringUTF("snmp.menu.LobbyPanel")));
    if (clear_exception(env) || !type) return false;
    auto render = env->GetStaticMethodID(type, "render", "(Landroid/app/Activity;Ljava/lang/String;)V");
    if (clear_exception(env) || !render) return false;
    JNINativeMethod method{const_cast<char*>("act"), const_cast<char*>("(III)V"), reinterpret_cast<void*>(action)};
    const int result = env->RegisterNatives(type, &method, 1);
    if (clear_exception(env) || result != JNI_OK) return false;
    panel = static_cast<jclass>(env->NewGlobalRef(type));
    show = render;
    return panel != nullptr;
}

std::string encode_view(bool at_menu) {
    if (!at_menu) return "";
    LobbyStatus state{};
    net_lobby_snapshot(&state);
    std::string view = std::to_string(state.view);
    const auto append = [&](uint32_t value) { view += ','; view += std::to_string(value); };
    append(state.host);
    append(state.guest);
    append(state.host_ready);
    append(state.guest_ready);
    append(state.can_start);
    append(state.room_lost);
    append(state.view == kLobbyWaiting ? net_session_state() : kSessionDiscovering);
    append(state.room);
    for (size_t i = 0; i < state.count; ++i) {
        append(state.rooms[i].host);
        append(state.rooms[i].room);
    }
    return view;
}

}  // namespace

void lobby_ui_tick(JNIEnv* env, jobject activity, bool at_menu) {
    consume_action(at_menu);
    static bool attempted = false;
    if (!panel && at_menu && !attempted) {
        attempted = true;
        if (!load_panel(env, activity)) LOG_WARN("lobby: failed to load the menu panel");
        else LOG_INFO("lobby: menu panel available");
    }
    if (!panel) return;
    const auto view = encode_view(at_menu);
    if (view == last_view && env->IsSameObject(last_activity, activity)) return;
    auto text = env->NewStringUTF(view.c_str());
    if (!text) {
        clear_exception(env);
        return;
    }
    env->CallStaticVoidMethod(panel, show, activity, text);
    env->DeleteLocalRef(text);
    if (clear_exception(env)) return;
    if (last_activity) env->DeleteWeakGlobalRef(last_activity);
    last_activity = env->NewWeakGlobalRef(activity);
    last_view = view;
}
#else
void lobby_ui_tick(JNIEnv*, jobject, bool) {}
#endif
