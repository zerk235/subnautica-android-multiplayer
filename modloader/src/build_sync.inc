// ---------------------------------------------------------------------------

#if defined(SNMOD_BASE_GEOMETRY_APPLY) && !defined(SNMOD_BASE_GEOMETRY_CAPTURE)
#error "The experimental apply path requires geometry capture and transport"
#endif
// Build sync (experimental v1)
//
// Crash-containment mode, not a base-geometry replication implementation.
// Existing Constructables receive constructedAmount only; _constructed, materials,
// behaviours and base geometry are deliberately left alone. Only a plain outdoor
// Constructable may be spawned, from a cached scene prefab or a completed, polled
// PrefabDatabase request.
// ConstructableBase creation is refused BEFORE Instantiate/Awake can run.
//
// Construct(), SetState(), UpdateMaterial(), prefab coroutines and runtime_invoke
// are never called here. All remaining IL2CPP calls stay on the game thread and
// carry the real MethodInfo. The host alone publishes build snapshots; a guest
// must not echo its deliberately incomplete visual/completion state back to it.
// ---------------------------------------------------------------------------

using BuildObjectFn = void* (*)(void*, void*);
using BuildStaticObjectFn = void* (*)(void*);
using BuildStaticBoolFn = bool (*)(void*);
using BuildClassIdFn = void* (*)(int32_t, void*);
using BuildPrefabFn = void* (*)(void*, int32_t, bool, void*);
using BuildSceneFn = bool (*)(void*, void**, void*);
using BuildAliveFn = bool (*)(void*, void*);
using BuildSetPositionFn = void (*)(void*, Vector3, void*);
// SetState(bool value, bool setAmount = true) - virtual slot 13, resolved on the object's
// real class at the call site so a ConstructableBase lands on its override.
using BuildSetStateFn = uint8_t (*)(void* self, uint8_t value, uint8_t set_amount, void* method);
// LinkModule(Nullable<Base.Face>): the nullable is 20 bytes, past the AAPCS by-value
// ceiling, so il2cpp hands it through an invisible reference.
using BuildLinkModuleFn = void (*)(void* self, const void* module_face, void* method);

constexpr int kBuildKnownLimit = 512;
constexpr int kBuildJobLimit = 8;
constexpr double kBuildScanSeconds = 1.0;
constexpr double kBuildRepairSeconds = 5.0;
// A job that cannot apply yet (no scene prefab cache, no local Base for its face) holds
// for this long before it is dropped; the sender's repair snapshot re-feeds it meanwhile.
constexpr double kBuildJobDeadlineSeconds = 45.0;
constexpr double kBuildJobRetrySeconds = 1.0;
// A vanished advertised piece is only reported as removed when it disappeared well inside
// the player's streaming range, twice in a row. Cell streaming unloads distant pieces all
// the time - that absence is not a deconstruction, and reporting it would destroy the
// piece on every other phone.
constexpr float kBuildRemoveRadius = 60.0f;
constexpr int kBuildRemoveVotes = 2;

struct BuildKnown {
    bool used;
    bool advertised;
    WorldEvent state;
    double sent_at;
    uint32_t last_seen_scan;
    uint8_t absent_votes;
};

struct BuildJob {
    bool used;
    WorldEvent state;
    double deadline;
    double next_try;
};

BuildKnown g_build_known[kBuildKnownLimit]{};
BuildJob g_build_jobs[kBuildJobLimit]{};
uint32_t g_build_scan_id = 0;

struct Il2CppClass* g_build_class = nullptr;
void* g_build_type = nullptr;
void* g_build_amount_field = nullptr;
void* g_build_tech_field = nullptr;
void* g_build_complete_field = nullptr;
void* g_build_inside_field = nullptr;
void* g_build_new_field = nullptr;
void* g_build_attached_field = nullptr;
bool g_build_attempted = false;
bool g_build_enabled = false;
bool g_build_baseline = false;
bool g_build_resync = false;
bool g_build_capacity_failed = false;
double g_build_next_scan = 0.0;
double g_build_place_until = 0.0;
int32_t g_build_place_tech = 0;

// Anchor (v2) API for ConstructableBase modules. Offsets are the dump's for this build;
// a mismatch leaves the field null and base spawning stays off while outdoor sync lives.
struct Il2CppClass* g_build_base_class = nullptr;
bool g_build_base_enabled = false;
void* g_build_proto_field = nullptr;            // int protoVersion 0xD4
void* g_build_face_link_type_field = nullptr;   // TechType faceLinkedModuleType 0xD8
void* g_build_face_link_pos_field = nullptr;    // Vector3 faceLinkedModulePosition 0xDC
void* g_build_module_face_field = nullptr;      // Nullable<Base.Face> moduleFace 0xE8
void* g_build_rotatable_field = nullptr;        // bool rotatableBasePiece 0xFC
void* g_build_force_upright_field = nullptr;    // bool forceUpright 0x72 (Constructable)
void* g_build_base_geometry_type = nullptr;     // reflection Type of Base, existence probe
// OnGlobalEntitiesLoaded - the save-load path's own finisher, the fallback when SetState
// reports it changed nothing on a freshly anchored module.
Entry<ManagedFn> g_build_global_entities_loaded;

Entry<BuildClassIdFn> g_build_class_id;
Entry<BuildSceneFn> g_build_scene_prefab;
Entry<BuildPrefabFn> g_build_instantiate;
Entry<BuildSetPositionFn> g_build_set_position;
Entry<BuildStaticBoolFn> g_build_is_placing;
Entry<BuildStaticObjectFn> g_build_ghost;
using BuildTechFn = int32_t (*)(void*);
Entry<BuildTechFn> g_build_last_tech;
Entry<BuildAliveFn> g_build_alive;
// il2cpp_gchandle_free crashed inside libil2cpp on this game build immediately after
// UniqueIdentifier.set_Id. Keep these short ID strings pinned until process exit, matching
// the already-proven inventory/avatar string path, and bound the total to the known table.
uint32_t g_build_id_roots[kBuildKnownLimit]{};
size_t g_build_id_root_count = 0;

// The assigned managed field normally roots the string, but retain the pinned handle as
// well: freeing it here crashed in il2cpp_gchandle_free on the supported game build.
bool build_keep_id_root(unsigned int root) {
    if (!root || g_build_id_root_count >= kBuildKnownLimit) return false;
    g_build_id_roots[g_build_id_root_count++] = root;
    return true;
}

double build_now() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + ts.tv_nsec * 1e-9;
}

struct Il2CppClass* build_class(const char* name) {
    struct Il2CppClass* c = find_class_in_domain("", name);
    return c ? c : find_class_in_domain("UWE", name);
}

// Shared managed_hook prefab loader: returns a ready scene/database prefab or starts and
// polls the existing game-thread PrefabDatabase request path for this TechType.
void* find_prefab_source(uint32_t tech);

// InstantiateFromPrefab overloads are distinguished by the first parameter type,
// not just the argument count.
bool build_parameter_is(struct Il2CppMethodInfo* m, const char* expected) {
    const void* t = il2cpp_method_get_param(m, 0);
    void* type = t ? il2cpp_type_get_object(t) : nullptr;
    if (!type) return false;
    struct Il2CppClass* c = il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(type));
    Entry<BuildObjectFn> full_name;
    full_name.bind(find_method(c, "get_FullName", 0));
    if (!full_name) return false;
    char text[128];
    return string_to_ascii(full_name.call(type, full_name.method), text, sizeof(text)) &&
           strcmp(text, expected) == 0;
}

struct Il2CppMethodInfo* build_typed_method(struct Il2CppClass* c, const char* name,
                                            unsigned int argc, const char* parameter0) {
    if (!c) return nullptr;
    void* it = nullptr;
    for (struct Il2CppMethodInfo* m = il2cpp_class_get_methods(c, &it); m;
         m = il2cpp_class_get_methods(c, &it)) {
        const char* n = il2cpp_method_get_name(m);
        if (n && strcmp(n, name) == 0 && il2cpp_method_get_param_count(m) == argc &&
            build_parameter_is(m, parameter0)) {
            return m;
        }
    }
    return nullptr;
}

