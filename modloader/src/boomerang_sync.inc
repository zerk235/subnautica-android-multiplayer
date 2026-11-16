// Unity objects and GC handles are owned by the game thread.
#include "boomerang_policy.h"
namespace fish_motion = creature_motion;
constexpr float kBoomerangRange = boomerang_sync::kRange;
void* find_prefab_source(uint32_t tech);
bool prefab_retain(ManagedRoot& root, void* object);
struct BoomerangSaved {
    void* component;
    bool enabled;
};
struct BoomerangReplica {
    fish_motion::Message identity;
    fish_motion::Receiver receiver;
    void* fish = nullptr;
    void* body = nullptr;
    void* visual = nullptr; // Private inert clone, never registered as a world fish.
    Il2CppGCHandle handle = 0;
    bool kinematic = false;
    bool collisions = false;
    int interpolation = 0;
    bool hidden = false;
    bool written = false;
    Vector3 last_written{};
    float maximum_drift = 0, maximum_error = 0, interval_drift = 0;
    uint32_t corrections = 0;
    double next_bind = 0, next_validate = 0, created = 0;
    std::vector<BoomerangSaved> behaviours;
    std::vector<BoomerangSaved> renderers;
    std::vector<BoomerangSaved> animators; // Saved applyRootMotion, not enabled.
};
struct BoomerangSync {
    bool runtime_ready = false;
    Entry<bool (*)(void*, void**, void*)> find_identifier;
    Entry<PredicateFn> active, get_renderer, get_collisions;
    Entry<void (*)(void*, bool, void*)> set_renderer, set_collisions;
    Entry<void (*)(void*, void*, void*)> construct_go;
    Entry<void* (*)(void*, Vector3, Quaternion, void*, void*)> instantiate_parent;
    Entry<void (*)(void*, int, void*)> hide_flags;
    Entry<void (*)(void*, int, void*)> set_interpolation;
    Entry<int (*)(void*, void*)> get_interpolation;
    Entry<void (*)(void*, bool, void*)> set_root_motion;
    Entry<void (*)(void*, bool, void*)> set_collider;
    void* mono_type = nullptr;
    void* peeper_type = nullptr;
    Entry<PredicateFn> peeper_hero, peeper_born_hero, peeper_in_prison;
    ManagedRoot staging;
    fish_motion::Id session{};
    uint32_t epoch = 0, host = 0, generation = 0, sequence = 0;
    double next_scan = 0, next_send = 0, next_report = 0, next_spawn = 0;
    std::array<fish_motion::Message, fish_motion::kLiveFishLimit> advertised;
    fish_motion::LiveSchedule schedule;
    size_t advertised_count = 0;
    size_t sent_since_report = 0;
    std::vector<BoomerangReplica> replicas;
    fish_motion::SequenceGate sequences;
} g_boomerang;

void destroy_boomerang_visual(void* go) {
    if (!probe_exists(go)) return;
    g_set_active.call(go, false, g_set_active.method);
    g_destroy.call(go, g_destroy.method);
}

// Clones are born under an inactive parent. Strip gameplay scripts before the
// hierarchy can run Awake/OnEnable/Start; never mutate the shared prefab.
bool resolve_boomerang_visual() {
    auto& sync = g_boomerang;
    if (!sync.construct_go) {
        auto* go = find_class_in_domain("UnityEngine", "GameObject");
        auto* object = find_class_in_domain("UnityEngine", "Object");
        auto* mono = find_class_in_domain("UnityEngine", "MonoBehaviour");
        auto* collider = find_class_in_domain("UnityEngine", "Collider");
        sync.construct_go.bind(find_method(go, ".ctor", 1));
        sync.instantiate_parent.bind(build_typed_method(object, "Instantiate", 4, "UnityEngine.Object"));
        sync.hide_flags.bind(find_method(object, "set_hideFlags", 1));
        sync.set_collider.bind(find_method(collider, "set_enabled", 1));
        if (mono) sync.mono_type = il2cpp_type_get_object(il2cpp_class_get_type(mono));
    }
    return sync.construct_go && sync.instantiate_parent && sync.hide_flags &&
        sync.set_collider && sync.set_interpolation && sync.set_root_motion && g_anim_get_root_motion && sync.mono_type &&
        g_collider_type_obj && g_rigidbody_type && g_renderer_type_obj && g_object_destroy_immediate &&
        g_set_active && g_destroy && g_get_go_transform && g_set_parent;
}

void* boomerang_staging_parent() {
    auto& sync = g_boomerang;
    if (probe_exists(sync.staging.get())) return sync.staging.get();
    auto* klass = find_class_in_domain("UnityEngine", "GameObject");
    void* go = il2cpp_object_new(klass);
    if (!go || !prefab_retain(sync.staging, go)) return nullptr;
    sync.construct_go.call(go, il2cpp_string_new("SNMP inert creature staging"), sync.construct_go.method);
    g_set_active.call(go, false, g_set_active.method);
    sync.hide_flags.call(go, 52, sync.hide_flags.method); // HideFlags.DontSave.
    return go;
}

