// All Unity access in this file runs on Player.Update.
namespace sp = shared_progress;

struct SharedProgressApi {
    Il2CppClass* pod_class = nullptr;
    void* pod_main = nullptr;
    void* pod_health = nullptr;
    void* pod_storage = nullptr;
    void* pod_radio_spawner = nullptr;
    void* spawned_object = nullptr;
    void* radio_type = nullptr;
    void* radio_health = nullptr;
    void* door_type = nullptr;
    void* door_open = nullptr;
    void* door_locked = nullptr;
    void* door_sealed = nullptr;
    void* seal_amount = nullptr;
    Entry<GetBoolFn> pod_initialized, pod_intro;
    Entry<float (*)(void*, void*)> health_fraction, max_health;
    Entry<float (*)(void*, float, void*)> add_health;
    Entry<ManagedFn> pod_repair, pod_effects, radio_repair;
    Entry<ManagedFn> door_toggle, door_unlock;
    Entry<GetBoolFn> seal_closed;
    Entry<void (*)(void*, float, void*)> seal_weld;
    Entry<int (*)(void*, void*)> sibling_index;
    Entry<bool (*)(int32_t, void*)> knows_tech;
    Entry<bool (*)(int32_t, bool, void*)> add_tech;
    std::vector<int32_t> tech_types;
};
SharedProgressApi g_shared_api;

struct SharedDoor {
    bool seen = false;
    uint16_t last_local = 0;
    bool changed = false;
    bool has_update = false;
    bool pending = false;
    uint16_t desired = 0;
};

struct SharedRepair {
    float last_fraction = -1;
    float pending_fraction = 0;
    float repaired_fraction = 0;
    float sent_fraction = 0;
    double next_send = 0;
};

struct SharedProgressState {
    bool active = false;
    uint8_t session[16]{};
    uint32_t epoch = 0;
    uint32_t peer = 0;
    double next_scan = 0;
    double next_door_scan = 0;
    double next_radio_scan = 0;
    double next_radio_missing_log = 0;
    double radio_ready_since = 0;
    SharedRepair pod_repair, radio_repair;
    std::map<std::string, SharedDoor> doors;
    std::set<int32_t> announced_tech;
    std::vector<int32_t> incoming_tech;
    size_t next_tech = 0;
};
SharedProgressState g_shared_progress;
void reset_shared_progress() { g_shared_progress = {}; }

void shared_progress_bind() {
    auto& api = g_shared_api;
    api.pod_class = build_class("EscapePod");
    auto* health = build_class("LiveMixin");
    api.pod_main = build_field_on(api.pod_class, "main", 0);
    api.pod_health = build_field_on(api.pod_class, "liveMixin", 0x40);
    api.pod_storage = build_field_on(api.pod_class, "storageContainer", 0x98);
    api.pod_radio_spawner = build_field_on(api.pod_class, "radioSpawner", 0x58);
    api.spawned_object = build_field_on(build_class("PrefabSpawnBase"), "spawnedObj", 0x40);
    auto* radio = build_class("Radio");
    api.radio_type = radio ? il2cpp_type_get_object(il2cpp_class_get_type(radio)) : nullptr;
    api.radio_health = build_field_on(radio, "liveMixin", 0x48);
    api.radio_repair.bind(find_method(radio, "OnRepair", 0));
    api.pod_initialized.bind(find_method(api.pod_class, "get_initialized", 0));
    api.pod_intro.bind(find_method(api.pod_class, "IsPlayingIntroCinematic", 0));
    api.pod_repair.bind(find_method(api.pod_class, "OnRepair", 0));
    api.pod_effects.bind(find_method(api.pod_class, "UpdateDamagedEffects", 0));
    api.health_fraction.bind(find_method(health, "GetHealthFraction", 0));
    api.max_health.bind(find_method(health, "get_maxHealth", 0));
    api.add_health.bind(find_method(health, "AddHealth", 1));

    auto* door = build_class("StarshipDoor");
    auto* seal = build_class("Sealed");
    api.door_type = door ? il2cpp_type_get_object(il2cpp_class_get_type(door)) : nullptr;
    api.door_open = build_field_on(door, "doorOpen", 0x40);
    api.door_locked = build_field_on(door, "doorLocked", 0x41);
    api.door_sealed = build_field_on(door, "sealedComponent", 0x58);
    api.seal_amount = build_field_on(seal, "maxOpenedAmount", 0x30);
    api.door_toggle.bind(find_method(door, "OnDoorToggle", 0));
    api.door_unlock.bind(find_method(door, "UnlockDoor", 0));
    api.seal_closed.bind(find_method(seal, "IsSealed", 0));
    api.seal_weld.bind(find_method(seal, "Weld", 1));
    api.sibling_index.bind(find_method(find_class_in_domain("UnityEngine", "Transform"),
                                       "GetSiblingIndex", 0));

    auto* known = build_class("KnownTech");
    api.knows_tech.bind(find_method(known, "Contains", 1));
    api.add_tech.bind(find_method(known, "Add", 2));
    if (api.tech_types.empty()) {
        auto* tech = build_class("TechType");
        void* it = nullptr;
        if (tech) {
            for (auto* field = il2cpp_class_get_fields(tech, &it); field;
                 field = il2cpp_class_get_fields(tech, &it)) {
                const char* name = il2cpp_field_get_name(field);
                if (!name || !strcmp(name, "value__")) continue;
                int32_t value = 0;
                il2cpp_field_static_get_value(field, &value);
                if (value > 0) api.tech_types.push_back(value);
            }
            std::sort(api.tech_types.begin(), api.tech_types.end());
            api.tech_types.erase(std::unique(api.tech_types.begin(), api.tech_types.end()),
                                 api.tech_types.end());
        }
    }
}