// The offsets come from the dumper for this exact build. A field found under another
// name or offset means the layout moved, and every read below would be reading a
// neighbour instead.
void* build_field_on(struct Il2CppClass* klass, const char* name, int expected_offset) {
    if (!klass) return nullptr;
    void* it = nullptr;
    for (struct Il2CppFieldInfo* f = il2cpp_class_get_fields(klass, &it); f;
         f = il2cpp_class_get_fields(klass, &it)) {
        const char* n = il2cpp_field_get_name(f);
        if (n && strcmp(n, name) == 0) {
            if (il2cpp_field_get_offset(f) == expected_offset) return f;
            LOG_WARN("build: field %s is at offset %d, expected %d - build sync stays off",
                     name, il2cpp_field_get_offset(f), expected_offset);
            return nullptr;
        }
    }
    return nullptr;
}

void* build_field(const char* name, int expected_offset) {
    return build_field_on(g_build_class, name, expected_offset);
}

int build_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool build_class_bytes(void* text, uint8_t out[16]) {
    char s[40];
    if (!string_to_ascii(text, s, sizeof(s))) return false;
    size_t len = strlen(s);
    if (len != 32 && len != 36) return false;
    int nibble = 0;
    memset(out, 0, 16);
    for (size_t i = 0; i < len; ++i) {
        if (len == 36 && (i == 8 || i == 13 || i == 18 || i == 23)) {
            if (s[i] != '-') return false;
            continue;
        }
        int h = build_hex(s[i]);
        if (h < 0 || nibble >= 32) return false;
        out[nibble / 2] |= static_cast<uint8_t>(h << ((nibble % 2) ? 0 : 4));
        ++nibble;
    }
    return nibble == 32;
}

void resolve_build_api() {
    g_build_attempted = true;
    if (!il2cpp_object_get_class || !il2cpp_class_get_methods || !il2cpp_method_get_name ||
        !il2cpp_method_get_param || !il2cpp_method_get_param_count ||
        !il2cpp_class_get_parent || !il2cpp_class_get_name ||
        !il2cpp_class_get_type || !il2cpp_type_get_object || !il2cpp_class_get_fields ||
        !il2cpp_field_get_name || !il2cpp_field_get_offset || !il2cpp_field_get_value ||
        !il2cpp_field_set_value || !il2cpp_gchandle_new || !il2cpp_string_new ||
        !g_get_component || !g_get_game_object || !g_gameobject_get_transform ||
        !g_unique_get_id || !g_uid_set_id || !g_unique_identifier_type ||
        !g_find_objects_all || !g_array_get_length || !g_array_get_value ||
        !g_get_position || !g_get_rotation || !g_set_rotation || !g_object_destroy) {
        LOG_WARN("build: disabled, runtime or item API missing");
        return;
    }

    g_build_class = build_class("Constructable");
    struct Il2CppClass* craft = build_class("CraftData");
    struct Il2CppClass* scene = build_class("ScenePrefabDatabase");
    struct Il2CppClass* builder = build_class("Builder");
    struct Il2CppClass* object = find_class_in_domain("UnityEngine", "Object");
    struct Il2CppClass* transform = find_class_in_domain("UnityEngine", "Transform");
    if (!g_build_class) {
        LOG_WARN("build: disabled, Constructable not found");
        return;
    }
    g_build_type = il2cpp_type_get_object(il2cpp_class_get_type(g_build_class));

    g_build_class_id.bind(find_method(craft, "GetClassIdForTechType", 1));
    g_build_scene_prefab.bind(find_method(scene, "TryGetScenePrefab", 2));
    g_build_instantiate.bind(
        build_typed_method(craft, "InstantiateFromPrefab", 3, "UnityEngine.GameObject"));
    g_build_alive.bind(find_method(object, "op_Implicit", 1));
    g_build_set_position.bind(find_method(transform, "set_position", 1));
    g_build_is_placing.bind(find_method(builder, "get_isPlacing", 0));
    g_build_ghost.bind(find_method(builder, "GetGhostModel", 0));
    g_build_last_tech.bind(find_method(builder, "get_lastTechType", 0));
    g_build_amount_field = build_field("constructedAmount", 0x30);
    g_build_tech_field = build_field("techType", 0x34);
    g_build_complete_field = build_field("_constructed", 0x2C);
    g_build_inside_field = build_field("isInside", 0x39);
    g_build_new_field = build_field("isNew", 0x38);
    g_build_attached_field = build_field("attachedToBase", 0x74);

    g_build_enabled = g_build_type && g_build_class_id &&
                      g_build_scene_prefab && g_build_instantiate &&
                      g_build_alive && g_build_set_position && g_build_is_placing &&
                      g_build_ghost && g_build_last_tech &&
                      g_build_amount_field && g_build_tech_field && g_build_complete_field &&
                      g_build_inside_field && g_build_new_field && g_build_attached_field;

    // Anchor (v2) API. Every piece of it is verified against the dump offset, so a moved
    // layout disables base modules but leaves the v1 outdoor path alive.
    g_build_base_class = build_class("ConstructableBase");
    if (g_build_base_class) {
        g_build_proto_field = build_field_on(g_build_base_class, "protoVersion", 0xD4);
        g_build_face_link_type_field =
            build_field_on(g_build_base_class, "faceLinkedModuleType", 0xD8);
        g_build_face_link_pos_field =
            build_field_on(g_build_base_class, "faceLinkedModulePosition", 0xDC);
        g_build_module_face_field = build_field_on(g_build_base_class, "moduleFace", 0xE8);
        g_build_rotatable_field = build_field_on(g_build_base_class, "rotatableBasePiece", 0xFC);
        g_build_force_upright_field = build_field_on(g_build_class, "forceUpright", 0x72);
        struct Il2CppClass* base_geometry = build_class("Base");
        g_build_base_geometry_type =
            base_geometry ? il2cpp_type_get_object(il2cpp_class_get_type(base_geometry)) : nullptr;
        g_build_global_entities_loaded.bind(find_method(g_build_base_class,
                                                        "OnGlobalEntitiesLoaded", 0));
        g_build_base_enabled =
            g_build_proto_field && g_build_face_link_type_field && g_build_face_link_pos_field &&
            g_build_module_face_field && g_build_rotatable_field && g_build_force_upright_field &&
            g_build_base_geometry_type && g_build_global_entities_loaded;
    }

    LOG_INFO("build: api %s mode=amount-only spawn=scene-cache+polled-database "
             "base-anchor=%s "
             "event=%zu bytes scene_method=%p instantiate_method=%p",
             g_build_enabled ? "ready" : "DISABLED", g_build_base_enabled ? "ready" : "off",
             sizeof(WorldEvent),
             (void*)g_build_scene_prefab.method, (void*)g_build_instantiate.method);
}

// A managed reference stays valid after Unity has destroyed the object, so a plain
// null test says nothing. Object.op_Implicit is the engine's own answer to that.
bool build_live(void* object) {
    return object != nullptr && g_build_alive.call(object, g_build_alive.method);
}

void* build_uid(void* component) {
    if (!build_live(component)) return nullptr;
    void* go = g_get_game_object.call(component, g_get_game_object.method);
    return go ? g_get_component.call(go, g_unique_identifier_type, g_get_component.method)
              : nullptr;
}

// Keep production off until this containment path has been tested on both phones.
// Enable explicitly for the test build; this is NOT full base-module replication.
constexpr bool kBuildSyncEnabled = true;

char g_build_refuse[160] = "";

// Inheritance is validated before any Constructable field is accessed. Existing
// derived components may receive amount-only updates; they may NOT be spawned.
bool build_derives_from(void* component) {
    struct Il2CppClass* c = il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(component));
    for (int depth = 0; c && depth < 32; depth++) {
        if (c == g_build_class) return true;
        c = il2cpp_class_get_parent(c);
    }
    return false;
}

bool build_supported(void* component) {
    if (!build_live(component)) {
        snprintf(g_build_refuse, sizeof(g_build_refuse), "object is not alive");
        return false;
    }
    if (!build_derives_from(component)) {
        struct Il2CppClass* c0 =
            il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(component));
        const char* name = c0 ? il2cpp_class_get_name(c0) : nullptr;
        snprintf(g_build_refuse, sizeof(g_build_refuse), "class '%s' is not a Constructable",
                 name ? name : "?");
        return false;
    }
    return true;
}