bool resolve_boomerang_sync() {
    auto& sync = g_boomerang;
    if (sync.find_identifier && sync.active && sync.get_renderer && sync.set_renderer &&
        sync.get_collisions && sync.set_collisions && sync.get_interpolation && sync.set_interpolation &&
        sync.set_root_motion && g_anim_get_root_motion && g_animator_type_obj) return true;
    auto* go = find_class_in_domain("UnityEngine", "GameObject");
    auto* renderer = find_class_in_domain("UnityEngine", "Renderer");
    auto* body = find_class_in_domain("UnityEngine", "Rigidbody");
    if (auto* peeper = build_class("Peeper")) {
        sync.peeper_hero.bind(find_method(peeper, "get_isHero", 0));
        sync.peeper_born_hero.bind(find_method(peeper, "get_isBornHero", 0));
        sync.peeper_in_prison.bind(find_method(peeper, "get_isInPrisonAquarium", 0));
        if (sync.peeper_hero && sync.peeper_born_hero && sync.peeper_in_prison)
            sync.peeper_type = il2cpp_type_get_object(il2cpp_class_get_type(peeper));
    }
    sync.find_identifier.bind(find_method(g_unique_identifier_class, "TryGetIdentifier", 2));
    sync.active.bind(find_method(go, "get_activeInHierarchy", 0));
    sync.get_renderer.bind(find_method(renderer, "get_enabled", 0));
    sync.set_renderer.bind(find_method(renderer, "set_enabled", 1));
    sync.get_collisions.bind(find_method(body, "get_detectCollisions", 0));
    sync.set_collisions.bind(find_method(body, "set_detectCollisions", 1));
    sync.get_interpolation.bind(find_method(body, "get_interpolation", 0));
    sync.set_interpolation.bind(find_method(body, "set_interpolation", 1));
    sync.set_root_motion.bind(find_method(g_animator_class, "set_applyRootMotion", 1));
    return sync.find_identifier && sync.active && sync.get_renderer && sync.set_renderer &&
           sync.get_collisions && sync.set_collisions && sync.get_interpolation && sync.set_interpolation &&
           sync.set_root_motion && g_anim_get_root_motion && g_animator_type_obj;
}

void* synchronized_fish_type(fish_motion::Species species) {
    return species == fish_motion::Species::Peeper ? g_boomerang.peeper_type : g_creature_probe.fish_type;
}

bool ordinary_peeper(void* fish) {
    const auto& sync = g_boomerang;
    return sync.peeper_hero && sync.peeper_born_hero && sync.peeper_in_prison &&
           !sync.peeper_hero.call(fish, sync.peeper_hero.method) &&
           !sync.peeper_born_hero.call(fish, sync.peeper_born_hero.method) &&
           !sync.peeper_in_prison.call(fish, sync.peeper_in_prison.method);
}

bool wild_synchronized_fish(void* fish, void* go, fish_motion::Species species) {
    const auto& probe = g_creature_probe;
    const auto& sync = g_boomerang;
    if (!probe_exists(fish) || !probe_exists(go) ||
        !sync.active.call(go, sync.active.method) || object_in_local_inventory(go)) return false;
    void* live = g_get_component.call(go, probe.live_type, g_get_component.method);
    if (!probe_exists(live) || !probe.alive.call(live, probe.alive.method)) return false;
    void* park = g_get_component.call(go, probe.waterpark_type, g_get_component.method);
    return (!park || !contents_field(park, probe.waterpark_field)) &&
           (species != fish_motion::Species::Peeper || ordinary_peeper(fish));
}

void release_boomerang(BoomerangReplica& replica) {
    if (!replica.handle) return;
    const auto& sync = g_boomerang;
    if (replica.visual) {
        destroy_boomerang_visual(replica.visual);
        replica.visual = nullptr;
        il2cpp_gchandle_free(replica.handle);
        replica.handle = 0;
        replica.fish = replica.body = nullptr;
        replica.behaviours.clear(); replica.renderers.clear(); replica.animators.clear();
        replica.hidden = replica.written = false;
        return;
    }
    if (probe_exists(replica.body)) {
        sync.set_interpolation.call(replica.body, replica.interpolation, sync.set_interpolation.method);
        sync.set_collisions.call(replica.body, replica.collisions, sync.set_collisions.method);
        g_rb_set_kinematic.call(replica.body, replica.kinematic, g_rb_set_kinematic.method);
    }
    for (const auto& saved : replica.animators)
        if (probe_exists(saved.component) &&
            g_anim_get_root_motion.call(saved.component, g_anim_get_root_motion.method) != saved.enabled)
            sync.set_root_motion.call(saved.component, saved.enabled, sync.set_root_motion.method);
    for (auto it = replica.behaviours.rbegin(); it != replica.behaviours.rend(); ++it)
        if (probe_exists(it->component))
            g_behaviour_set_enabled.call(it->component, it->enabled, g_behaviour_set_enabled.method);
    if (probe_exists(replica.fish) && !replica.behaviours.empty() && replica.behaviours.front().enabled) {
        const auto& allow = g_creature_probe.allow;
        allow.call(replica.fish, true, allow.method);
    }
    if (replica.hidden)
        for (const auto& saved : replica.renderers)
            if (probe_exists(saved.component))
                sync.set_renderer.call(saved.component, saved.enabled, sync.set_renderer.method);
    il2cpp_gchandle_free(replica.handle);
    replica.handle = 0;
    replica.fish = replica.body = nullptr;
    replica.behaviours.clear();
    replica.renderers.clear();
    replica.animators.clear();
    replica.hidden = false;
}