template <class T>
T shared_field(void* object, void* field) {
    T value{};
    if (object && field) il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(object), field, &value);
    return value;
}

void* shared_pod() {
    void* pod = nullptr;
    if (g_shared_api.pod_main) il2cpp_field_static_get_value(g_shared_api.pod_main, &pod);
    return pod;
}

void* shared_pod_storage() {
    return shared_field<void*>(shared_pod(), g_shared_api.pod_storage);
}

bool shared_progress_context() {
    uint8_t session[16]{};
    uint32_t epoch = 0;
    if (!net_geometry_session(session, &epoch)) return false;
    auto& state = g_shared_progress;
    if (!state.active || state.epoch != epoch || memcmp(session, state.session, sizeof(session))) {
        state = {};
        memcpy(state.session, session, sizeof(session));
        state.active = true;
        state.epoch = epoch;
    }
    return true;
}

bool shared_progress_send(WorldEvent event) {
    if (!sp::valid(event) || !g_shared_progress.peer || net_pending_world_event_count() >= 48) return false;
    event.kind = kWorldEventSharedProgress;
    return net_try_post_world_event(event);
}

void shared_progress_receive(const WorldEvent& event) try {
    if (!sp::valid(event) || !shared_progress_context()) return;
    auto& state = g_shared_progress;
    switch (event.build_flags) {
    case sp::PodRepair:
        state.pod_repair.pending_fraction = std::max(state.pod_repair.pending_fraction,
                                                     event.constructed_amount);
        break;
    case sp::RadioRepair:
        state.radio_repair.pending_fraction = std::max(state.radio_repair.pending_fraction,
                                                       event.constructed_amount);
        LOG_INFO("shared progress: received lifepod radio repair fraction=%.3f",
                 event.constructed_amount);
        break;
    case sp::Door: {
        if (!g_local_is_host && event.origin_id != net_session_host_id()) return;
        const std::string id(event.object_id);
        auto it = state.doors.find(id);
        if (it == state.doors.end()) {
            if (state.doors.size() >= 256) return;
            it = state.doors.emplace(id, SharedDoor{}).first;
        }
        it->second.desired = event.reserved16;
        it->second.pending = true;
        break;
    }
    case sp::Blueprint:
        if (state.incoming_tech.size() < 512) state.incoming_tech.push_back(event.tech_type);
        break;
    }
} catch (...) { LOG_WARN("shared progress: could not store event"); }