// Stronger checks apply to NEW instances. A plain outdoor piece stays the exact-Constructable
// check of v1. An anchored event may be a ConstructableBase module: its anchors travel in the
// event and are written before the state transition, which is the v2 order the base geometry
// needs. Any other subclass stays refused - its initialization is unknown to us.
// Do not change the shared item-sync helpers or the existing ghost scan guard.
bool build_spawn_supported(void* component, bool anchored) {
    if (!build_supported(component)) return false;
    struct Il2CppClass* cls =
        il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(component));
    if (cls == g_build_class) {
        bool inside = false;
        bool attached = false;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(component),
                               g_build_inside_field, &inside);
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(component),
                               g_build_attached_field, &attached);
        if (inside || attached) {
            snprintf(g_build_refuse, sizeof(g_build_refuse),
                     "prefab needs interior/base attachment (inside=%u attached=%u)",
                     static_cast<unsigned>(inside), static_cast<unsigned>(attached));
            return false;
        }
        return true;
    }
    if (anchored && g_build_base_enabled && cls == g_build_base_class) {
        return true;
    }
    snprintf(g_build_refuse, sizeof(g_build_refuse),
             "class '%s' requires unsupported base/subclass initialization (anchored=%u)",
             il2cpp_class_get_name(cls), static_cast<unsigned>(anchored));
    return false;
}

// The reason the last object was refused, so a scan that finds nothing says why instead
// of just being silent.
bool build_capture(void* component, WorldEvent* out) {
    if (!build_supported(component)) {
        return false;
    }
    void* uid = build_uid(component);
    if (!uid) {
        snprintf(g_build_refuse, sizeof(g_build_refuse), "no UniqueIdentifier component");
        return false;
    }
    if (!string_to_ascii(g_unique_get_id.call(uid, g_unique_get_id.method), out->object_id,
                         sizeof(out->object_id))) {
        snprintf(g_build_refuse, sizeof(g_build_refuse), "Id is not ascii (len>127?)");
        return false;
    }
    guid_from_string(out->object_id, out->guid);
    struct Il2CppObject* o = reinterpret_cast<Il2CppObject*>(component);
    il2cpp_field_get_value(o, g_build_tech_field, &out->tech_type);
    il2cpp_field_get_value(o, g_build_amount_field, &out->constructed_amount);
    bool complete = false;
    il2cpp_field_get_value(o, g_build_complete_field, &complete);
    out->build_flags = complete ? kWorldBuildComplete : 0;
    void* go = g_get_game_object.call(component, g_get_game_object.method);
    void* tr = go ? g_gameobject_get_transform.call(go, g_gameobject_get_transform.method)
                  : nullptr;
    if (!tr || out->tech_type <= 0 || !std::isfinite(out->constructed_amount) ||
        out->constructed_amount < 0 || out->constructed_amount > 1) {
        snprintf(g_build_refuse, sizeof(g_build_refuse), "no transform or bad techType/amount");
        return false;
    }
    Vector3 p = g_get_position.call(tr, g_get_position.method);
    Quaternion q = g_get_rotation.call(tr, g_get_rotation.method);
    out->x = p.x; out->y = p.y; out->z = p.z;
    out->qx = q.x; out->qy = q.y; out->qz = q.z; out->qw = q.w;

    // Anchor tail, only for exact ConstructableBase modules: these are the serialized
    // fields the game itself keeps and resolves on load (dump.cs 11474). A derived class
    // that is not exactly ConstructableBase stays amount-only, as before.
    if (g_build_base_enabled &&
        il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(component)) ==
            g_build_base_class) {
        int32_t proto = 0;
        int32_t link_type = 0;
        Vector3 link_pos{};
        uint8_t face[20]{};
        bool rotatable = false;
        bool force_upright = false;
        bool inside = false;
        bool attached = false;
        il2cpp_field_get_value(o, g_build_proto_field, &proto);
        il2cpp_field_get_value(o, g_build_face_link_type_field, &link_type);
        il2cpp_field_get_value(o, g_build_face_link_pos_field, &link_pos);
        il2cpp_field_get_value(o, g_build_module_face_field, face);
        il2cpp_field_get_value(o, g_build_rotatable_field, &rotatable);
        il2cpp_field_get_value(o, g_build_force_upright_field, &force_upright);
        il2cpp_field_get_value(o, g_build_inside_field, &inside);
        il2cpp_field_get_value(o, g_build_attached_field, &attached);
        out->proto_version = proto;
        out->face_linked_module_type = link_type;
        out->face_linked_position[0] = link_pos.x;
        out->face_linked_position[1] = link_pos.y;
        out->face_linked_position[2] = link_pos.z;
        // IL2CPP lays Nullable<T> out as hasValue first, then T at the next
        // aligned offset. Base.Face is 16 bytes (Int3 + Direction), so its
        // value starts at +4 and Nullable<Base.Face> occupies 20 bytes.
        out->face_has_value = face[0];
        memcpy(out->face_cell, face + 4, sizeof(out->face_cell));
        memcpy(&out->face_direction, face + 16, sizeof(out->face_direction));
        out->build_flags |= static_cast<uint8_t>(kWorldBuildAnchored |
                                                 (inside ? kWorldBuildIsInside : 0) |
                                                 (attached ? kWorldBuildAttached : 0) |
                                                 (force_upright ? kWorldBuildForceUpright : 0) |
                                                 (rotatable ? kWorldBuildRotatable : 0));
    }
    return true;
}

#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
// Field offsets are checked against the supported game's metadata before any access.
struct GhostCaptureApi {
    bool attempted = false;
    bool ready = false;
    void *model = nullptr, *ghost_type = nullptr, *ghost_base = nullptr, *target = nullptr, *offset = nullptr;
    void *shape = nullptr, *cell_offset = nullptr, *anchor = nullptr;
    void *faces = nullptr, *cells = nullptr, *links = nullptr, *masks = nullptr, *glass = nullptr;
};
GhostCaptureApi g_ghost_capture;

bool build_resolve_ghost_capture() {
    auto& a = g_ghost_capture;
    if (a.attempted) return a.ready;
    a.attempted = true;
    auto* base = build_class("Base");
    auto* ghost = build_class("BaseGhost");
    if (!base || !ghost || !g_build_base_geometry_type || g_get_component_is_string) return false;
    const void* ghost_il2cpp_type = il2cpp_class_get_type(ghost);
    if (!ghost_il2cpp_type) return false;
    a.ghost_type = il2cpp_type_get_object(ghost_il2cpp_type);
    a.model = build_field_on(g_build_class, "model", 0x40);
    a.ghost_base = build_field_on(ghost, "ghostBase", 0x30);
    a.target = build_field_on(ghost, "targetBase", 0x38);
    a.offset = build_field_on(ghost, "targetOffset", 0x20);
    a.shape = build_field_on(base, "baseShape", 0x90);
    a.faces = build_field_on(base, "faces", 0xA0);
    a.cells = build_field_on(base, "cells", 0xA8);
    a.links = build_field_on(base, "links", 0xB0);
    a.cell_offset = build_field_on(base, "cellOffset", 0xB8);
    a.masks = build_field_on(base, "masks", 0xC8);
    a.glass = build_field_on(base, "isGlass", 0xD0);
    a.anchor = build_field_on(base, "anchor", 0xD8);
    a.ready = a.ghost_type && a.model && a.ghost_base && a.target && a.offset && a.shape && a.faces &&
        a.cells && a.links && a.cell_offset && a.masks && a.glass && a.anchor;
    LOG_INFO("build: geometry capture %s", a.ready ? "ready" : "unavailable");
    return a.ready;
}

#include "base_topology_capture.inc"

