#include "player_marker_policy.h"

// Archived HUD experiments below are NOT production implementations. Creation and
// heuristic cleanup requests are refused. Only explicit text control and a read-only
// probe are reachable from the entrypoint; no Android-wide impossibility is established.
// This fragment currently relies on unpublished local experiment helpers; see docs.
// Original experiment goal: a real HUD ping, the same system that draws Seamoth/beacon markers
// (PingInstance + PingManager -> uGUI_Pings). The game itself projects the icon, the
// label and the distance; the mod only owns a bare transform that follows the peer.
//
// Nothing is rendered in the world: the marker object carries a PingInstance and a
// Transform, no Renderer and no model, so it cannot be seen as scenery and cannot
// collide. Creation is opt-in through a request file so a broken first attempt can
// never take a session down unasked.
//
// The text hint from the first version stays as a fallback: it is what proves the
// session is healthy when the ping system is unavailable.
constexpr const char* kPingMarkerRequest =
    "/sdcard/Android/data/com.unknownworlds.subnautica/files/mod_log/ping_marker.request";

// PingType used for the partner marker. 1 is Lifepod, a type the loaded world already
// carries, so uGUI_Pings is guaranteed to have an icon for it. 6 (Beacon) was used in the
// attempt that took the game down inside libunity about a second after creation.
constexpr int kPingPlayerType = 1;
constexpr float kPingRange = 500.0f;

struct PingMarker {
    bool resolved = false;
    bool failed = false;
    player_marker::Control control;
    bool allow_create = false;
    bool probe_done = false;
    bool created = false;
    // Set while the marker is parented to nothing and only its position is driven.
    void* game_object = nullptr;
    void* transform = nullptr;
    void* instance = nullptr;
    uint32_t peer = 0;
    double next_arm_check = 0;
    double next_report = 0;
    unsigned long long updates = 0;

    struct Il2CppClass* ping_class = nullptr;
    void* ping_type_obj = nullptr;
    void* prefab_field = nullptr;
    void* origin_field = nullptr;
    void* type_field = nullptr;
    void* range_field = nullptr;
    void* min_dist_field = nullptr;
    void* visible_field = nullptr;
    void* color_field = nullptr;
    void* display_field = nullptr;
    void* id_field = nullptr;
    void* label_field = nullptr;
    bool cleanup_requested = false;
    Entry<void (*)(void*, void*, void*)> game_object_ctor;
    Entry<void (*)(void*, void*)> manager_register;
    Entry<void (*)(void*, void*, void*)> set_label;
    // PingInstance.Initialize is private; Nitrox calls it explicitly right after the fields
    // are written, instead of waiting for Start to do it a frame later.
    Entry<void (*)(void*, void*)> instance_initialize;
    // PingInstance.SetVisible(bool) is public and notifies the HUD; a freshly instantiated
    // beacon prefab starts with visible = false, which is why no icon appeared.
    Entry<void (*)(void*, bool, void*)> ping_set_visible;
    // Explicit HUD notifications. SetVisible/SetLabel early-return when the value already
    // matches, so a field written beforehand would never reach uGUI_Pings.
    Entry<void (*)(void*, void*)> notify_visible;
    Entry<void (*)(void*, void*)> notify_rename;
    bool allow_create_nitrox = false;
    bool allow_create_beacon = false;
};
static PingMarker g_ping_marker;

void* ping_field_of(struct Il2CppClass* klass, const char* name) {
    if (!klass || !il2cpp_class_get_fields || !il2cpp_field_get_name) return nullptr;
    // Fields declared on the type itself come first; MonoBehaviour/PingInstance keeps
    // everything we need on PingInstance, but walking up costs nothing and survives a
    // future move of a field into a base type.
    for (auto* current = klass; current; current = il2cpp_class_get_parent(current)) {
        void* iterator = nullptr;
        for (auto* field = il2cpp_class_get_fields(current, &iterator); field;
             field = il2cpp_class_get_fields(current, &iterator)) {
            if (!strcmp(il2cpp_field_get_name(field), name)) return field;
        }
    }
    return nullptr;
}