void reset_boomerang_sync() {
    auto& sync = g_boomerang;
    for (auto& replica : sync.replicas) {
        release_boomerang(replica);
    }
    sync.replicas.clear();
    destroy_boomerang_visual(sync.staging.get());
    sync.staging.clear();
    sync.sequences = {};
    sync.host = sync.epoch = 0;
    sync.runtime_ready = false;
    sync.next_scan = sync.next_send = sync.next_report = sync.next_spawn = 0;
    sync.advertised_count = 0;
    sync.sent_since_report = 0;
    sync.schedule = {};
}

// Unity's registered UID is the authority for the local instance. Format the
// raw GUID as Unity stores it, then confirm both IDs and the Boomerang component.
bool boomerang_identity(void* uid, fish_motion::Message& out) {
    return probe_exists(uid) &&
           build_class_bytes(g_unique_get_id.call(uid, g_unique_get_id.method), out.fish.data()) &&
           build_class_bytes(g_unique_get_class_id.call(uid, g_unique_get_class_id.method), out.prefab.data());
}

void* find_boomerang(const fish_motion::Message& identity) {
    static constexpr char hex[] = "0123456789abcdef";
    char id[37]{};
    size_t n = 0;
    for (size_t i = 0; i < identity.fish.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) id[n++] = '-';
        id[n++] = hex[identity.fish[i] >> 4];
        id[n++] = hex[identity.fish[i] & 15];
    }
    const auto& find = g_boomerang.find_identifier;
    void* uid = nullptr;
    if (!find.call(il2cpp_string_new(id), &uid, find.method)) {
        // Some saves use the compact spelling of the same exact GUID.
        char compact[33]{}; size_t at = 0;
        for (const char* p = id; *p; ++p) if (*p != '-') compact[at++] = *p;
        uid = nullptr;
        if (!find.call(il2cpp_string_new(compact), &uid, find.method)) return nullptr;
    }
    fish_motion::Message found;
    if (!boomerang_identity(uid, found) || found.fish != identity.fish || found.prefab != identity.prefab)
        return nullptr;
    void* go = g_get_game_object.call(uid, g_get_game_object.method);
    if (!probe_exists(go)) return nullptr;
    void* type = synchronized_fish_type(identity.species);
    if (!type) return nullptr;
    void* fish = g_get_component.call(go, type, g_get_component.method);
    return wild_synchronized_fish(fish, go, identity.species) ? fish : nullptr;
}

void update_boomerang_replica(BoomerangReplica& replica, bool hidden) {
    const auto& sync = g_boomerang;
    const auto& allow = g_creature_probe.allow;
    if (replica.fish && g_behaviour_get_enabled.call(replica.fish, g_behaviour_get_enabled.method))
        allow.call(replica.fish, false, allow.method);
    for (const auto& saved : replica.behaviours)
        if (probe_exists(saved.component) &&
            g_behaviour_get_enabled.call(saved.component, g_behaviour_get_enabled.method)) {
            ++replica.corrections;
            g_behaviour_set_enabled.call(saved.component, false, g_behaviour_set_enabled.method);
        }
    if (replica.body && !g_rb_get_kinematic.call(replica.body, g_rb_get_kinematic.method)) {
        ++replica.corrections;
        g_rb_set_kinematic.call(replica.body, true, g_rb_set_kinematic.method);
    }
    if (replica.body && sync.get_collisions.call(replica.body, sync.get_collisions.method))
        sync.set_collisions.call(replica.body, false, sync.set_collisions.method);
    if (replica.body && sync.get_interpolation.call(replica.body, sync.get_interpolation.method) != 0) {
        ++replica.corrections;
        sync.set_interpolation.call(replica.body, 0, sync.set_interpolation.method);
    }
    for (const auto& saved : replica.animators)
        if (probe_exists(saved.component) && g_anim_get_root_motion.call(saved.component, g_anim_get_root_motion.method)) {
            ++replica.corrections;
            sync.set_root_motion.call(saved.component, false, sync.set_root_motion.method);
        }
    if (!hidden && !replica.hidden) return;
    for (const auto& saved : replica.renderers)
        if (probe_exists(saved.component) &&
            sync.get_renderer.call(saved.component, sync.get_renderer.method) != (!hidden && saved.enabled))
            sync.set_renderer.call(saved.component, !hidden && saved.enabled, sync.set_renderer.method);
    replica.hidden = hidden;
}