void sync_shared_repair(SharedRepair& state, void* target, void* health,
                        const Entry<ManagedFn>& on_repair, const Entry<ManagedFn>* refresh_effects,
                        sp::Kind kind, double now, bool publish_initial = false) {
    const auto& api = g_shared_api;
    if (!g_build_alive || !build_live(health) || !on_repair) return;
    float local = api.health_fraction.call(health, api.health_fraction.method);
    if (!std::isfinite(local) || local < 0 || local > 1) return;
    const float requested = state.pending_fraction;
    if (requested > local + 0.0001f || (requested == 1 && local < 1)) {
        const float maximum = api.max_health.call(health, api.max_health.method);
        if (!std::isfinite(maximum) || maximum <= 0) return;
        const float before = local;
        // AddHealth clamps at max, so a full repair cannot round down to
        // just below the game's IsFullHealth check.
        api.add_health.call(health, requested == 1 ? maximum : (requested - local) * maximum,
                            api.add_health.method);
        local = api.health_fraction.call(health, api.health_fraction.method);
        if (!std::isfinite(local) || local < 0 || local > 1) return;
        if (local == 1) on_repair.call(target, on_repair.method);
        if (refresh_effects) refresh_effects->call(target, refresh_effects->method);
        if (local > before) {
            LOG_INFO("shared progress: %s repair %.3f -> %.3f",
                     kind == sp::RadioRepair ? "lifepod radio" : "lifepod", before, local);
        }
    }
    if (requested == 1 ? local == 1 : local + 0.0001f >= requested) state.pending_fraction = 0;
    if ((state.last_fraction < 0 && publish_initial) ||
        (state.last_fraction >= 0 && local > state.last_fraction + 0.0001f)) {
        state.repaired_fraction = local;
    } else if (local < state.last_fraction) {
        state.repaired_fraction = std::min(state.repaired_fraction, local);
        state.sent_fraction = std::min(state.sent_fraction, local);
    }
    state.last_fraction = local;
    if (state.repaired_fraction > state.sent_fraction && now >= state.next_send) {
        WorldEvent event{};
        event.build_flags = kind;
        event.constructed_amount = state.repaired_fraction;
        if (shared_progress_send(event)) state.sent_fraction = state.repaired_fraction;
        state.next_send = now + 0.5;
    }
}

// EscapePod saves its radio as a child prefab, independently of the spawner's
// runtime reference. The class ID is the stock radio prefab in 1.22.83315.
constexpr char kPodRadioClassId[] = "5c06baec-0539-4f26-817d-78443548cc52";

bool shared_radio_prefab(void* radio, void* pod_go) {
    if (!g_unique_identifier_type || !g_unique_get_class_id || g_get_component_is_string) return false;
    void* go = g_get_game_object.call(radio, g_get_game_object.method);
    bool matches = false;
    walk_parents(go, [&](void* node) {
        if (node == pod_go) return true;
        void* identifier = g_get_component.call(node, g_unique_identifier_type, g_get_component.method);
        char class_id[64]{};
        matches = identifier && string_to_ascii(
            g_unique_get_class_id.call(identifier, g_unique_get_class_id.method), class_id, sizeof(class_id)) &&
            strcmp(class_id, kPodRadioClassId) == 0;
        return matches;
    });
    return matches;
}

void* shared_radio_in_tree(void* root, bool saved_pod_tree) {
    const auto& api = g_shared_api;
    if (!build_live(root) || !g_get_components_in_children || !g_array_get_length || !g_array_get_value) {
        return nullptr;
    }
    void* children = g_get_components_in_children.call(root, api.radio_type, false,
                                                      g_get_components_in_children.method);
    const int count = children ? g_array_get_length.call(children, g_array_get_length.method) : 0;
    if (count > 16) return nullptr;
    void* match = nullptr;
    for (int i = 0; i < count; ++i) {
        void* radio = g_array_get_value.call(children, i, g_array_get_value.method);
        if (!build_live(radio) || (saved_pod_tree && !shared_radio_prefab(radio, root))) continue;
        if (match) return nullptr; // Ambiguous: do not repair a player-built radio.
        match = radio;
    }
    return match;
}