bool ping_marker_resolve() {
    auto& m = g_ping_marker;
    if (m.resolved) return !m.failed;
    m.resolved = true;
    if (!il2cpp_class_get_type || !il2cpp_type_get_object || !il2cpp_object_new ||
        !il2cpp_string_new || !g_set_active || !g_add_component || !g_get_go_transform) {
        m.failed = true;
        LOG_WARN("ping marker: core il2cpp/game api missing");
        return false;
    }
    m.ping_class = find_class_in_domain("", "PingInstance");
    auto* manager = find_class_in_domain("", "PingManager");
    auto* game_object_class = find_class_in_domain("UnityEngine", "GameObject");
    if (!m.ping_class || !manager || !game_object_class) {
        m.failed = true;
        LOG_WARN("ping marker: classes missing ping=%p manager=%p go=%p", (void*)m.ping_class,
                 (void*)manager, (void*)game_object_class);
        return false;
    }
    m.ping_type_obj = il2cpp_type_get_object(il2cpp_class_get_type(m.ping_class));
    m.origin_field = ping_field_of(m.ping_class, "origin");
    m.type_field = ping_field_of(m.ping_class, "pingType");
    m.range_field = ping_field_of(m.ping_class, "range");
    m.min_dist_field = ping_field_of(m.ping_class, "minDist");
    m.visible_field = ping_field_of(m.ping_class, "visible");
    m.color_field = ping_field_of(m.ping_class, "colorIndex");
    m.display_field = ping_field_of(m.ping_class, "displayPingInManager");
    m.id_field = ping_field_of(m.ping_class, "_id");
    m.manager_register.bind(find_method(manager, "Register", 1));
    m.set_label.bind(find_method(m.ping_class, "SetLabel", 1));
    m.instance_initialize.bind(find_method(m.ping_class, "Initialize", 0));
    m.ping_set_visible.bind(find_method(m.ping_class, "SetVisible", 1));
    m.notify_visible.bind(find_method(manager, "NotifyVisible", 1));
    m.notify_rename.bind(find_method(manager, "NotifyRename", 1));
    m.game_object_ctor.bind(find_method_typed(game_object_class, ".ctor", 1, "system.string"));
    if (!m.game_object_ctor) m.game_object_ctor.bind(find_method(game_object_class, ".ctor", 0));
    if (!m.origin_field || !m.type_field || !m.range_field || !m.visible_field ||
        !m.display_field || !m.id_field || !m.game_object_ctor || !m.ping_type_obj) {
        m.failed = true;
        LOG_WARN("ping marker: api incomplete origin=%p type=%p range=%p visible=%p display=%p "
                 "id=%p ctor=%p typeObj=%p",
                 m.origin_field, m.type_field, m.range_field, m.visible_field, m.display_field,
                 m.id_field, (void*)m.game_object_ctor.call, m.ping_type_obj);
        return false;
    }
    LOG_INFO("ping marker: api ready register=%p set_label=%p ctor=%p",
             (void*)m.manager_register.call, (void*)m.set_label.call,
             (void*)m.game_object_ctor.call);
    return true;
}

// The request file mirrors the creature probe's arm file: one line, removed on read, so
// a marker is never created by surprise after a restart.
bool ping_marker_read_arm() {
    auto& m = g_ping_marker;
    double now = build_now();
    if (now < m.next_arm_check) return m.control.text_enabled;
    m.next_arm_check = now + 2.0;
    FILE* file = fopen(kPingMarkerRequest, "rb");
    if (!file) return m.control.text_enabled;
    char command[32]{};
    size_t n = fread(command, 1, sizeof(command), file);
    bool oversized = fgetc(file) != EOF;
    bool unreadable = ferror(file) != 0;
    fclose(file);
    unlink(kPingMarkerRequest);
    const auto action = oversized || unreadable ? player_marker::Command::Ignore
        : player_marker::parse(command, static_cast<unsigned>(n));
    m.control.apply(action);
    switch(action) {
    case player_marker::Command::On:
        m.next_report=0;
        LOG_INFO("player marker: text hints enabled");
        break;
    case player_marker::Command::Hud:
        LOG_INFO("peer HUD: requested; text hints disabled");
        break;
    case player_marker::Command::Off:
        LOG_INFO("player marker: text hints and pending probe disabled; HUD disabled");
        break;
    case player_marker::Command::Probe:
        m.probe_done=false;
        LOG_INFO("ping marker: read-only probe requested; text state unchanged");
        break;
    case player_marker::Command::Refused:
        // Archive only: failed creation/cleanup paths must never be re-enabled by a file.
        LOG_WARN("ping marker: unsafe HUD/cleanup command refused; see PING_MARKER_2026-11-09.md");
        break;
    default:
        LOG_INFO("ping marker: request ignored (%zu bytes, oversized=%u)",n,unsigned(oversized));
        break;
    }

    return m.control.text_enabled;
}