void bind_boomerang(BoomerangReplica& replica) {
    const auto& probe = g_creature_probe;
    const auto& sync = g_boomerang;
    void* fish = find_boomerang(replica.identity);
    if (!fish) return;
    void* go = g_get_game_object.call(fish, g_get_game_object.method);
    void* body = g_get_component.call(go, g_rigidbody_type, g_get_component.method);
    if (!probe_exists(body)) return;
    std::vector<BoomerangSaved> behaviours, renderers, animators;
    if (!boomerang_components(fish, go, [&](void* component) {
        if (probe_exists(component) && std::none_of(behaviours.begin(), behaviours.end(),
            [component](const auto& saved) { return saved.component == component; }))
            behaviours.push_back({component, g_behaviour_get_enabled.call(component, g_behaviour_get_enabled.method)});
    })) return;
    void* list = probe.components.call(go, g_renderer_type_obj, true, probe.components.method);
    if (!list) return;
    const int count = g_array_get_length.call(list, g_array_get_length.method);
    if (count <= 0 || count > 48) return;
    for (int i = 0; i < count; ++i) {
        void* renderer = g_array_get_value.call(list, i, g_array_get_value.method);
        if (probe_exists(renderer))
            renderers.push_back({renderer, sync.get_renderer.call(renderer, sync.get_renderer.method)});
    }
    if (renderers.empty()) return;
    void* animator_list = probe.components.call(go, g_animator_type_obj, true, probe.components.method);
    if (!animator_list) return;
    const int animator_count = g_array_get_length.call(animator_list, g_array_get_length.method);
    if (animator_count < 0 || animator_count > 16) return;
    for (int i = 0; i < animator_count; ++i) {
        void* animator = g_array_get_value.call(animator_list, i, g_array_get_value.method);
        if (probe_exists(animator))
            animators.push_back({animator, g_anim_get_root_motion.call(animator, g_anim_get_root_motion.method)});
    }

    auto* object_class = find_class_in_domain("System", "Object");
    if (!object_class) return;
    void* roots = il2cpp_array_new(object_class, 2 + behaviours.size() + renderers.size() + animators.size());
    if (!roots) return;
    const auto handle = il2cpp_gchandle_new(reinterpret_cast<Il2CppObject*>(roots), true);
    if (!handle) return;
    const auto& set = g_contents_api.array_set;
    set.call(roots, fish, 0, set.method);
    set.call(roots, body, 1, set.method);
    int index = 2;
    for (const auto& saved : behaviours) set.call(roots, saved.component, index++, set.method);
    for (const auto& saved : renderers) set.call(roots, saved.component, index++, set.method);
    for (const auto& saved : animators) set.call(roots, saved.component, index++, set.method);
    replica.fish = fish;
    replica.body = body;
    replica.handle = handle;
    replica.kinematic = g_rb_get_kinematic.call(body, g_rb_get_kinematic.method);
    replica.collisions = sync.get_collisions.call(body, sync.get_collisions.method);
    replica.interpolation = sync.get_interpolation.call(body, sync.get_interpolation.method);
    replica.behaviours = std::move(behaviours);
    replica.renderers = std::move(renderers);
    replica.animators = std::move(animators);
    replica.written = false;
    replica.maximum_drift = replica.maximum_error = replica.interval_drift = 0;
    // A locally loaded fish is suppressed even before a matching host packet.
    update_boomerang_replica(replica, true);
    replica.corrections = 0;
    const auto root_motion = std::count_if(replica.animators.begin(), replica.animators.end(),
        [](const auto& saved) { return saved.enabled; });
    LOG_INFO("boomerang sync: guest bound seq=%u behaviours=%zu renderers=%zu interpolation=%d root_motion=%zu",
        replica.identity.sequence, replica.behaviours.size(), replica.renderers.size(), replica.interpolation, size_t(root_motion));
}