void shared_progress_pod(double now) {
    const auto& api = g_shared_api;
    if (!api.pod_initialized || !api.pod_intro || !api.health_fraction ||
        !api.max_health || !api.add_health || !g_build_alive) return;
    void* pod = shared_pod();
    if (!build_live(pod) || !api.pod_initialized.call(pod, api.pod_initialized.method) ||
        api.pod_intro.call(pod, api.pod_intro.method)) return;
    auto& state = g_shared_progress;
    sync_shared_repair(state.pod_repair, pod, shared_field<void*>(pod, api.pod_health),
                       api.pod_repair, api.pod_effects ? &api.pod_effects : nullptr, sp::PodRepair, now);

    if (now < state.next_radio_scan) return;
    state.next_radio_scan = now + 0.5;
    if (!api.radio_type || !api.radio_health || !g_get_component || !g_get_game_object) return;
    void* spawner = shared_field<void*>(pod, api.pod_radio_spawner);
    void* go = build_live(spawner) ? shared_field<void*>(spawner, api.spawned_object) : nullptr;
    void* radio = shared_radio_in_tree(go, false);
    const char* source = "spawner";
    if (!radio) {
        radio = shared_radio_in_tree(g_get_game_object.call(pod, g_get_game_object.method), true);
        source = "saved capsule child";
    }
    void* health = shared_field<void*>(radio, api.radio_health);
    if (!build_live(radio) || !build_live(health)) {
        state.radio_ready_since = 0;
        if (now >= state.next_radio_missing_log) {
            LOG_WARN("shared progress: lifepod radio not resolved (spawner=%u spawned=%u radio=%u pending=%.3f)",
                     unsigned(build_live(spawner)), unsigned(build_live(go)), unsigned(build_live(radio)),
                     state.radio_repair.pending_fraction);
            state.next_radio_missing_log = now + 15;
        }
        return;
    }
    if (!state.radio_ready_since) {
        state.radio_ready_since = now;
        LOG_INFO("shared progress: lifepod radio found via %s fraction=%.3f",
                 source, api.health_fraction.call(health, api.health_fraction.method));
    }
    // Wait for the native spawn and scripted intro damage before publishing
    // an already repaired radio from the host's save.
    if (now - state.radio_ready_since < 2) return;
    sync_shared_repair(state.radio_repair, radio, health,
                       api.radio_repair, nullptr, sp::RadioRepair, now, g_local_is_host);
}

std::string shared_door_id(void* door) {
    const auto& api = g_shared_api;
    if (!api.sibling_index || !g_transform_get_parent || !g_gameobject_get_transform ||
        !g_unique_get_id || !g_unique_identifier_type) return {};
    void* go = g_get_game_object.call(door, g_get_game_object.method);
    int children[12]{};
    size_t count = 0;
    while (go) {
        void* uid = g_get_component.call(go, g_unique_identifier_type, g_get_component.method);
        if (uid) {
            char root[kWorldEventObjectIdSize]{};
            if (!string_to_ascii(g_unique_get_id.call(uid, g_unique_get_id.method), root, sizeof(root))) return {};
            std::string key(root);
            while (count) key += "/" + std::to_string(children[--count]);
            return key.size() < kWorldEventObjectIdSize ? key : std::string{};
        }
        if (count == std::size(children)) return {};
        void* tr = g_gameobject_get_transform.call(go, g_gameobject_get_transform.method);
        if (!tr) return {};
        children[count++] = api.sibling_index.call(tr, api.sibling_index.method);
        void* parent = g_transform_get_parent.call(tr, g_transform_get_parent.method);
        go = parent ? g_get_game_object.call(parent, g_get_game_object.method) : nullptr;
    }
    return {};
}

uint16_t shared_door_state(void* door) {
    const auto& api = g_shared_api;
    uint16_t state = shared_field<bool>(door, api.door_open) ? sp::Open : 0;
    if (!shared_field<bool>(door, api.door_locked)) state |= sp::Unlocked;
    void* seal = shared_field<void*>(door, api.door_sealed);
    if (!seal || !api.seal_closed.call(seal, api.seal_closed.method)) state |= sp::Unsealed;
    return state;
}

uint16_t shared_door_apply(void* door, uint16_t current, uint16_t desired) {
    const auto& api = g_shared_api;
    if ((desired & sp::Unlocked) && !(current & sp::Unlocked)) {
        api.door_unlock.call(door, api.door_unlock.method);
    }
    if ((desired & sp::Unsealed) && !(current & sp::Unsealed)) {
        void* seal = shared_field<void*>(door, api.door_sealed);
        const float amount = shared_field<float>(seal, api.seal_amount);
        if (amount > 0 && std::isfinite(amount)) api.seal_weld.call(seal, amount, api.seal_weld.method);
    }
    current = shared_door_state(door);
    if ((current & (sp::Unlocked | sp::Unsealed)) == (sp::Unlocked | sp::Unsealed) &&
        ((current ^ desired) & sp::Open)) api.door_toggle.call(door, api.door_toggle.method);
    return shared_door_state(door);
}