bool ping_marker_create() {
    auto& m = g_ping_marker;
    if (m.created) return true;
    if (!ping_marker_resolve()) return false;
    // Created inactive first: AddComponent on an inactive object does not run the
    // component's OnEnable, which is where PingInstance reads its fields and registers
    // itself with PingManager. Everything is in place before that runs.
    auto* game_object_class = find_class_in_domain("UnityEngine", "GameObject");
    if (!game_object_class) return false;
    void* name = il2cpp_string_new("SNMP_PeerMarker");
    LOG_INFO("ping marker: creating marker object");
    void* go = il2cpp_object_new(game_object_class);
    if (!go) {
        LOG_WARN("ping marker: object_new(GameObject) returned null");
        return false;
    }
    m.game_object_ctor.call(go, name, m.game_object_ctor.method);
    g_set_active.call(go, false, g_set_active.method);
    void* instance = g_add_component.call(go, m.ping_type_obj, g_add_component.method);
    if (!instance) {
        LOG_WARN("ping marker: AddComponent(PingInstance) returned null");
        return false;
    }
    void* transform = g_get_go_transform.call(go, g_get_go_transform.method);
    if (!transform) {
        LOG_WARN("ping marker: marker has no transform");
        return false;
    }
    int32_t type = kPingPlayerType;
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.type_field, &type);
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.origin_field, &transform);
    float range = kPingRange;
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.range_field, &range);
    float min_dist = 0.0f;
    if (m.min_dist_field)
        il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.min_dist_field,
                               &min_dist);
    bool no_manager = false;
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.display_field,
                           &no_manager);
    bool visible = true;
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.visible_field, &visible);
    int32_t color = 0;
    if (m.color_field)
        il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.color_field, &color);
    // A stable id keeps the ping addressable; PingManager keys its table by it.
    void* id = il2cpp_string_new("snmp-peer");
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.id_field, &id);
    if (m.set_label && m.set_label.call) {
        void* label = il2cpp_string_new("Напарник");
        m.set_label.call(instance, label, m.set_label.method);
    }
    m.game_object = go;
    m.transform = transform;
    m.instance = instance;
    m.created = true;
    // Activating is what runs OnEnable -> Initialize -> PingManager.Register.
    g_set_active.call(go, true, g_set_active.method);
    LOG_INFO("ping marker: created go=%p instance=%p type=%d range=%.0f", go, instance,
             kPingPlayerType, kPingRange);
    return true;
}

void ping_marker_destroy(const char* reason) {
    auto& m = g_ping_marker;
    // The ping lives on the peer's avatar clone, which the game destroys with the peer. This
    // only drops the bookkeeping: deactivating the avatar would hide the partner itself.
    LOG_INFO("ping marker: dropped reason=%s updates=%llu", reason, m.updates);
    m.game_object = nullptr;
    m.transform = nullptr;
    m.instance = nullptr;
    m.created = false;
    m.peer = 0;
}