bool build_capture_geometry(void* component, base_geometry::Snapshot* snapshot,
                            int32_t target_offset[3], int32_t target_anchor[3],
                            char* target_id, size_t target_id_size,
                            const char** ghost_class,int32_t recipe=0) {
    if (!component || !snapshot || !target_offset || !target_anchor || !target_id || !target_id_size ||
        !ghost_class || !build_resolve_ghost_capture()) return false;
    auto& a = g_ghost_capture;
    void* model = nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(component), a.model, &model);
    if (!build_live(model)) { LOG_WARN("build: geometry capture reason=model-not-live"); return false; }
    void* geometry = g_get_component.call(model, g_build_base_geometry_type, g_get_component.method);
    void* ghost = g_get_component.call(model, a.ghost_type, g_get_component.method);
    if (!build_live(geometry) || !build_live(ghost)) {
        LOG_WARN("build: geometry capture reason=components base=%d ghost=%d",
                 int(build_live(geometry)), int(build_live(ghost))); return false;
    }
    auto* geometry_obj = reinterpret_cast<Il2CppObject*>(geometry);
    auto* ghost_obj = reinterpret_cast<Il2CppObject*>(ghost);
    void* linked_geometry = nullptr;
    il2cpp_field_get_value(ghost_obj, a.ghost_base, &linked_geometry);
    if (linked_geometry != geometry) { LOG_WARN("build: geometry capture reason=ghost-base-mismatch linked-live=%d", int(build_live(linked_geometry))); return false; }
    int32_t shape[4]{}; // Grid3Shape: x, y, z, xy
    il2cpp_field_get_value(geometry_obj, a.shape, shape);
    base_geometry::Snapshot result;
    result.shape = {shape[0], shape[1], shape[2]};
    size_t count;
    if (!base_geometry::cell_count(result.shape, &count) ||
        int64_t(shape[0]) * int64_t(shape[1]) != shape[3]) {
        LOG_WARN("build: geometry capture reason=shape shape=%d,%d,%d xy=%d", shape[0], shape[1], shape[2], shape[3]); return false;
    }
    il2cpp_field_get_value(geometry_obj, a.cell_offset, result.cell_offset.data());
    il2cpp_field_get_value(geometry_obj, a.anchor, result.anchor.data());
    il2cpp_field_get_value(ghost_obj, a.offset, target_offset);
    void* target = nullptr;
    il2cpp_field_get_value(ghost_obj, a.target, &target);
    target_id[0] = 0;
    target_anchor[0] = target_anchor[1] = target_anchor[2] = 0;
    if (build_live(target)) {
        void* uid = build_uid(target);
        if (!uid || !string_to_ascii(g_unique_get_id.call(uid, g_unique_get_id.method),
                                     target_id, target_id_size)) {
            LOG_WARN("build: geometry capture reason=target-id uid=%d", int(uid != nullptr)); return false;
        }
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(target), a.anchor, target_anchor);
    }
    const char* actual_class=il2cpp_class_get_name(il2cpp_object_get_class(ghost_obj));
    const bool foundation=recipe==5509;
    const bool room=recipe==5500;
    if ((foundation || room) && (!actual_class || (strcmp(actual_class,"BaseAddCellGhost") && strcmp(actual_class,"BaseGhost")))) return false;
    if (!build_copy_topology_arrays(geometry,&result,foundation,room)) return false;
    *ghost_class = il2cpp_class_get_name(il2cpp_object_get_class(ghost_obj));
    *snapshot = std::move(result);
    return *ghost_class != nullptr;
}
#endif

#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
#include "base_corridor_registry.inc"
#endif

BuildKnown* build_known(const WorldEvent& state) {
    for (size_t i = 0; i < sizeof(g_build_known) / sizeof(BuildKnown); i++) {
        if (g_build_known[i].used && strcmp(g_build_known[i].state.object_id, state.object_id) == 0) {
            return &g_build_known[i];
        }
    }
    for (size_t i = 0; i < sizeof(g_build_known) / sizeof(BuildKnown); i++) {
        if (!g_build_known[i].used) {
            g_build_known[i].used = true;
            g_build_known[i].state = state;
            return &g_build_known[i];
        }
    }
    if (!g_build_capacity_failed) LOG_WARN("build: known table full, further builds ignored");
    g_build_capacity_failed = true;
    return nullptr;
}

bool build_payload_valid(const WorldEvent& e) {
    constexpr uint8_t kBuildFlagMask = kWorldBuildComplete | kWorldBuildIsInside |
                                       kWorldBuildForceUpright | kWorldBuildRotatable |
                                       kWorldBuildAttached | kWorldBuildAnchored
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
                                       | kWorldBuildHasGeometry
#endif
                                       ;
    if (!memchr(e.object_id, '\0', sizeof(e.object_id)) || !e.object_id[0] || e.tech_type <= 0 ||
        (e.build_flags & ~kBuildFlagMask) != 0 ||
        !std::isfinite(e.constructed_amount) || e.constructed_amount < 0 ||
        e.constructed_amount > 1) {
        return false;
    }
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
    if ((e.build_flags & kWorldBuildHasGeometry) && !(e.build_flags & kWorldBuildAnchored))
        return false;
#endif
    for (const char* p = e.object_id; *p; ++p) {
        if (static_cast<unsigned char>(*p) >= 128) return false;
    }
    // The Id and its hash must agree, otherwise the sender is talking about a
    // different object than the one its guid addresses.
    uint8_t hash[16];
    guid_from_string(e.object_id, hash);
    if (memcmp(hash, e.guid, 16) != 0) return false;
    const float pose[] = {e.x, e.y, e.z, e.qx, e.qy, e.qz, e.qw};
    for (float f : pose) {
        if (!std::isfinite(f)) return false;
    }
    if (e.build_flags & kWorldBuildAnchored) {
        const float tail[] = {e.face_linked_position[0], e.face_linked_position[1],
                              e.face_linked_position[2]};
        for (float f : tail) {
            if (!std::isfinite(f)) return false;
        }
        if (e.face_has_value > 1) return false;
    }
    float norm = e.qx * e.qx + e.qy * e.qy + e.qz * e.qz + e.qw * e.qw;
    return norm > 0.9f && norm < 1.1f &&
           ((e.build_flags & kWorldBuildComplete) == 0 || e.constructed_amount > 0.999f);
}

// Both kinds are full snapshots rather than deltas, which is what makes a lost event
// survivable: the next scan sends the state again and the receiver converges instead
// of drifting. A progress that overtakes a lost build therefore still creates it.
void queue_remote_build(const WorldEvent& e) {
    if (!kBuildSyncEnabled) return;
    if (!build_payload_valid(e)) {
        LOG_WARN("build: refused, payload failed validation");
        return;
    }
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
    if (e.build_flags & kWorldBuildAnchored) {
        if (e.kind == kWorldEventRemove) {
            if (!(e.build_flags & kWorldBuildHasGeometry) ||
                (base_geometry::piece_cell_type(e.tech_type) < 0 && base_geometry::face_type_for_tech(e.tech_type)<0 && e.tech_type!=5504) || e.face_has_value) return;
            auto* entry = build_synced_corridor(e.object_id, true);
            if (!entry) { LOG_WARN("build: removal identity table full"); return; }
            entry->removed = true; // suppress late build repairs while geometry is in flight
        } else if (build_corridor_removed(e.object_id)) {
            return;
        }
    }
#endif
    for (size_t i = 0; i < sizeof(g_build_jobs) / sizeof(BuildJob); i++) {
        BuildJob& j = g_build_jobs[i];
        if (j.used && strcmp(j.state.object_id, e.object_id) == 0) {
            if (j.state.tech_type != e.tech_type || memcmp(j.state.class_id, e.class_id, 16)) {
                LOG_WARN("build: refused, same Id with a different prefab");
                return;
            }
            j.state = e;
            return;
        }
    }
    for (size_t i = 0; i < sizeof(g_build_jobs) / sizeof(BuildJob); i++) {
        BuildJob& j = g_build_jobs[i];
        if (!j.used) {
            j = BuildJob{};
            j.used = true;
            j.state = e;
            j.deadline = build_now() + kBuildJobDeadlineSeconds;
            j.next_try = 0.0;
            return;
        }
    }
    LOG_WARN("build: refused, pending table full");
}

void build_finish_job(BuildJob& j) {
    j = BuildJob{};
}

void* build_find(const WorldEvent& e) {
    void* list = g_find_objects_all.call(g_build_type, true, g_find_objects_all.method);
    if (!list) return nullptr;
    int32_t n = g_array_get_length.call(list, g_array_get_length.method);
    for (int32_t i = 0; i < n; ++i) {
        void* c = g_array_get_value.call(list, i, g_array_get_value.method);
        WorldEvent state{};
        if (build_capture(c, &state) && strcmp(state.object_id, e.object_id) == 0) return c;
    }
    return nullptr;
}