void shared_progress_doors() {
    const auto& api = g_shared_api;
    if (!api.door_type || !api.door_open || !api.door_locked || !api.door_sealed ||
        !api.seal_amount || !api.seal_closed || !api.seal_weld ||
        !api.door_unlock || !api.door_toggle || !g_find_objects_of_type ||
        !g_array_get_length || !g_array_get_value) return;
    void* doors = g_find_objects_of_type.call(api.door_type, g_find_objects_of_type.method);
    if (!doors) return;
    const int count = g_array_get_length.call(doors, g_array_get_length.method);
    auto& state = g_shared_progress;
    for (int i = 0; i < count; ++i) {
        void* door = g_array_get_value.call(doors, i, g_array_get_value.method);
        if (!door) continue;
        const std::string id = shared_door_id(door);
        if (id.empty()) continue;
        auto it = state.doors.find(id);
        if (it == state.doors.end()) {
            if (state.doors.size() >= 256) continue;
            it = state.doors.emplace(id, SharedDoor{}).first;
        }
        auto& record = it->second;
        uint16_t local = shared_door_state(door);
        if (record.pending) {
            const uint16_t old = local;
            local = shared_door_apply(door, local, record.desired);
            record.changed = g_local_is_host;
            record.pending = false;
            record.last_local = local;
            if (local != old) LOG_INFO("shared progress: wreck door state %u -> %u", old, local);
        }
        if (record.seen && record.last_local != local) record.changed = true;
        record.last_local = local;
        record.seen = true;
        if (record.changed) record.has_update = true;
    }
    for (auto& [id, record] : state.doors) {
        if (!record.changed || record.pending) continue;
        WorldEvent event{};
        event.build_flags = sp::Door;
        event.reserved16 = record.last_local;
        memcpy(event.object_id, id.c_str(), id.size() + 1);
        if (shared_progress_send(event)) record.changed = false;
    }
}

void shared_progress_blueprints() {
    const auto& api = g_shared_api;
    auto& state = g_shared_progress;
    if (api.tech_types.empty() || !api.knows_tech || !api.add_tech) return;
    for (int32_t tech : state.incoming_tech) {
        if (!std::binary_search(api.tech_types.begin(), api.tech_types.end(), tech)) continue;
        if (!api.knows_tech.call(tech, api.knows_tech.method) &&
            api.add_tech.call(tech, false, api.add_tech.method)) {
            LOG_INFO("shared progress: learned blueprint tech=%d", tech);
        }
        state.announced_tech.insert(tech);
    }
    state.incoming_tech.clear();
    if (!state.peer) return;
    for (int tried = 0, sent = 0; tried < 128 && sent < 6; ++tried) {
        const int32_t tech = api.tech_types[state.next_tech];
        if (!state.announced_tech.count(tech) && api.knows_tech.call(tech, api.knows_tech.method)) {
            WorldEvent event{};
            event.build_flags = sp::Blueprint;
            event.tech_type = tech;
            if (!shared_progress_send(event)) break;
            state.announced_tech.insert(tech);
            ++sent;
        }
        state.next_tech = (state.next_tech + 1) % api.tech_types.size();
    }
}

void tick_shared_progress(const LocalState& local) try {
    if (!local.valid || !g_role_known || !shared_progress_context()) return;
    auto& state = g_shared_progress;
    const double now = build_now();
    if (now < state.next_scan) return;
    state.next_scan = now + 0.25;
    if (!g_shared_api.pod_class || !g_shared_api.radio_type || !g_shared_api.door_type ||
        g_shared_api.tech_types.empty()) {
        static double next_bind = 0;
        if (now >= next_bind) {
            next_bind = now + 5;
            shared_progress_bind();
        }
    }
    PeerState peer{};
    if (net_copy_world_peers(&peer, 1) && peer.id != state.peer) {
        state.peer = peer.id;
        state.pod_repair.sent_fraction = 0;
        state.radio_repair.sent_fraction = 0;
        state.announced_tech.clear();
        state.next_tech = 0;
        if (g_local_is_host) {
            for (auto& entry : state.doors) entry.second.changed = entry.second.has_update;
        }
    }
    shared_progress_pod(now);
    if (now >= state.next_door_scan) {
        state.next_door_scan = now + 0.75;
        shared_progress_doors();
    }
    shared_progress_blueprints();
} catch (...) { LOG_WARN("shared progress: game tick deferred"); }