bool spawn_boomerang_visual(BoomerangReplica& replica, const fish_motion::Message& pose) {
    auto& sync = g_boomerang;
    if (!resolve_boomerang_visual()) return false;
    void* prefab = find_prefab_source(fish_motion::tech_for_species(pose.species));
    if (!synchronized_fish_type(pose.species) || !probe_exists(prefab)) return false;
    void* uid = g_get_component.call(prefab, g_unique_identifier_type, g_get_component.method);
    fish_motion::Id prefab_id{};
    if (!uid || !build_class_bytes(g_unique_get_class_id.call(uid, g_unique_get_class_id.method), prefab_id.data()) ||
        prefab_id != pose.prefab) return false;
    void* stage = boomerang_staging_parent();
    if (!stage) return false;
    const Vector3 pos{pose.position[0], pose.position[1], pose.position[2]};
    const Quaternion rot{pose.rotation[0], pose.rotation[1], pose.rotation[2], pose.rotation[3]};
    void* go = sync.instantiate_parent.call(prefab, pos, rot,
        g_get_go_transform.call(stage, g_get_go_transform.method), sync.instantiate_parent.method);
    if (!probe_exists(go) || go == prefab || go == stage) return false;
    ManagedRoot guard;
    if (!prefab_retain(guard, go)) { destroy_boomerang_visual(go); return false; }
    auto fail = [&]() { destroy_boomerang_visual(go); return false; };
    void* clone_tr = g_get_go_transform.call(go, g_get_go_transform.method);
    if (!clone_tr ||
        !g_transform_get_parent || g_transform_get_parent.call(clone_tr, g_transform_get_parent.method) !=
            g_get_go_transform.call(stage, g_get_go_transform.method) ||
        sync.active.call(go, sync.active.method)) return fail();
    g_set_active.call(go, false, g_set_active.method);
    sync.hide_flags.call(go, 52, sync.hide_flags.method);

    // Keep only the already-used lighting component. Animator, renderers and
    // bones are native Unity components, not MonoBehaviours. No UID, Creature,
    // pickup, damage, LargeWorldEntity or serializer survives on this clone.
    auto components = [&](void* type, auto visit) {
        void* array = g_creature_probe.components.call(go, type, true, g_creature_probe.components.method);
        if (!array) return false;
        const int size = g_array_get_length.call(array, g_array_get_length.method);
        if (size < 0 || size > 128) return false;
        ManagedRoot root;
        if (!prefab_retain(root, array)) return false;
        for (int i = 0; i < size; ++i) {
            void* item = g_array_get_value.call(array, i, g_array_get_value.method);
            if (probe_exists(item) && !visit(item)) return false;
        }
        return true;
    };
    unsigned stripped = 0;
    // RequireComponent dependencies can prevent the first removal. Retry in
    // bounded passes, and refuse activation if any gameplay script remains.
    bool clean = false;
    for (int pass = 0; pass < 4 && !clean; ++pass) {
        if (!components(sync.mono_type, [&](void* item) {
            const char* name = il2cpp_class_get_name(il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(item)));
            if (name && !strcmp(name, "SkyApplier")) return true;
            g_object_destroy_immediate.call(item, g_object_destroy_immediate.method);
            if (!probe_exists(item)) ++stripped;
            return true;
        })) return fail();
        clean = components(sync.mono_type, [&](void* item) {
            const char* name = il2cpp_class_get_name(il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(item)));
            return name && !strcmp(name, "SkyApplier");
        });
    }
    if (!clean) return fail();
    if (!components(g_collider_type_obj, [&](void* item) {
        sync.set_collider.call(item, false, sync.set_collider.method); return true;
    })) return fail();
    if (!components(g_rigidbody_type, [&](void* item) {
        g_rb_set_kinematic.call(item, true, g_rb_set_kinematic.method);
        sync.set_interpolation.call(item, 0, sync.set_interpolation.method);
        sync.set_collisions.call(item, false, sync.set_collisions.method); return true;
    })) return fail();
    if (!components(g_animator_type_obj, [&](void* item) {
        if (g_anim_get_root_motion.call(item, g_anim_get_root_motion.method))
            sync.set_root_motion.call(item, false, sync.set_root_motion.method);
        return true;
    })) return fail();
    std::vector<BoomerangSaved> renderers;
    if (!components(g_renderer_type_obj, [&](void* item) {
        renderers.push_back({item, sync.get_renderer.call(item, sync.get_renderer.method)});
        return true;
    }) || renderers.empty()) return fail();
    void* roots = il2cpp_array_new(find_class_in_domain("System", "Object"), 1 + renderers.size());
    if (!roots) return fail();
    const auto handle = il2cpp_gchandle_new(reinterpret_cast<Il2CppObject*>(roots), true);
    if (!handle) return fail();
    const auto& set = g_contents_api.array_set;
    set.call(roots, go, 0, set.method);
    for (size_t i = 0; i < renderers.size(); ++i) set.call(roots, renderers[i].component, i + 1, set.method);
    replica.handle = handle; replica.visual = go; replica.renderers = std::move(renderers);
    replica.fish = replica.body = nullptr; replica.behaviours.clear(); replica.animators.clear();
    replica.maximum_drift = replica.maximum_error = replica.interval_drift = 0;
    replica.written = replica.hidden = false; replica.corrections = 0;
    // Remain inactive until the pose is written below; detach from staging only
    // after the component audit. A visual has no identity in Unity's registry.
    void* tr = g_get_go_transform.call(go, g_get_go_transform.method);
    g_set_parent.call(tr, nullptr, true, g_set_parent.method);
    LOG_INFO("boomerang sync: inert visual created tech=%d seq=%u stripped=%u renderers=%zu no_world_id=1",
        fish_motion::tech_for_species(pose.species), pose.sequence, stripped, replica.renderers.size());
    return true;
}