bool build_apply_state(void* c, const WorldEvent& e) {
    if (!build_payload_valid(e)) return false;
    LOG_INFO("build: stage=apply-begin id=%s obj=%p origin=%08x seq=%u",
             e.object_id, c, e.origin_id, e.sequence);
    WorldEvent state{};
    if (!build_capture(c, &state) || state.tech_type != e.tech_type ||
        strcmp(state.object_id, e.object_id) != 0) return false;
    struct Il2CppClass* cls = il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(c));
    ++g_applying_remote;
    float amount = e.constructed_amount;
    LOG_INFO("build: stage=amount-before id=%s class=%s old=%.3f new=%.3f",
             e.object_id, il2cpp_class_get_name(cls), state.constructed_amount, amount);
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(c), g_build_amount_field, &amount);
    --g_applying_remote;
    // Do NOT write _constructed or call SetState/UpdateMaterial. A float write is
    // data replication, not completion: renderers, colliders and behaviours may
    // remain unchanged. Existing base transforms are also deliberately untouched.
    if (!build_capture(c, &state) ||
        std::fabs(state.constructed_amount - amount) > 0.0001f) {
        LOG_WARN("build: stage=amount-readback-failed id=%s", e.object_id);
        return false;
    }
    BuildKnown* k = build_known(state);
    if (k) {
        k->state = state;
        k->advertised = false;
    }
    LOG_INFO("build: stage=apply-done id=%s amount=%.3f complete_rx=%u complete_local=%u "
             "mode=amount-only pose=unchanged origin=%08x seq=%u", e.object_id, amount,
             static_cast<unsigned>((e.build_flags & kWorldBuildComplete) != 0),
             static_cast<unsigned>((state.build_flags & kWorldBuildComplete) != 0),
             e.origin_id, e.sequence);
    return true;
}

// Writes the v2 anchor fields on a freshly instantiated ConstructableBase module. All of
// them are plain data writes at dump-verified offsets; the game's own transitions consume
// them afterwards. The Nullable<Base.Face> is laid out as hasValue at +0, padding
// through +3, then the 16-byte Base.Face value at +4.
void build_write_anchors(void* c, const WorldEvent& e) {
    struct Il2CppObject* o = reinterpret_cast<Il2CppObject*>(c);
    int32_t proto = e.proto_version;
    int32_t link_type = e.face_linked_module_type;
    Vector3 link_pos{e.face_linked_position[0], e.face_linked_position[1],
                     e.face_linked_position[2]};
    bool rotatable = (e.build_flags & kWorldBuildRotatable) != 0;
    bool inside = (e.build_flags & kWorldBuildIsInside) != 0;
    bool attached = (e.build_flags & kWorldBuildAttached) != 0;
    bool force_upright = (e.build_flags & kWorldBuildForceUpright) != 0;
    bool is_new = false;
    uint8_t face[20]{};
    face[0] = e.face_has_value;
    memcpy(face + 4, e.face_cell, sizeof(e.face_cell));
    memcpy(face + 16, &e.face_direction, sizeof(e.face_direction));

    ++g_applying_remote;
    il2cpp_field_set_value(o, g_build_proto_field, &proto);
    il2cpp_field_set_value(o, g_build_face_link_type_field, &link_type);
    il2cpp_field_set_value(o, g_build_face_link_pos_field, &link_pos);
    il2cpp_field_set_value(o, g_build_module_face_field, face);
    il2cpp_field_set_value(o, g_build_rotatable_field, &rotatable);
    il2cpp_field_set_value(o, g_build_inside_field, &inside);
    il2cpp_field_set_value(o, g_build_attached_field, &attached);
    il2cpp_field_set_value(o, g_build_force_upright_field, &force_upright);
    il2cpp_field_set_value(o, g_build_new_field, &is_new);
    --g_applying_remote;
}

// Completes an anchored module: amount write, then the game's own transition on the
// object's REAL class, then the face link. SetState is the one risky call in this whole
// path - the ConstructableBase override binds the piece into the base and rebuilds the
// geometry. Mitigations: anchors are already on the object, setAmount=false keeps other
// people's resources untouched, and the fallback is the save-load finisher itself.
bool build_finish_anchor(void* c, const WorldEvent& e) {
    if (!build_payload_valid(e)) return false;
    struct Il2CppClass* real = il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(c));

    float amount = e.constructed_amount;
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(c), g_build_amount_field, &amount);

    if (e.build_flags & kWorldBuildComplete) {
        Entry<BuildSetStateFn> set_state;
        set_state.bind(find_method(real, "SetState", 2));
        if (!set_state) {
            snprintf(g_build_refuse, sizeof(g_build_refuse), "no SetState on class '%s'",
                     il2cpp_class_get_name(real));
            return false;
        }
        LOG_INFO("build: stage=set-state id=%s class=%s", e.object_id,
                 il2cpp_class_get_name(real));
        uint8_t done = set_state.call(c, 1, 0, set_state.method);
        LOG_INFO("build: stage=set-state-done id=%s result=%u", e.object_id,
                 static_cast<unsigned>(done));
        if (!done && g_build_global_entities_loaded) {
            LOG_WARN("build: set-state reported no change, trying the save-load finisher id=%s",
                     e.object_id);
            g_build_global_entities_loaded.call(c, g_build_global_entities_loaded.method);
        }
        if (e.face_has_value) {
            Entry<BuildLinkModuleFn> link;
            link.bind(find_method(real, "LinkModule", 1));
            if (link) {
                uint8_t face[20]{};
                face[0] = e.face_has_value;
                memcpy(face + 4, e.face_cell, sizeof(e.face_cell));
                memcpy(face + 16, &e.face_direction, sizeof(e.face_direction));
                link.call(c, face, link.method);
                LOG_INFO("build: stage=link-module id=%s", e.object_id);
            }
        }
    }

    // Same bookkeeping as apply_state: the known table learns the piece, so repair
    // snapshots converge instead of re-applying forever.
    WorldEvent state{};
    if (build_capture(c, &state)) {
        BuildKnown* k = build_known(state);
        if (k) {
            k->state = state;
            k->advertised = false;
        }
    }
    LOG_INFO("build: anchored apply done id=%s complete=%u amount=%.3f", e.object_id,
             static_cast<unsigned>((e.build_flags & kWorldBuildComplete) != 0),
             e.constructed_amount);
    return true;
}

void build_spawn(void* prefab, const WorldEvent& e) {
    if (!build_payload_valid(e) || !build_live(prefab)) return;
    if (g_build_id_root_count >= kBuildKnownLimit) {
        LOG_WARN("build: refused spawn id=%s, pinned-ID safety budget (%d) exhausted",
                 e.object_id, kBuildKnownLimit);
        return;
    }
    bool anchored = (e.build_flags & kWorldBuildAnchored) != 0 && g_build_base_enabled;
    LOG_INFO("build: stage=prefab-inspect id=%s prefab=%p anchored=%u", e.object_id, prefab,
             static_cast<unsigned>(anchored));
    void* source = g_get_component.call(prefab, g_build_type, g_get_component.method);
    if (!build_spawn_supported(source, anchored)) {
        LOG_WARN("build: refused spawn id=%s tech=%d reason=%s", e.object_id, e.tech_type,
                 g_build_refuse);
        return;
    }
    LOG_INFO("build: stage=instantiate-before id=%s prefab=%p method=%p origin=%08x seq=%u",
             e.object_id, prefab, (void*)g_build_instantiate.method, e.origin_id, e.sequence);
    void* go = g_build_instantiate.call(prefab, e.tech_type, false, g_build_instantiate.method);
    LOG_INFO("build: stage=instantiate-after id=%s go=%p", e.object_id, go);
    if (!build_live(go)) {
        LOG_WARN("build: refused, instantiate returned nothing");
        return;
    }
    void* c = g_get_component.call(go, g_build_type, g_get_component.method);
    void* uid = c ? build_uid(c) : nullptr;
    void* tr = g_gameobject_get_transform.call(go, g_gameobject_get_transform.method);
    bool ok = build_spawn_supported(c, anchored) && uid != nullptr && tr != nullptr;
    if (ok && anchored) build_write_anchors(c, e);
    if (ok) {
        void* text = il2cpp_string_new(e.object_id);
        unsigned int root =
            text ? il2cpp_gchandle_new(reinterpret_cast<Il2CppObject*>(text), true) : 0;
        ok = build_keep_id_root(root);
        if (ok) {
            // The host's exact Id, not a fresh local one: the world is addressed by
            // these ids, and renaming would orphan every saved reference to this piece.
            LOG_INFO("build: stage=uid-before id=%s method=%p", e.object_id,
                     (void*)g_uid_set_id.method);
            g_uid_set_id.call(uid, text, g_uid_set_id.method);
            LOG_INFO("build: stage=uid-after id=%s pinned_root=%u", e.object_id, root);
            Vector3 p{e.x, e.y, e.z};
            Quaternion q{e.qx, e.qy, e.qz, e.qw};
            LOG_INFO("build: stage=pose-before id=%s transform=%p", e.object_id, tr);
            g_build_set_position.call(tr, p, g_build_set_position.method);
            g_set_rotation.call(tr, q, g_set_rotation.method);
            LOG_INFO("build: stage=pose-after id=%s", e.object_id);
            ok = anchored ? build_finish_anchor(c, e) : build_apply_state(c, e);
        }
    }
    if (!ok) {
        g_object_destroy.call(go, g_object_destroy.method);
        LOG_WARN("build: refused, spawn setup failed, object removed again");
    } else {
        LOG_INFO("build: spawned id=%s tech=%d mode=%s", e.object_id, e.tech_type,
                 anchored ? "anchored" : "amount-only");
    }
}