// Read-only: what pings does this world actually have? Tells us which template can be
// cloned instead of building a GameObject from scratch, which is what crashed.
void ping_marker_probe() {
    auto& m = g_ping_marker;
    if (m.probe_done) return;
    if (!ping_marker_resolve()) { m.probe_done = true; return; }
    if (!g_find_objects_all || !m.ping_type_obj || !g_array_get_length || !g_array_get_value) return;
    m.probe_done = true;
    void* list = g_find_objects_all.call(m.ping_type_obj, true, g_find_objects_all.method);
    if (!list) {
        LOG_INFO("ping probe: FindObjectsOfType returned null");
        return;
    }
    int count = g_array_get_length.call(list, g_array_get_length.method);
    LOG_INFO("ping probe: %d PingInstance objects in the loaded world", count);
    for (int i = 0; i < count && i < 64; ++i) {
        void* instance = g_array_get_value.call(list, i, g_array_get_value.method);
        if (!instance) continue;
        int32_t type = -1;
        if (m.type_field)
            il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(instance), m.type_field, &type);
        float range = -1.0f;
        if (m.range_field)
            il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(instance), m.range_field, &range);
        bool display = false;
        if (m.display_field)
            il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(instance), m.display_field,
                                   &display);
        void* go = g_get_game_object ? g_get_game_object.call(instance, g_get_game_object.method)
                                     : nullptr;
        const char* name = "?";
        char name_buffer[96] = {0};
        if (go && g_get_name) {
            void* text = g_get_name.call(go, g_get_name.method);
            if (text) {
                // Il2CppString layout: {klass, monitor, length, chars}; chars start at +0x14.
                int32_t length = *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(text) + 0x10);
                const char16_t* chars =
                    reinterpret_cast<const char16_t*>(reinterpret_cast<char*>(text) + 0x14);
                if (length > 0 && length < 95) {
                    for (int32_t c = 0; c < length; ++c)
                        name_buffer[c] = chars[c] < 0x80 ? static_cast<char>(chars[c]) : '?';
                    name_buffer[length] = 0;
                    name = name_buffer;
                }
            }
        }
        int renderers = 0;
        if (go && g_renderer_type_obj && g_get_component && !g_get_component_is_string) {
            for (int depth = 0; depth <= 5 && go; ++depth) {
                if (g_get_component.call(go, g_renderer_type_obj, g_get_component.method))
                    ++renderers;
                void* transform = g_get_go_transform ? g_get_go_transform.call(go, g_get_go_transform.method) : nullptr;
                if (!transform || !g_transform_get_parent) break;
                void* parent = g_transform_get_parent.call(transform, g_transform_get_parent.method);
                if (!parent || !g_get_game_object) break;
                go = g_get_game_object.call(parent, g_get_game_object.method);
            }
        }
        LOG_INFO("ping probe: [%d] name=%s type=%d range=%.0f display=%u renderers=%d", i, name,
                 type, range, display ? 1u : 0u, renderers);
    }
}

int ping_marker_renderers(void* go, int depth) {
    if (!go || depth > 6) return 0;
    int total = 0;
    if (g_get_component && g_renderer_type_obj && !g_get_component_is_string &&
        g_get_component.call(go, g_renderer_type_obj, g_get_component.method))
        ++total;
    void* transform =
        g_get_go_transform ? g_get_go_transform.call(go, g_get_go_transform.method) : nullptr;
    if (!transform || !g_get_child || !g_get_child_count) return total;
    int children = g_get_child_count.call(transform, g_get_child_count.method);
    for (int i = 0; i < children && i < 64; ++i) {
        void* child = g_get_child.call(transform, i, g_get_child.method);
        if (!child || !g_get_game_object) continue;
        total += ping_marker_renderers(g_get_game_object.call(child, g_get_game_object.method),
                                       depth + 1);
    }
    return total;
}

// Instantiating an existing ping keeps the object inside Unity's own bookkeeping, which is
// exactly what the scratch-built GameObject lacked when uGUI_Pings crashed on it. Only a
// template with no renderers anywhere is accepted, so a clone cannot appear as scenery.
void* ping_marker_pick_template() {
    auto& m = g_ping_marker;
    void* list = g_find_objects_all.call(m.ping_type_obj, true, g_find_objects_all.method);
    if (!list) {
        LOG_WARN("ping marker: template search returned nothing");
        return nullptr;
    }
    int count = g_array_get_length.call(list, g_array_get_length.method);
    void* best = nullptr;
    int best_renderers = 1 << 20;
    int32_t best_type = -1;
    for (int i = 0; i < count && i < 256; ++i) {
        void* instance = g_array_get_value.call(list, i, g_array_get_value.method);
        if (!instance || !g_get_game_object) continue;
        void* go = g_get_game_object.call(instance, g_get_game_object.method);
        if (!go) continue;
        const int renderers = ping_marker_renderers(go, 0);
        int32_t type = -1;
        if (m.type_field)
            il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(instance), m.type_field, &type);
        if (renderers < best_renderers) {
            best_renderers = renderers;
            best = go;
            best_type = type;
        }
        if (!renderers) break;
    }
    LOG_INFO("ping marker: template search found=%d best_renderers=%d best_type=%d", count,
             best_renderers, best_type);
    if (!best || best_renderers != 0) {
        LOG_WARN("ping marker: no renderer-free PingInstance to clone; refusing to clone");
        return nullptr;
    }
    return best;
}