template<class Position>
float boomerang_distance_sq(Vector3 pos, const Position& other) {
    const float dx = pos.x - other.x, dy = pos.y - other.y, dz = pos.z - other.z;
    return dx * dx + dy * dy + dz * dz;
}

void log_boomerang_pose(const char* role, const fish_motion::Message& pose,
                       double age, float error, float drift, uint32_t corrections,
                       float window = 0, bool visual = false) {
    char id[33]{};
    for (size_t i = 0; i < pose.fish.size(); ++i) snprintf(id + i * 2, 3, "%02x", pose.fish[i]);
    LOG_INFO("boomerang sync: %s id=%s tech=%d seq=%u pos=%.3f,%.3f,%.3f age=%.3f error=%.5f drift=%.5f corrections=%u drift_window=%.5f visual=%u",
        role, id, fish_motion::tech_for_species(pose.species), pose.sequence, pose.position[0], pose.position[1], pose.position[2], age, error, drift, corrections, window, unsigned(visual));
}

BoomerangReplica* boomerang_record(const fish_motion::Message& identity, double now) {
    auto& replicas = g_boomerang.replicas;
    auto it = std::find_if(replicas.begin(), replicas.end(),
        [&](const auto& r) { return fish_motion::same_run(r.identity, identity); });
    if (it != replicas.end()) return &*it;
    // Bound by the same loaded-object scan limit, not a lifetime fish quota.
    if (replicas.size() >= 4096) return nullptr;
    replicas.emplace_back();
    auto& r = replicas.back();
    r.identity = identity;
    r.receiver.reset(identity);
    r.created = now;
    return &r;
}

template<class Visit>
void for_each_synchronized_fish(Visit visit) {
    for (auto species : {fish_motion::Species::Boomerang, fish_motion::Species::Peeper}) {
        void* type = synchronized_fish_type(species);
        if (!type) continue;
        void* list = g_find_objects_all.call(type, false, g_find_objects_all.method);
        const int length = list ? g_array_get_length.call(list, g_array_get_length.method) : 0;
        for (int i = 0; i < length && i < 4096; ++i) {
            void* fish = g_array_get_value.call(list, i, g_array_get_value.method);
            if (probe_exists(fish)) visit(fish, species);
        }
    }
}

void suppress_local_boomerangs(const LocalState& local, double now) {
    auto& sync = g_boomerang;
    if (now < sync.next_scan) return;
    sync.next_scan = now + .25;
    for_each_synchronized_fish([&](void* fish, fish_motion::Species species) {
        void* go = g_get_game_object.call(fish, g_get_game_object.method);
        if (!wild_synchronized_fish(fish, go, species)) return;
        void* tr = g_get_transform.call(fish, g_get_transform.method);
        if (!probe_exists(tr)) return;
        const auto pos = g_get_position.call(tr, g_get_position.method);
        if (!(boomerang_distance_sq(pos, local) <= kBoomerangRange * kBoomerangRange)) return;
        fish_motion::Message identity;
        void* uid = g_get_component.call(go, g_unique_identifier_type, g_get_component.method);
        if (!boomerang_identity(uid, identity)) return;
        identity.species = species;
        identity.kind = fish_motion::LivePose;
        identity.sender = identity.host = sync.host;
        identity.session = sync.session;
        identity.epoch = sync.epoch;
        identity.nonce = 1;
        identity.sequence = 1; // run identity only; Receiver still has no pose
        auto* replica = boomerang_record(identity, now);
        if (fish_pickup_suppressed(identity)) return;
        if (replica && !replica->handle && now >= replica->next_bind) {
            replica->next_bind = now + .5;
            bind_boomerang(*replica);
        }
    });
}