// Deconstruction on the receiving side: SetState(false, false) on the object's real
// class runs the game's own deconstruction path (progress coroutine, base geometry
// rebuild). Resources are nobody's concern here - the phone that deconstructed had its
// own game return them through its normal Deconstruct flow. A pending job for the same
// Id is cancelled first: events arrive in order, so a remove that overtakes its own
// build job must win, not spawn the piece back.
void build_handle_remove(const WorldEvent& e) {
    if (!kBuildSyncEnabled) return;
    if (!build_payload_valid(e)) {
        LOG_WARN("build: remove refused, payload failed validation");
        return;
    }
    // A ConstructableBase can disappear from the Constructable scan while the game is
    // rebuilding/registering base geometry. Absence is not proof of player deconstruction;
    // until we have an explicit deconstruct hook, never let an inferred remove tear down a
    // room/corridor on another phone.
    if (e.build_flags & kWorldBuildAnchored) {
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
        if (e.build_flags & kWorldBuildHasGeometry) {
            queue_remote_build(e);
            return;
        }
#endif
        LOG_WARN("build: ignoring inferred remove for anchored Base module id=%s", e.object_id);
        return;
    }
    for (size_t i = 0; i < sizeof(g_build_jobs) / sizeof(BuildJob); i++) {
        BuildJob& j = g_build_jobs[i];
        if (j.used && strcmp(j.state.object_id, e.object_id) == 0) {
            LOG_INFO("build: remove id=%s cancels a pending job", e.object_id);
            build_finish_job(j);
        }
    }
    void* existing = build_find(e);
    if (!existing) {
        LOG_INFO("build: remove id=%s, not in scene (already gone or never arrived)", e.object_id);
    } else {
        struct Il2CppClass* real =
            il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(existing));
        Entry<BuildSetStateFn> set_state;
        set_state.bind(find_method(real, "SetState", 2));
        bool removed = false;
        if (set_state) {
            LOG_INFO("build: stage=remove-set-state id=%s class=%s", e.object_id,
                     il2cpp_class_get_name(real));
            removed = set_state.call(existing, 0, 0, set_state.method) != 0;
        }
        // A piece that was never completed has nothing to deconstruct: the state stays
        // false and the transition is a no-op. Only a plain outdoor piece may then be
        // destroyed outright; a base module stays for the base's own geometry pass.
        if (!removed && real == g_build_class) {
            void* go = g_get_game_object.call(existing, g_get_game_object.method);
            if (build_live(go)) {
                g_object_destroy.call(go, g_object_destroy.method);
                removed = true;
            }
        }
        LOG_INFO("build: remove id=%s done=%u", e.object_id, static_cast<unsigned>(removed));
    }
    for (size_t i = 0; i < sizeof(g_build_known) / sizeof(BuildKnown); i++) {
        BuildKnown& k = g_build_known[i];
        if (k.used && strcmp(k.state.object_id, e.object_id) == 0) {
            k = BuildKnown{};  // a future rebuild with the same Id re-publishes cleanly
        }
    }
}

// A face-anchored module needs a Base to bind into. Existence is probed, never guessed:
// holding the job until the base arrives is what keeps a module from instantiating into
// nothing. Foundations carry no face and skip this probe.
bool build_base_exists() {
    if (!g_build_base_geometry_type) return false;
    void* list = g_find_objects_all.call(g_build_base_geometry_type, true,
                                         g_find_objects_all.method);
    if (!list) return false;
    return g_array_get_length.call(list, g_array_get_length.method) > 0;
}

#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
base_geometry::GeometryCache g_geometry_cache;
uint8_t g_geometry_cache_session[16]{};
uint32_t g_geometry_cache_epoch = 0;
bool g_geometry_cache_session_valid = false;

#include "base_hatch_capture.inc"
#include "base_ladder_capture.inc"
#if defined(SNMOD_BASE_GEOMETRY_APPLY)
#include "base_geometry_apply.inc"
#include "base_hatch_apply.inc"
#include "base_ladder_apply.inc"
#endif
#include "base_build_completion.inc"

void build_refresh_geometry_cache_session() {
    uint8_t current[16]{};
    uint32_t epoch = 0;
    const bool valid = net_geometry_session(current, &epoch);
    if (!valid || !g_geometry_cache_session_valid || epoch != g_geometry_cache_epoch ||
        memcmp(current, g_geometry_cache_session, sizeof(current)) != 0) {
        g_geometry_cache.clear();
        for (auto& pending : g_completed_base_builds) pending = CompletedBaseBuild{};
        for (auto& entry : g_synced_corridors) entry = SyncedCorridor{};
#if defined(SNMOD_BASE_GEOMETRY_APPLY)
        for (auto& entry : g_applied_corridors) entry = AppliedCorridor{};
#endif
        g_geometry_cache_session_valid = valid;
        g_geometry_cache_epoch = epoch;
        memcpy(g_geometry_cache_session, current, sizeof(current));
    }
}
#endif