bool ping_marker_clone_create() {
    auto& m = g_ping_marker;
    if (!ping_marker_resolve()) return false;
    if (!g_instantiate || !g_find_objects_all || !g_array_get_length || !g_array_get_value ||
        !g_get_component) {
        LOG_WARN("ping marker: clone api missing");
        return false;
    }
    void* source = ping_marker_pick_template();
    if (!source) return false;
    Vector3 position{0.0f, 0.0f, 0.0f};
    Quaternion rotation{0.0f, 0.0f, 0.0f, 1.0f};
    void* go = g_instantiate.call(source, position, rotation, g_instantiate.method);
    if (!go) {
        LOG_WARN("ping marker: Instantiate(ping template) returned null");
        return false;
    }
    g_set_active.call(go, false, g_set_active.method);
    void* instance = g_get_component.call(go, m.ping_type_obj, g_get_component.method);
    void* transform = g_get_go_transform.call(go, g_get_go_transform.method);
    if (!instance || !transform) {
        LOG_WARN("ping marker: clone lacks PingInstance or transform instance=%p transform=%p",
                 instance, transform);
        g_object_destroy.call(go, g_object_destroy.method);
        return false;
    }
    int32_t type = kPingPlayerType;
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.type_field, &type);
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.origin_field, &transform);
    float range = kPingRange;
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.range_field, &range);
    bool no_manager = false;
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.display_field,
                           &no_manager);
    bool visible = true;
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.visible_field, &visible);
    if (m.set_label && m.set_label.call) {
        void* label = il2cpp_string_new("Напарник");
        m.set_label.call(instance, label, m.set_label.method);
    }
    m.game_object = go;
    m.transform = transform;
    m.instance = instance;
    m.created = true;
    g_set_active.call(go, true, g_set_active.method);
    LOG_INFO("ping marker: cloned go=%p instance=%p type=%d range=%.0f", go, instance,
             kPingPlayerType, kPingRange);
    return true;
}

// Attach the ping to the peer's avatar clone instead of building a GameObject.
//
// The clone is a real scene object that the game already moves and destroys, so the marker
// needs no transform driving of its own: origin is the clone's own transform. AddComponent
// on an existing object is the same call this mod already makes for SkyApplier, so the one
// remaining difference from the attempt that crashed is that no object is created here.
bool ping_marker_attach(void* avatar_go) {
    auto& m = g_ping_marker;
    if (!avatar_go || !ping_marker_resolve()) return false;
    if (!g_get_go_transform || !g_get_component) return false;
    if (g_get_component.call(avatar_go, m.ping_type_obj, g_get_component.method)) {
        LOG_INFO("ping marker: avatar already carries a PingInstance");
        return false;
    }
    void* instance = g_add_component.call(avatar_go, m.ping_type_obj, g_add_component.method);
    if (!instance) {
        LOG_WARN("ping marker: AddComponent(PingInstance) on the avatar returned null");
        return false;
    }
    void* transform = g_get_go_transform.call(avatar_go, g_get_go_transform.method);
    int32_t type = kPingPlayerType;
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.type_field, &type);
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.origin_field, &transform);
    float range = kPingRange;
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.range_field, &range);
    if (m.min_dist_field) {
        float min_dist = 0.0f;
        il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.min_dist_field,
                               &min_dist);
    }
    bool no_manager = false;
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.display_field,
                           &no_manager);
    bool visible = true;
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.visible_field, &visible);
    void* id = il2cpp_string_new("snmp-peer");
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.id_field, &id);
    if (m.set_label && m.set_label.call) {
        void* label = il2cpp_string_new("Напарник");
        m.set_label.call(instance, label, m.set_label.method);
    }
    m.instance = instance;
    m.created = true;
    LOG_INFO("ping marker: attached to avatar go=%p instance=%p type=%d range=%.0f", avatar_go,
             instance, kPingPlayerType, kPingRange);
    return true;
}