void publish_boomerangs(const LocalState& local, double now) {
    auto& sync = g_boomerang;
    if (now < sync.next_send) return;
    sync.next_send = now + .02;
    PeerState peers[kMaxPeers]{};
    const size_t count = net_copy_world_peers(peers, kMaxPeers);
    if (!count) {
        sync.advertised_count = 0;
        sync.schedule = {};
        sync.next_scan = 0;
        return;
    }
    if (now >= sync.next_scan) {
        sync.next_scan = now + 1;
        sync.advertised_count = 0;
        using Candidate = std::pair<float, fish_motion::Message>;
        std::vector<Candidate> candidates;
        for_each_synchronized_fish([&](void* fish, fish_motion::Species species) {
            void* tr = g_get_transform.call(fish, g_get_transform.method);
            if (!probe_exists(tr)) return;
            Vector3 pos = g_get_position.call(tr, g_get_position.method);
            if (!(boomerang_distance_sq(pos, local) <= kBoomerangRange * kBoomerangRange)) return;
            float distance = kBoomerangRange * kBoomerangRange;
            for (size_t p = 0; p < count; ++p)
                distance = std::min(distance, boomerang_distance_sq(pos, peers[p]));
            if (distance >= kBoomerangRange * kBoomerangRange) return;
            void* go = g_get_game_object.call(fish, g_get_game_object.method);
            if (!wild_synchronized_fish(fish, go, species)) return;
            fish_motion::Message identity;
            identity.species = species;
            void* uid = g_get_component.call(go, g_unique_identifier_type, g_get_component.method);
            if (boomerang_identity(uid, identity)) candidates.emplace_back(distance, identity);
        });
        const size_t selected = std::min(candidates.size(), sync.advertised.size());
        std::partial_sort(candidates.begin(), candidates.begin() + selected, candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.first < b.first; });
        for (size_t i = 0; i < selected; ++i) sync.advertised[sync.advertised_count++] = candidates[i].second;
        std::array<fish_motion::Id, fish_motion::kLiveFishLimit> ids{};
        for (size_t i = 0; i < sync.advertised_count; ++i) ids[i] = sync.advertised[i].fish;
        sync.schedule.refresh(ids.data(), sync.advertised_count);
    }
    size_t sent = 0;
    for (size_t n = 0, i = 0; n < fish_motion::kLiveSendPerTick && sync.schedule.take(now, &i); ++n) {
        auto pose = sync.advertised[i];
        if (fish_pickup_suppressed(pose)) continue;
        void* fish = find_boomerang(pose);
        if (!fish) continue;
        void* tr = g_get_transform.call(fish, g_get_transform.method);
        if (!probe_exists(tr)) continue;
        const Vector3 pos = g_get_position.call(tr, g_get_position.method);
        const Quaternion rot = g_get_rotation.call(tr, g_get_rotation.method);
        pose.kind = fish_motion::LivePose;
        pose.host = pose.sender = sync.host;
        pose.session = sync.session;
        pose.epoch = sync.epoch;
        pose.nonce = 1;
        if (++sync.sequence == 0) ++sync.sequence;
        pose.sequence = sync.sequence;
        pose.position = {pos.x, pos.y, pos.z};
        pose.rotation = {rot.x, rot.y, rot.z, rot.w};
        if (net_creature_probe_send(pose)) {
            ++sent;
            if (now >= sync.next_report) log_boomerang_pose("tx", pose, 0, 0, 0, 0);
        }
    }
    sync.sent_since_report += sent;
    if (now >= sync.next_report) {
        sync.next_report = now + 5;
        LOG_INFO("boomerang sync: host peers=%zu poses=%zu selected=%zu limit=%zu interval_sent=%zu seq=%u",
            count, sent, sync.advertised_count, fish_motion::kLiveFishLimit, sync.sent_since_report, sync.sequence);
        sync.sent_since_report = 0;
    }
}