void build_pump_jobs() {
    // Events queued before role discovery wait for it. Both roles apply: the save stays
    // the host's (autosave), but a guest's placed piece is applied on the host too, so the
    // next host autosave writes it into the authoritative save.
    if (!g_role_known) return;
    double now = build_now();
    for (size_t i = 0; i < sizeof(g_build_jobs) / sizeof(BuildJob); i++) {
        BuildJob& j = g_build_jobs[i];
        if (!j.used) continue;
        if (now < j.next_try) continue;
        if (now > j.deadline) {
            LOG_WARN("build: job expired id=%s tech=%d, dropped after %.0f s of waiting",
                     j.state.object_id, j.state.tech_type, kBuildJobDeadlineSeconds);
            build_finish_job(j);
            continue;
        }
        LOG_INFO("build: stage=classid-before id=%s tech=%d origin=%08x seq=%u",
                 j.state.object_id, j.state.tech_type, j.state.origin_id, j.state.sequence);
        void* class_text = g_build_class_id.call(j.state.tech_type, g_build_class_id.method);
        uint8_t bytes[16];
        if (!build_class_bytes(class_text, bytes) || memcmp(bytes, j.state.class_id, 16)) {
            LOG_WARN("build: refused, TechType does not match the advertised classId");
            build_finish_job(j);
            continue;
        }
        LOG_INFO("build: stage=find-before id=%s", j.state.object_id);
        bool anchored = (j.state.build_flags & kWorldBuildAnchored) != 0;
#if defined(SNMOD_BASE_GEOMETRY_APPLY)
        if (anchored && j.state.kind == kWorldEventRemove) {
            const auto* removed=build_synced_corridor(j.state.object_id);
            if (removed && removed->removal_applied) { build_finish_job(j);continue; }
            const uint64_t fingerprint = uint64_t(j.state.geometry_hash_low) |
                (uint64_t(j.state.geometry_hash_high) << 32);
            const auto* geometry = g_geometry_cache.find(j.state.origin_id, j.state.object_id, fingerprint, now);
            if (!geometry) { j.next_try = now + kBuildJobRetrySeconds; continue; }
            void* target = build_corridor_target(geometry->target_base_id, geometry->target_base_anchor);
            if (!target) { j.next_try = now + kBuildJobRetrySeconds; continue; }
            const bool done=j.state.tech_type==5504 ? build_apply_ladder(target,j.state,*geometry) : base_geometry::face_type_for_tech(j.state.tech_type)>=0 ? build_apply_hatch(target,j.state,*geometry) :
                build_apply_corridor_removal(target, j.state, *geometry);
            if (done) build_finish_job(j);
            else j.next_try = now + kBuildJobRetrySeconds;
            continue;
        }
        if (anchored && build_corridor_removed(j.state.object_id)) {
            build_finish_job(j);
            continue;
        }
        if (anchored && j.state.tech_type==5504) {
            const uint64_t hash=uint64_t(j.state.geometry_hash_low) | (uint64_t(j.state.geometry_hash_high)<<32);
            const auto* g=g_geometry_cache.find(j.state.origin_id,j.state.object_id,hash,now);
            if (!g) { j.next_try=now+kBuildJobRetrySeconds;continue; }
            void* target=build_corridor_target(g->target_base_id,g->target_base_anchor);
            if (!target) { j.next_try=now+kBuildJobRetrySeconds;continue; }
            if (build_apply_ladder(target,j.state,*g)) build_finish_job(j);
            else j.next_try=now+kBuildJobRetrySeconds;
            continue;
        }
        if (anchored && base_geometry::face_type_for_tech(j.state.tech_type)>=0) {
            const uint64_t fingerprint=uint64_t(j.state.geometry_hash_low) | (uint64_t(j.state.geometry_hash_high)<<32);
            const auto* geometry=g_geometry_cache.find(j.state.origin_id,j.state.object_id,fingerprint,now);
            if (!geometry) { j.next_try=now+kBuildJobRetrySeconds; continue; }
            void* target=build_corridor_target(geometry->target_base_id,geometry->target_base_anchor);
            if (!target) { j.next_try=now+kBuildJobRetrySeconds; continue; }
            if (build_apply_hatch(target,j.state,*geometry)) build_finish_job(j);
            else j.next_try=now+kBuildJobRetrySeconds;
            continue;
        }
        if (anchored && build_corridor_record(j.state, false)) {
            // A previous attempt may already have changed the base, even if its final
            // readback was inconclusive. Never repeat it on a repair snapshot.
            build_finish_job(j);
            continue;
        }
#endif
        if (void* existing = build_find(j.state)) {
#if defined(SNMOD_BASE_GEOMETRY_APPLY)
            if (anchored) {
                LOG_WARN("build: existing base ghost id=%s requires reconciliation; not respawning",
                         j.state.object_id);
                build_finish_job(j);
                continue;
            }
#endif
            if (!build_apply_state(existing, j.state)) {
                LOG_WARN("build: refused, existing object would not accept the state");
            }
            build_finish_job(j);
            continue;
        }
#if defined(SNMOD_BASE_GEOMETRY_APPLY)
        const base_geometry::GhostEnvelope* corridor_geometry = nullptr;
        void* corridor_target = nullptr;
#endif
        // A ConstructableBase is only the temporary construction ghost. The game's
        // placement path also needs its BaseGhost geometry and target Base, neither of
        // which is present in this event. Calling SetState on a bare prefab can destroy
        // that ghost without placing any base geometry; the old path logged success and
        // instantiated it again on the next repair snapshot. Refuse this incomplete
        // operation until the protocol carries the real placement data.
        if (anchored) {
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
            if (!(j.state.build_flags & kWorldBuildHasGeometry)) {
                j.next_try = now + kBuildJobRetrySeconds;
                continue;
            }
            const uint64_t fingerprint = uint64_t(j.state.geometry_hash_low) |
                (uint64_t(j.state.geometry_hash_high) << 32);
            const auto* matched_geometry =
                g_geometry_cache.find(j.state.origin_id, j.state.object_id, fingerprint, now);
            if (!matched_geometry) {
                j.next_try = now + kBuildJobRetrySeconds;
                continue;
            }
#if defined(SNMOD_BASE_GEOMETRY_APPLY)
            if (!(j.state.build_flags & kWorldBuildComplete)) {
                j.next_try = now + kBuildJobRetrySeconds;
                continue;
            }
            if (j.state.face_has_value ||
                !base_geometry::supported_piece_placement(*matched_geometry, j.state.tech_type)) {
                LOG_WARN("build: unsupported base piece id=%s tech=%d ghost=%s", j.state.object_id,
                         j.state.tech_type, matched_geometry->ghost_class.c_str());
                build_finish_job(j);
                continue;
            }
            corridor_geometry = matched_geometry;
            corridor_target = build_corridor_target(matched_geometry->target_base_id,
                                                     matched_geometry->target_base_anchor,!matched_geometry->creates_base);
            if (!corridor_target && !matched_geometry->creates_base) {
                j.next_try = now + kBuildJobRetrySeconds;
                continue;
            }
            if (!corridor_target && matched_geometry->creates_base && build_base_id_exists(matched_geometry->target_base_id)) {
                j.next_try=now+kBuildJobRetrySeconds;
                continue; // same ID is busy, ambiguous, or not fully loaded
            }
#else
            LOG_WARN("build: geometry received for id=%s; reconstruction is disabled", j.state.object_id);
            build_finish_job(j);
            continue;
#endif
#else
            LOG_WARN("build: base geometry unavailable id=%s", j.state.object_id);
            build_finish_job(j);
            continue;
#endif
        }
        void* prefab = nullptr;
        LOG_INFO("build: stage=scene-before id=%s method=%p", j.state.object_id,
                 (void*)g_build_scene_prefab.method);
        bool cached = g_build_scene_prefab.call(class_text, &prefab, g_build_scene_prefab.method);
        LOG_INFO("build: stage=scene-after id=%s cached=%u prefab=%p", j.state.object_id,
                 static_cast<unsigned>(cached), prefab);
        if (!cached || !build_live(prefab)) {
            // ScenePrefabDatabase only covers bundles already loaded in this process.
            // Reuse the item/vehicle path's polled PrefabDatabase.GetPrefabAsync request;
            // it is driven here on Player.Update, never on the network pump or via
            // runtime_invoke. build_spawn still validates the prefab class/attachment
            // before InstantiateFromPrefab can run.
            prefab = find_prefab_source(static_cast<uint32_t>(j.state.tech_type));
            if (build_live(prefab)) {
                LOG_INFO("build: database prefab ready id=%s tech=%d prefab=%p",
                         j.state.object_id, j.state.tech_type, prefab);
            }
        }
        if (build_live(prefab)) {
#if defined(SNMOD_BASE_GEOMETRY_APPLY)
            if (anchored) {
                bool verified = build_apply_completed_corridor(prefab, corridor_target, j.state,
                                                                *corridor_geometry);
                if (verified || build_corridor_record(j.state, false)) build_finish_job(j);
                else j.next_try = now + kBuildJobRetrySeconds;
                continue;
            }
#endif
            build_spawn(prefab, j.state);
            build_finish_job(j);
        } else {
            // Keep the job while the game's async prefab request loads. The sender's
            // periodic repair snapshot re-feeds it if the bounded job queue expires.
            j.next_try = now + kBuildJobRetrySeconds;
        }
    }
}