// Faithful copy of Nitrox's PlayerPing.SpawnPlayerPing sequence (NitroxClient/MonoBehaviours/
// Gui/InGame/PlayerPing.cs), which is known to work on the PC build:
//   new GameObject -> AddComponent<PingInstance> -> set pingType/origin/minDist/_label/
//   displayPingInManager/visible -> Initialize() explicitly.
// Differences from our earlier attempts, all deliberately kept: PingType.Signal instead of
// Beacon or Lifepod, minDist = 5, `range` left untouched, `_id` left empty so Initialize
// generates the GUID itself, and no reliance on Start.
bool ping_marker_create_nitrox() {
    auto& m = g_ping_marker;
    if (!ping_marker_resolve()) return false;
    if (!m.allow_create_nitrox) return false;
    if (!m.instance_initialize || !m.origin_field || !m.type_field || !m.display_field ||
        !m.visible_field) {
        LOG_WARN("ping marker: nitrox api missing initialize=%p",
                 (void*)m.instance_initialize.call);
        return false;
    }
    auto* game_object_class = find_class_in_domain("UnityEngine", "GameObject");
    if (!game_object_class) return false;
    void* name = il2cpp_string_new("SNMP_PeerPing");
    void* go = il2cpp_object_new(game_object_class);
    if (!go) {
        LOG_WARN("ping marker: object_new returned null");
        return false;
    }
    m.game_object_ctor.call(go, name, m.game_object_ctor.method);
    void* instance = g_add_component.call(go, m.ping_type_obj, g_add_component.method);
    if (!instance) {
        LOG_WARN("ping marker: AddComponent returned null");
        return false;
    }
    void* transform = g_get_go_transform.call(go, g_get_go_transform.method);
    int32_t type = 7;  // PingType.Signal
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.type_field, &type);
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.origin_field, &transform);
    if (m.min_dist_field) {
        float min_dist = 5.0f;
        il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.min_dist_field,
                               &min_dist);
    }
    bool no_manager = false;
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.display_field,
                           &no_manager);
    bool visible = true;
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(instance), m.visible_field, &visible);
    if (m.set_label && m.set_label.call) {
        void* label = il2cpp_string_new("Напарник");
        m.set_label.call(instance, label, m.set_label.method);
    }
    m.game_object = go;
    m.transform = transform;
    m.instance = instance;
    m.created = true;
    LOG_INFO("ping marker: nitrox sequence prepare go=%p instance=%p type=Signal minDist=5", go,
             instance);
    m.instance_initialize.call(instance, m.instance_initialize.method);
    LOG_INFO("ping marker: nitrox Initialize returned");
    return true;
}

// The game's own Beacon prefab. Everything the crash-prone path needed by hand
// (PingInstance, registration, HUD icon) is already wired inside the prefab by the game,
// and spawning a prefab is the path this mod already uses for vehicles and held props.
// TechType.Beacon = 508.
constexpr uint32_t kBeaconTechType = 508;