void apply_boomerangs(const LocalState& local, double now) {
    auto& sync = g_boomerang;
    suppress_local_boomerangs(local, now);
    fish_motion::Message pose;
    for (size_t n = 0; n < fish_motion::kLiveQueueCapacity && net_creature_pose_take(&pose); ++n) {
        if (!fish_motion::permitted(pose, net_local_id(), sync.host, sync.session, sync.epoch)) continue;
        if (!synchronized_fish_type(pose.species) || !fish_pickup_pose_allowed(pose)) continue;
        const Vector3 pos{pose.position[0], pose.position[1], pose.position[2]};
        if (!(boomerang_distance_sq(pos, local) <= kBoomerangRange * kBoomerangRange)) continue;
        if (!sync.sequences.accept(pose)) continue;
        auto* match = boomerang_record(pose, now);
        if (match) match->receiver.accept(pose, now);
    }
    size_t matched = 0, applied = 0, hidden = 0, visible = 0, visuals = 0, waiting = 0;
    for (auto it = sync.replicas.begin(); it != sync.replicas.end();) {
        auto& replica = *it;
        if (fish_pickup_suppressed(replica.identity)) {
            if (replica.visual) release_boomerang(replica);
            else if (replica.handle) update_boomerang_replica(replica, true);
            ++it; continue;
        }
        const bool has_pose = replica.receiver.sample(now, &pose);
        const double age = replica.receiver.age(now);
        if (has_pose) ++matched;
        // Interest can change before old poses expire. A stale private visual
        // has no world state to retain; do not accumulate invisible clones.
        if (replica.visual && boomerang_sync::hidden(has_pose, age)) release_boomerang(replica);
        if (replica.handle && (replica.visual ? !probe_exists(replica.visual) :
            (!probe_exists(replica.fish) || !probe_exists(replica.body))))
            release_boomerang(replica);
        if (replica.handle && now >= replica.next_validate) {
            replica.next_validate = now + .25;
            if (replica.visual) {
                // A streamed original with this exact identity supersedes the
                // private visual. Never leave two displayed copies behind.
                if (find_boomerang(replica.identity)) {
                    release_boomerang(replica);
                    bind_boomerang(replica);
                }
            } else {
                void* go = g_get_game_object.call(replica.fish, g_get_game_object.method);
                if (!wild_synchronized_fish(replica.fish, go, replica.identity.species)) release_boomerang(replica);
            }
        }
        if (!replica.handle && has_pose && age < 2 && now >= replica.next_bind) {
            replica.next_bind = now + .5;
            bind_boomerang(replica);
            if (!replica.handle && age < .5 && visible < fish_motion::kLiveFishLimit && now >= sync.next_spawn) {
                sync.next_spawn = now + .25; // At most four private clones/sec.
                spawn_boomerang_visual(replica, pose);
            }
        }
        if (!replica.handle) {
            if (has_pose && age < 2) ++waiting;
            if (now - replica.created > 5 && age > 5) it = sync.replicas.erase(it);
            else ++it;
            continue;
        }
        void* tr = replica.visual ? g_get_go_transform.call(replica.visual, g_get_go_transform.method)
            : g_get_transform.call(replica.fish, g_get_transform.method);
        if (!probe_exists(tr)) { ++it; continue; }
        const Vector3 pos = has_pose ? Vector3{pose.position[0], pose.position[1], pose.position[2]}
            : g_get_position.call(tr, g_get_position.method);
        if (boomerang_sync::outside_domain(boomerang_distance_sq(pos, local))) {
            release_boomerang(replica);
            it = sync.replicas.erase(it);
            continue;
        }
        const bool hide = boomerang_sync::hidden(has_pose, age) || visible >= fish_motion::kLiveFishLimit;
        update_boomerang_replica(replica, hide);
        hidden += hide;
        visible += !hide;
        if (!has_pose) { ++it; continue; }
        if (replica.written) {
            const auto before = g_get_position.call(tr, g_get_position.method);
            const auto drift = std::sqrt(boomerang_distance_sq(before, replica.last_written));
            replica.maximum_drift = std::max(replica.maximum_drift, drift);
            replica.interval_drift = std::max(replica.interval_drift, drift);
        }
        const Quaternion rot{pose.rotation[0], pose.rotation[1], pose.rotation[2], pose.rotation[3]};
        g_build_set_position.call(tr, pos, g_build_set_position.method);
        g_set_rotation.call(tr, rot, g_set_rotation.method);
        if (replica.visual && !hide && !sync.active.call(replica.visual, sync.active.method))
            g_set_active.call(replica.visual, true, g_set_active.method);
        replica.last_written = g_get_position.call(tr, g_get_position.method);
        replica.written = true;
        replica.maximum_error = std::max(replica.maximum_error,
            std::sqrt(boomerang_distance_sq(replica.last_written, pos)));
        if (now >= sync.next_report) {
            log_boomerang_pose("apply", pose, age, replica.maximum_error, replica.maximum_drift,
                replica.corrections, replica.interval_drift, replica.visual != nullptr);
            replica.interval_drift = 0;
        }
        visuals += replica.visual != nullptr;
        ++applied;
        ++it;
    }
    if (now >= sync.next_report) {
        sync.next_report = now + 5;
        LOG_INFO("boomerang sync: guest local=%zu matched=%zu applied=%zu hidden=%zu visuals=%zu waiting=%zu visible=%zu limit=%zu AI=off",
            sync.replicas.size(), matched, applied, hidden, visuals, waiting, visible, fish_motion::kLiveFishLimit);
    }
}

void tick_boomerang_sync(const LocalState& local, bool diagnostic) try {
    auto& sync = g_boomerang;
    fish_motion::Id session{};
    uint32_t epoch = 0;
    const uint32_t host = net_session_host_id();
    const bool changed = sync.host && sync.generation != g_world_context_generation;
    const bool role_changed = sync.host && g_role_known &&
        (g_local_is_host != (sync.host == net_local_id()));
    if (boomerang_sync::restore_context(changed, role_changed, diagnostic)) reset_boomerang_sync();
    if (diagnostic || !local.valid || g_world_live_since <= 0 ||
        build_now() - g_world_live_since < kReidSettleSeconds) return;
    if (!g_role_known || !host || !net_geometry_session(session.data(), &epoch)) {
        // Loss of network membership does not start a second local simulation.
        if (sync.runtime_ready && sync.host && sync.host != net_local_id()) apply_boomerangs(local, build_now());
        return;
    }
    if (sync.session != session || sync.epoch != epoch || sync.host != host ||
        sync.generation != g_world_context_generation) {
        reset_boomerang_sync();
        sync.session = session;
        sync.epoch = epoch;
        sync.host = host;
        sync.generation = g_world_context_generation;
    }
    if (!probe_resolve() || !resolve_boomerang_sync() || !g_renderer_type_obj ||
        !g_unique_identifier_type || !g_unique_get_id || !g_unique_get_class_id ||
        !g_build_set_position || !g_set_rotation) return;
    sync.runtime_ready = true;
    const double now = build_now();
    if (host == net_local_id()) publish_boomerangs(local, now);
    else apply_boomerangs(local, now);
} catch (...) {
    reset_boomerang_sync();
    LOG_ERROR("boomerang sync: rolled back after native exception");
}