void tick_build_sync(void* /*player*/, const LocalState& local) {
    if (!local.valid) return;
    if (!kBuildSyncEnabled) return;
    if (!g_role_known) {
        g_build_baseline = false;
        return;
    }
    if (!g_build_attempted) resolve_build_api();
    if (!g_build_enabled) return;
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
    build_refresh_geometry_cache_session();
    build_install_completion_hook();
    build_install_deconstruction_hook();
    build_send_completed();
    base_geometry::GhostEnvelope received_geometry;
    uint32_t geometry_origin = 0;
    while (net_geometry_take(&geometry_origin, &received_geometry)) {
        const std::string id = received_geometry.object_id;
        uint64_t fingerprint = 0;
        if (g_geometry_cache.put(geometry_origin, std::move(received_geometry), build_now(), &fingerprint))
            LOG_INFO("build: geometry cached id=%s origin=%08x fingerprint=%016llx",
                     id.c_str(), geometry_origin, static_cast<unsigned long long>(fingerprint));
        else LOG_WARN("build: invalid geometry received id=%s origin=%08x", id.c_str(), geometry_origin);
    }
#endif
    build_pump_jobs();
    double now = build_now();

    // Placement is not hooked, so the moment a build is on screen is remembered here
    // and used later to tell "just placed this" from "found in the world".
    if (g_build_is_placing.call(g_build_is_placing.method)) {
        g_build_place_tech = g_build_last_tech.call(g_build_last_tech.method);
        g_build_place_until = now + 2.0;
    }

    if (now < g_build_next_scan || g_applying_remote) return;
    g_build_next_scan = now + kBuildScanSeconds;
    if (net_peer_count() == 0) {
        g_build_baseline = false;
        return;
    }
    void* list = g_find_objects_all.call(g_build_type, true, g_find_objects_all.method);
    if (!list) return;
    void* ghost = g_build_ghost.call(g_build_ghost.method);
    int32_t n = g_array_get_length.call(list, g_array_get_length.method);
    int32_t captured = 0;
    int32_t refused = 0;
    int32_t fresh_count = 0;
    g_build_refuse[0] = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* c = g_array_get_value.call(list, i, g_array_get_value.method);
        WorldEvent state{};
        if (!build_capture(c, &state)) {
            refused++;
            continue;
        }
        captured++;

#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
        // A preview can already have amount=1 and _constructed=true. Polling its
        // fields is not evidence that the player built anything. Base pieces are
        // published exclusively by the confirmed SetState transition hook.
        if (state.build_flags & kWorldBuildAnchored) continue;
        if (build_is_preview(c)) continue;
#endif

        // The ghost follows the player around while a piece is being aimed; shipping it
        // would spawn a duplicate wherever the other phone happens to be looking.
        if (ghost) {
            void* go = g_get_game_object.call(c, g_get_game_object.method);
            bool is_ghost = false;
            walk_parents(go, [&](void* node) {
                if (node != ghost) return false;
                is_ghost = true;
                return true;
            });
            if (is_ghost) continue;
        }

        BuildKnown* known = nullptr;
        for (size_t k = 0; k < sizeof(g_build_known) / sizeof(BuildKnown); k++) {
            if (g_build_known[k].used &&
                strcmp(g_build_known[k].state.object_id, state.object_id) == 0) {
                known = &g_build_known[k];
                break;
            }
        }
        bool fresh = known == nullptr;
        if (!known) known = build_known(state);
        if (fresh) fresh_count++;
        if (!known || g_build_capacity_failed) continue;
        known->last_seen_scan = g_build_scan_id;
        known->absent_votes = 0;

#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
        if ((state.build_flags & kWorldBuildAnchored) &&
            (known->state.build_flags & kWorldBuildHasGeometry)) {
            state.build_flags |= kWorldBuildHasGeometry;
            state.geometry_hash_low = known->state.geometry_hash_low;
            state.geometry_hash_high = known->state.geometry_hash_high;
        }
#endif

        bool changed =
            std::fabs(state.constructed_amount - known->state.constructed_amount) > 0.0001f ||
            state.build_flags != known->state.build_flags;
        bool is_new = false;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(c), g_build_new_field, &is_new);
        float dx = state.x - local.x;
        float dy = state.y - local.y;
        float dz = state.z - local.z;
        // fresh is the condition that matters: an object this phone has never seen is
        // by definition one the other phone has not been told about. The builder window and
        // isNew only decide whether it is called a placement or a progress snapshot -
        // relying on them to gate the send loses every module placed a second too late.
        bool placement = fresh;
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
        if ((placement || g_build_resync ||
             (known->advertised && now - known->sent_at >= kBuildRepairSeconds)) &&
            (state.build_flags & kWorldBuildAnchored)) {
            base_geometry::Snapshot topology;
            int32_t target_offset[3]{};
            int32_t target_anchor[3]{};
            char target_id[kWorldEventObjectIdSize]{};
            const char* ghost_class = nullptr;
            std::vector<uint8_t> encoded;
            bool captured_geometry = build_capture_geometry(c, &topology, target_offset, target_anchor, target_id,
                                                             sizeof(target_id), &ghost_class,state.tech_type);
            base_geometry::GhostEnvelope envelope;
            if (captured_geometry) {
                envelope.object_id = state.object_id;
                envelope.target_base_id = target_id;
                envelope.ghost_class = ghost_class;
                envelope.target_offset = {target_offset[0], target_offset[1], target_offset[2]};
                envelope.target_base_anchor = {target_anchor[0], target_anchor[1], target_anchor[2]};
                envelope.geometry = std::move(topology);
            }
            if (captured_geometry && base_geometry::encode_envelope(envelope, &encoded)) {
                LOG_INFO("build: geometry captured id=%s ghost=%s target=%s "
                         "targetOffset=(%d,%d,%d) shape=(%d,%d,%d) bytes=%zu",
                         state.object_id, ghost_class, target_id[0] ? target_id : "new-base",
                         target_offset[0], target_offset[1], target_offset[2],
                         envelope.geometry.shape[0], envelope.geometry.shape[1],
                         envelope.geometry.shape[2], encoded.size());
                if (!net_geometry_enqueue(envelope)) {
                    LOG_WARN("build: geometry not queued id=%s (no peer or channel busy)", state.object_id);
                    state.build_flags &= static_cast<uint8_t>(~kWorldBuildHasGeometry);
                    state.geometry_hash_low = state.geometry_hash_high = 0;
                } else {
                    const uint64_t fingerprint = base_geometry::envelope_fingerprint(encoded.data(), encoded.size());
                    state.geometry_hash_low = static_cast<uint32_t>(fingerprint);
                    state.geometry_hash_high = static_cast<uint32_t>(fingerprint >> 32);
                    state.build_flags |= kWorldBuildHasGeometry;
                }
            } else {
                state.build_flags &= static_cast<uint8_t>(~kWorldBuildHasGeometry);
                state.geometry_hash_low = state.geometry_hash_high = 0;
                LOG_WARN("build: geometry capture unavailable id=%s tech=%d",
                         state.object_id, state.tech_type);
            }
        }
#endif
        (void)is_new;
        (void)g_build_place_until;
        (void)dx; (void)dy; (void)dz;
        // A periodic snapshot repairs anything the transport dropped: every event is
        // retransmitted until acked, but an acked event applied to an object that was
        // then removed would never be repeated otherwise.
        bool repair = known->advertised && now - known->sent_at >= kBuildRepairSeconds;

        if (g_build_baseline && (placement || (!fresh && changed) || repair || g_build_resync)) {
            void* class_text = g_build_class_id.call(state.tech_type, g_build_class_id.method);
            if (build_class_bytes(class_text, state.class_id)) {
                state.kind = placement ? kWorldEventBuild : kWorldEventProgress;
                net_post_world_event(state, true);
                known->advertised = true;
                known->sent_at = now;
                LOG_INFO("build: local %s id=%s tech=%d amount=%.3f",
                         placement ? "place" : "snapshot", state.object_id, state.tech_type,
                         state.constructed_amount);
            } else {
                LOG_WARN("build: skipped, classId is not a GUID");
            }
        }
        known->state = state;
    }
    LOG_INFO("build: scan objects=%d captured=%d refused=%d fresh=%d peers=%zu refuse=%s", n,
             captured, refused, fresh_count, net_peer_count(),
             g_build_refuse[0] ? g_build_refuse : "-");

    // Only plain outdoor pieces use absence as a deconstruction heuristic. A Base module
    // may leave the Constructable scan during the game's geometry rebuild even though the
    // room/corridor remains; base deconstruction needs its own explicit game hook.
    for (size_t k = 0; k < sizeof(g_build_known) / sizeof(BuildKnown); k++) {
        BuildKnown& known = g_build_known[k];
        if (!known.used || !known.advertised) continue;
        if (known.last_seen_scan == g_build_scan_id) continue;
        if (known.state.build_flags & kWorldBuildAnchored) continue;
        float dx = known.state.x - local.x;
        float dy = known.state.y - local.y;
        float dz = known.state.z - local.z;
        if (dx * dx + dy * dy + dz * dz > kBuildRemoveRadius * kBuildRemoveRadius) continue;
        known.absent_votes++;
        if (known.absent_votes < kBuildRemoveVotes) continue;
        WorldEvent removed = known.state;
        removed.origin_id = 0;  // queue_local_event stamps the local origin
        removed.sequence = 0;
        removed.kind = kWorldEventRemove;
        net_post_world_event(removed, true);
        LOG_INFO("build: local remove id=%s tech=%d", removed.object_id, removed.tech_type);
        known = BuildKnown{};
    }
    g_build_scan_id++;
    if (!g_build_baseline) {
        // A peer that has just joined has never heard about anything that already existed when
        // it connected, and those objects are not fresh - they have been in the table since
        // before. The baseline is therefore followed by exactly one scan that sends the whole
        // table, and the flag is dropped here so it is not repeated every scan.
        LOG_INFO("build: baseline taken, next scan resends the whole table to the new peer");
        g_build_resync = true;
    } else {
        g_build_resync = false;
    }
    g_build_baseline = true;
}