bool ping_marker_spawn_beacon(const Vector3& position) {
    auto& m = g_ping_marker;
    if (!ping_marker_resolve()) return false;
    if (!g_instantiate) {
        LOG_WARN("ping marker: Instantiate is not bound");
        return false;
    }
    void* prefab = find_prefab_source(kBeaconTechType);
    if (!prefab) {
        // Poll once a second instead of silently waiting: a failed or still-loading prefab
        // request used to look exactly like a broken HUD.
        static double next_wait_log = 0;
        double now = build_now();
        if (now >= next_wait_log) {
            next_wait_log = now + 2.0;
            LOG_INFO("ping marker: beacon waiting for the prefab (tech 508), world_ops=%u",
                     world_operations_ready() ? 1u : 0u);
        }
        return false;
    }
    Quaternion rotation{0.0f, 0.0f, 0.0f, 1.0f};
    void* go = g_instantiate.call(prefab, position, rotation, g_instantiate.method);
    if (!go) {
        LOG_WARN("ping marker: Instantiate(Beacon prefab) returned null");
        return false;
    }
    if (g_set_active) g_set_active.call(go, true, g_set_active.method);
    m.game_object = go;
    m.transform = g_get_go_transform ? g_get_go_transform.call(go, g_get_go_transform.method)
                                     : nullptr;
    // The prefab's own ping is registered by the game; only its display parameters are
    // rewritten. range=5 on the prefab is why the HUD icon vanished a few metres away.
    if (m.ping_type_obj && g_get_component && !g_get_component_is_string) {
        void* ping = find_component_in_hierarchy(go, m.ping_type_obj, nullptr, 0);
        if (!ping && g_get_component) {
            // The ping can sit on a child object rather than on the root.
            ping = g_get_component.call(go, m.ping_type_obj, g_get_component.method);
        }
        if (ping) {
            bool display = true;
            if (m.display_field)
                il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(ping), m.display_field,
                                       &display);
            if (m.range_field) {
                float range = kPingRange;
                il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(ping), m.range_field,
                                       &range);
            }
            if (m.min_dist_field) {
                float min_dist = 0.0f;
                il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(ping), m.min_dist_field,
                                       &min_dist);
            }
            if (m.set_label && m.set_label.call) {
                void* label = il2cpp_string_new("Напарник");
                m.set_label.call(ping, label, m.set_label.method);
            }
            // SetVisible compares against the current value and returns early, so it runs
            // before anything writes the field; the explicit notify covers the case where the
            // prefab already started with visible = true.
            if (m.ping_set_visible && m.ping_set_visible.call)
                m.ping_set_visible.call(ping, true, m.ping_set_visible.method);
            bool visible = true;
            il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(ping), m.visible_field,
                                   &visible);
            if (m.notify_visible && m.notify_visible.call)
                m.notify_visible.call(ping, m.notify_visible.method);
            if (m.notify_rename && m.notify_rename.call)
                m.notify_rename.call(ping, m.notify_rename.method);
            LOG_INFO("ping marker: beacon ping tuned instance=%p range=%.0f visible=1 label=Напарник "
                     "notify_visible=%p notify_rename=%p",
                     ping, kPingRange, (void*)m.notify_visible.call,
                     (void*)m.notify_rename.call);
        } else {
            LOG_WARN("ping marker: beacon has no PingInstance in its hierarchy");
        }
    }
    m.created = true;
    LOG_INFO("ping marker: beacon prefab spawned go=%p transform=%p at %.1f,%.1f,%.1f", go,
             m.transform, position.x, position.y, position.z);
    return true;
}

// Removes beacons this mod spawned: they carry our label, so a player-placed beacon is
// never touched. One-shot, requested by the "cleanup" arm file.
void ping_marker_cleanup() {
    auto& m = g_ping_marker;
    if (!ping_marker_resolve()) return;
    if (!g_find_objects_all || !g_array_get_length || !g_array_get_value || !g_get_game_object ||
        !g_object_destroy || !m.id_field) {
        LOG_WARN("ping marker: cleanup api missing");
        return;
    }
    void* list = g_find_objects_all.call(m.ping_type_obj, true, g_find_objects_all.method);
    if (!list) return;
    int count = g_array_get_length.call(list, g_array_get_length.method);
    int removed = 0;
    int sampled = 0;
    for (int i = 0; i < count && i < 512; ++i) {
        if (removed >= 48) break;  // bounded per frame: 800 objects must not hitch the game
        void* ping = g_array_get_value.call(list, i, g_array_get_value.method);
        if (!ping) continue;
        void* label_field = m.id_field;
        // _label is not the id; resolve it once.
        if (!m.label_field) m.label_field = ping_field_of(m.ping_class, "_label");
        label_field = m.label_field;
        if (!label_field) continue;
        void* text = nullptr;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(ping), label_field, &text);
        if (!text) continue;
        char name[64] = {0};
        read_managed_name(text, name, sizeof(name));
        if (sampled < 8) {
            int32_t ptype = -1;
            if (m.type_field)
                il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(ping), m.type_field,
                                       &ptype);
            LOG_INFO("ping marker: cleanup sample [%d] label='%s' type=%d", i, name, ptype);
            ++sampled;
        }
        if (strcmp(name, "Напарник")) continue;
        void* go = g_get_game_object.call(ping, g_get_game_object.method);
        if (!go) continue;
        g_object_destroy.call(go, g_object_destroy.method);
        ++removed;
    }
    LOG_INFO("ping marker: cleanup pass removed %d mod beacons (of %d pings)", removed, count);
    if (removed == 0) {
        m.cleanup_requested = false;
        LOG_INFO("ping marker: cleanup finished");
    }
}

#include "player_hud_marker.inc"

void tick_player_marker(const LocalState& local) {
    auto& m = g_ping_marker;
    // The arm file is read before any world/role gate: an isolated test must be able to run
    // in solo, where no peer exists and the role note was never delivered.
    ping_marker_read_arm();
    const bool role_ready = local.valid && g_role_known && contents_world_ready() && world_operations_ready();
    tick_player_hud_marker(local,m.control.hud_enabled&&role_ready);
    if (!local.valid || !m.control.active()) return;
    if (!role_ready) {
        // Isolated paths: a live world is enough. The normal path still waits for the full
        // session/world gate. Cleanup must work in solo too, otherwise a test world cannot
        // be tidied up without a second player.
        const bool solo_allowed =
            m.allow_create_nitrox || m.allow_create_beacon || m.cleanup_requested;
        if (!solo_allowed || g_world_live_since <= 0.0) return;
    }
    if (m.control.probe_pending && !m.probe_done) {
        ping_marker_probe();
        if(m.probe_done)m.control.probe_pending=false;
    }
    if(!m.control.text_enabled)return;
    if (m.cleanup_requested) ping_marker_cleanup();
    PeerState peers[kMaxPeers];
    size_t count = net_copy_peers(peers, kMaxPeers);
    const PeerState* seen = nullptr;
    for (size_t i = 0; i < count; ++i) {
        if (!peers[i].seq_valid) continue;
        if (m.peer && peers[i].id == m.peer) {
            seen = &peers[i];
            break;
        }
        if (!seen) seen = &peers[i];
    }
    if (!seen) {
        // Isolated test path: neither the Nitrox sequence nor the beacon needs a peer, so a
        // marker can be created at the local position to check it on a single phone.
        Vector3 self{local.x, local.y, local.z + 3.0f};
        if (m.control.text_enabled && !m.created) {
            if (m.allow_create_nitrox) {
                if (ping_marker_create_nitrox() && m.transform)
                    g_set_position.call(m.transform, self, g_set_position.method);
            } else if (m.allow_create_beacon) {
                ping_marker_spawn_beacon(self);
            }
        }
        if (m.created) ++m.updates;
        return;
    }
    if (m.control.text_enabled && !m.created) {
        if (m.allow_create_nitrox) {
            if (!ping_marker_create_nitrox()) return;
            m.peer = seen->id;
        } else if (m.allow_create_beacon) {
            Vector3 position{seen->x, seen->y, seen->z};
            if (!ping_marker_spawn_beacon(position)) return;
            m.peer = seen->id;
        }
    }
    if (m.created && m.transform) {
        Vector3 position{seen->x, seen->y, seen->z};
        if (m.peer != seen->id) m.peer = seen->id;
        g_set_position.call(m.transform, position, g_set_position.method);
    }
    if (m.created) ++m.updates;
    // Text fallback, kept from the first version: five seconds, distance and bearing.
    double now = build_now();
    if (now < m.next_report) return;
    m.next_report = now + 5.0;
    float dx = seen->x - local.x, dy = seen->y - local.y, dz = seen->z - local.z;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(dist) || dist > 100000.0f) return;
    // view_yaw comes from Player.camAnchor, whose yaw runs opposite to the body/model yaw
    // used by atan2(dx, dz) - the same convention mismatch that needed the +180 turn fix in
    // drive_avatar_animator. Without it every side read backwards: the partner behind was
    // reported as "впереди" and left/right swapped with it.
    const float world_deg = atan2f(dx, dz) * 57.2957795f;
    const float marker_yaw = local.view_yaw + 180.0f;
    const auto direction = creature_motion::bearing(dx, dz, marker_yaw);
    const char* side = direction == creature_motion::Bearing::Ahead ? "впереди"
        : direction == creature_motion::Bearing::Right               ? "справа"
        : direction == creature_motion::Bearing::Behind              ? "сзади"
                                                                    : "слева";
    const char* height = dy > 2 ? ", выше" : dy < -2 ? ", ниже" : "";
    char text[256];
    snprintf(text, sizeof(text), "SNMP: напарник — %.0f м, %s%s", dist, side, height);
    motion_notify(text);
    LOG_INFO("player marker: peer=%08x dist=%.1f side=%s%s ping=%u updates=%llu "
             "world_deg=%.1f view_yaw=%.1f marker_yaw=%.1f",
             seen->id, dist, side, height, m.created ? 1u : 0u, m.updates, world_deg,
             local.view_yaw, marker_yaw);
}
