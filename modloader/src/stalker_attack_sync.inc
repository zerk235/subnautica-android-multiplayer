// Stalkers use the host's simulation. Only the victim's own game applies damage.
struct StalkerApi {
    void* type = nullptr;
    void* melee_type = nullptr;
    void* aggression_type = nullptr;
    void* swim_type = nullptr;
    void* action_type = nullptr;
    void* player_live = nullptr;
    void* damage_dealer = nullptr;
    void* damage_amount = nullptr;
    void* damage_type = nullptr;
    void* mouth = nullptr;
    void* last_target = nullptr;
    void* last_bite_time = nullptr;
    void* bite_interval = nullptr;
    void* bite_damage = nullptr;
    void* can_bite_player = nullptr;
    void* frozen = nullptr;
    void* swim_velocity = nullptr;
    void* remember_target_time = nullptr;
    void* friendly = nullptr;
    Entry<void* (*)(void*, void*)> get_target;
    Entry<float (*)(void*, void*)> get_target_time;
    Entry<bool (*)(void*, void*, void*)> can_see;
    Entry<void (*)(void*, Vector3, float, void*)> swim_to;
    Entry<float (*)(void*)> game_time;
    Entry<bool (*)(void*, float, Vector3, int, void*, void*)> take_damage;
    void (*original_player_damage)(void*, void*, void*) = nullptr;
    bool ready = false;
} g_stalker;

struct StalkerHit {
    fish_motion::Bite bite;
    double until;
};
std::map<fish_motion::Id, StalkerHit> g_stalker_hits;
uint32_t g_stalker_bite_serial = 0;
thread_local bool g_stalker_unity_thread = false;

bool patch_managed_vtable_slot(Il2CppClass* klass, Il2CppMethodInfo* method, unsigned expected_slot,
                               void* replacement, void** original);

template<class T>
T stalker_field(void* object, void* field) {
    T value{};
    if (object && field)
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(object), field, &value);
    return value;
}

void* stalker_type() {
    return g_stalker.melee_type && g_stalker.aggression_type ? g_stalker.type : nullptr;
}

void* stalker_from_dealer(void* dealer) {
    if (!probe_exists(dealer)) return nullptr;
    void* stalker = nullptr;
    walk_parents(dealer, [&](void* go) {
        stalker = g_get_component.call(go, g_stalker.type, g_get_component.method);
        return probe_exists(stalker);
    });
    return probe_exists(stalker) ? stalker : nullptr;
}

void record_stalker_hit(const fish_motion::Id& id, uint32_t victim, uint32_t victim_pose,
                        float damage, double now) {
    for (auto it = g_stalker_hits.begin(); it != g_stalker_hits.end();) {
        if (it->second.until < now) it = g_stalker_hits.erase(it);
        else ++it;
    }
    if (++g_stalker_bite_serial == 0) ++g_stalker_bite_serial;
    g_stalker_hits[id] = {{g_stalker_bite_serial, victim, victim_pose, damage}, now + 0.6};
    LOG_INFO("stalker: bite victim=%08x serial=%u damage_to_guest=%.1f",
             victim, g_stalker_bite_serial, damage);
}

void hooked_stalker_player_damage(void* self, void* info, void* method) {
    const auto& sync = g_boomerang;
    if (g_stalker_unity_thread && g_stalker.ready && self == g_local_player && info &&
        sync.host && sync.host == net_local_id() &&
        sync.generation == g_world_context_generation) {
        try {
            const float damage = stalker_field<float>(info, g_stalker.damage_amount);
            if (damage > 0 && stalker_field<int>(info, g_stalker.damage_type) == 0) {
                void* dealer = stalker_field<void*>(info, g_stalker.damage_dealer);
                void* stalker = stalker_from_dealer(dealer);
                if (stalker && world_operations_ready()) {
                    void* go = g_get_game_object.call(stalker, g_get_game_object.method);
                    fish_motion::Message identity;
                    void* uid = g_get_component.call(go, g_unique_identifier_type, g_get_component.method);
                    if (boomerang_identity(uid, identity))
                        record_stalker_hit(identity.fish, sync.host, 0, 0, build_now());
                }
            }
        } catch (...) {
            LOG_WARN("stalker: failed to queue native bite animation");
        }
    }
    g_stalker.original_player_damage(self, info, method);
}

bool resolve_stalker_attack() {
    auto& a = g_stalker;
    if (a.ready) { g_stalker_unity_thread = true; return true; }
    static double next_attempt = 0;
    const double now = build_now();
    if (now < next_attempt) return false;
    next_attempt = now + 10;
    if (!il2cpp_field_get_value || !il2cpp_field_set_value || !il2cpp_class_get_fields ||
        !il2cpp_field_get_name || !il2cpp_field_get_offset || !g_get_component ||
        g_get_component_is_string || !g_get_go_transform || !g_get_position) return false;

    auto* stalker = build_class("Stalker");
    auto* melee = build_class("MeleeAttack");
    auto* swim = build_class("SwimBehaviour");
    auto* action = build_class("AttackLastTarget");
    auto* aggressor = build_class("AggressiveWhenSeeTarget");
    if (!stalker || !melee || !swim || !action || !aggressor) return false;
    a.type = il2cpp_type_get_object(il2cpp_class_get_type(stalker));
    a.melee_type = il2cpp_type_get_object(il2cpp_class_get_type(melee));
    a.swim_type = il2cpp_type_get_object(il2cpp_class_get_type(swim));
    a.action_type = il2cpp_type_get_object(il2cpp_class_get_type(action));
    a.aggression_type = il2cpp_type_get_object(il2cpp_class_get_type(aggressor));
    auto* player = build_class("Player");
    auto* damage = build_class("DamageInfo");
    a.player_live = build_field_on(player, "liveMixin", 0xe8);
    a.damage_dealer = build_field_on(damage, "dealer", 0x28);
    a.damage_amount = build_field_on(damage, "damage", 0x14);
    a.damage_type = build_field_on(damage, "type", 0x24);
    a.mouth = build_field_on(melee, "mouth", 0x48);
    a.last_target = build_field_on(melee, "lastTarget", 0x50);
    a.last_bite_time = build_field_on(melee, "timeLastBite", 0x2c);
    a.bite_interval = build_field_on(melee, "biteInterval", 0x24);
    a.bite_damage = build_field_on(melee, "biteDamage", 0x28);
    a.can_bite_player = build_field_on(melee, "canBitePlayer", 0x7b);
    a.frozen = build_field_on(melee, "frozen", 0x7e);
    a.swim_velocity = build_field_on(action, "swimVelocity", 0x4c);
    a.remember_target_time = build_field_on(action, "rememberTargetTime", 0x64);
    a.friendly = build_field_on(build_class("Creature"), "_friendlyToPlayer", 0x10c);
    auto* last_target = build_class("LastTarget");
    a.get_target.bind(find_method(last_target, "get_target", 0));
    a.get_target_time.bind(find_method(last_target, "get_targetTime", 0));
    a.can_see.bind(find_method(build_class("Creature"), "GetCanSeeObject", 1));
    a.swim_to.bind(find_method_typed(swim, "SwimTo", 2, "unityengine.vector3"));
    a.game_time.bind(find_method(find_class_in_domain("UnityEngine", "Time"), "get_time", 0));
    a.take_damage.bind(find_method_typed(build_class("LiveMixin"), "TakeDamage", 4, "system.single"));
    if (!a.type || !a.melee_type || !a.swim_type || !a.action_type || !a.aggression_type ||
        !a.player_live || !a.damage_dealer || !a.damage_amount || !a.damage_type ||
        !a.mouth || !a.last_target || !a.last_bite_time ||
        !a.bite_interval || !a.bite_damage || !a.can_bite_player || !a.frozen ||
        !a.swim_velocity || !a.remember_target_time || !a.friendly || !a.get_target ||
        !a.get_target_time || !a.can_see || !a.swim_to || !a.game_time ||
        !a.take_damage) {
        LOG_WARN("stalker: game API incomplete");
        return false;
    }
    // LiveMixin notifies Player via IOnTakeDamage dispatch after applying the hit.
    // Capture that virtual callback. MeleeAttack.OnTouch checks Behaviour.enabled,
    // so the guest replica's disabled MeleeAttack cannot apply native bites.
    auto* on_damage = find_method_typed(player, "OnTakeDamage", 1, "damageinfo");
    void* original = reinterpret_cast<void*>(a.original_player_damage);
    a.ready = patch_managed_vtable_slot(player, on_damage, 9,
        reinterpret_cast<void*>(&hooked_stalker_player_damage), &original);
    a.original_player_damage = reinterpret_cast<decltype(a.original_player_damage)>(original);
    g_stalker_unity_thread = a.ready;
    LOG_INFO("stalker: movement=%u player damage callback=%u",
             unsigned(stalker_type() != nullptr), unsigned(a.ready));
    return a.ready;
}

void add_stalker_behaviours(void* go, std::vector<BoomerangSaved>& behaviours) {
    // CreatureAction excludes these two components. Disable and restore them with the replica.
    for (void* type : {g_stalker.melee_type, g_stalker.aggression_type}) {
        void* list = g_creature_probe.components.call(go, type, true, g_creature_probe.components.method);
        const int count = list ? g_array_get_length.call(list, g_array_get_length.method) : 0;
        for (int index = 0; index < count && index < 16; ++index) {
            void* component = g_array_get_value.call(list, index, g_array_get_value.method);
            if (probe_exists(component))
                behaviours.push_back({component, g_behaviour_get_enabled.call(component, g_behaviour_get_enabled.method)});
        }
    }
}

bool stalker_exposed(uint32_t flags) {
    return (flags & kFlagSwimming) && !(flags & (kFlagInside | kFlagPiloting));
}

void stalker_host_tick(const HostFishSample& sample, const LocalState& local,
                       const PeerState* peers, size_t count, Vector3 position, double now,
                       fish_motion::Message& pose) {
    const auto& a = g_stalker;
    const auto send_recent_bite = [&]() {
        const auto hit = g_stalker_hits.find(pose.fish);
        if (hit != g_stalker_hits.end() && now <= hit->second.until) pose.bite = hit->second.bite;
    };
    send_recent_bite();
    if (pose.bite.serial || !a.ready || stalker_field<bool>(sample.fish, a.friendly) ||
        !g_boomerang.get_time_scale ||
        !(g_boomerang.get_time_scale.call(g_boomerang.get_time_scale.method) > 0)) return;

    const PeerState* target = nullptr;
    float best_distance = 100.0f;
    if (stalker_exposed(local.flags)) best_distance = std::min(best_distance, boomerang_distance_sq(position, local));
    for (size_t i = 0; i < count; ++i) {
        const auto& peer = peers[i];
        const double age = now - peer.pose_received_at;
        if (!peer.seq_valid || !peer.sequence || !stalker_exposed(peer.flags) ||
            !(age >= 0 && age <= 0.30)) continue;
        const float distance = boomerang_distance_sq(position, Vector3{peer.x, peer.y, peer.z});
        if (distance < best_distance) { target = &peer; best_distance = distance; }
    }
    if (!target) return;
    auto* avatar = find_avatar(target->id);
    if (!avatar || !probe_exists(avatar->game_object) ||
        !a.can_see.call(sample.fish, avatar->game_object, a.can_see.method)) return;

    void* go = sample.game_object;
    void* melee = find_component_in_hierarchy(go, a.melee_type, nullptr, 0);
    if (!probe_exists(melee) || !g_behaviour_get_enabled.call(melee, g_behaviour_get_enabled.method) ||
        !stalker_field<bool>(melee, a.can_bite_player) || stalker_field<bool>(melee, a.frozen)) return;
    void* last_target = stalker_field<void*>(melee, a.last_target);
    void* action = find_component_in_hierarchy(go, a.action_type, nullptr, 0);
    float game_time = a.game_time.call(a.game_time.method);
    if (!probe_exists(last_target) || !probe_exists(action) || !std::isfinite(game_time)) return;
    if (probe_exists(a.get_target.call(last_target, a.get_target.method))) {
        const float target_age = game_time - a.get_target_time.call(last_target, a.get_target_time.method);
        const float remember_time = stalker_field<float>(action, a.remember_target_time);
        if (!std::isfinite(target_age) || !std::isfinite(remember_time) ||
            target_age < remember_time) return;
    }

    const Vector3 destination{target->x, target->y, target->z};
    void* mouth = stalker_field<void*>(melee, a.mouth);
    if (!probe_exists(mouth)) return;
    void* mouth_transform = g_get_go_transform.call(mouth, g_get_go_transform.method);
    if (!probe_exists(mouth_transform)) return;
    const Vector3 mouth_position = g_get_position.call(mouth_transform, g_get_position.method);
    if (boomerang_distance_sq(mouth_position, destination) > 3.24f) {
        void* swim = g_get_component.call(go, a.swim_type, g_get_component.method);

        const float velocity = stalker_field<float>(action, a.swim_velocity);
        if (probe_exists(swim) && velocity > 0 && velocity < 40)
            a.swim_to.call(swim, destination, velocity, a.swim_to.method);
        return;
    }

    const float last_bite = stalker_field<float>(melee, a.last_bite_time);
    const float cooldown = stalker_field<float>(melee, a.bite_interval);
    const float damage = stalker_field<float>(melee, a.bite_damage);
    if (!std::isfinite(last_bite) ||
        !(cooldown > 0 && game_time - last_bite >= std::max(cooldown, 0.7f)) ||
        !(damage > 0 && damage <= fish_motion::kMaxBiteDamage)) return;

    record_stalker_hit(pose.fish, target->id, target->sequence, damage, now);
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(melee), a.last_bite_time, &game_time);
    send_recent_bite();
}

void set_stalker_bite_animation(void* go, bool playing) {
    if (!probe_exists(go) || !g_anim_set_bool || !g_get_anim_params) return;
    void* list = g_creature_probe.components.call(go, g_animator_type_obj, true,
                                                  g_creature_probe.components.method);
    const int count = list ? g_array_get_length.call(list, g_array_get_length.method) : 0;
    for (int index = 0; index < count && index < 16; ++index) {
        void* animator = g_array_get_value.call(list, index, g_array_get_value.method);
        if (!probe_exists(animator)) continue;
        void* params = g_get_anim_params.call(animator, g_get_anim_params.method);
        const int length = params ? g_array_get_length.call(params, g_array_get_length.method) : 0;
        for (int i = 0; i < length && i < 192; ++i) {
            auto* param = static_cast<char*>(g_array_get_value.call(params, i, g_array_get_value.method));
            char name[32];
            int type;
            if (read_anim_param(param, name, sizeof(name), &type, false) &&
                type == kAnimParamBool && !strcmp(name, "bite")) {
                g_anim_set_bool.call(animator, cached_managed_string("bite"), playing, g_anim_set_bool.method);
                break;
            }
        }
    }
}

void stalker_guest_tick(BoomerangReplica& replica, const LocalState& local,
                       double now, bool visible) {
    if (replica.identity.species != fish_motion::Species::Stalker) return;
    void* go = replica.visual ? replica.visual :
        (probe_exists(replica.fish) ? g_get_game_object.call(replica.fish, g_get_game_object.method) : nullptr);
    if (!probe_exists(go)) return;

    if (visible && replica.pending_bite.serial) {
        const auto bite = replica.pending_bite;
        replica.pending_bite = {};
        if (now - replica.bite_received_at <= 0.45 && net_session_state() == kSessionGuest) {
            set_stalker_bite_animation(go, true);
            replica.bite_animation_until = now + 0.40;
            const auto& a = g_stalker;
            const double pose_age = net_sent_pose_age(replica.identity.host, bite.victim_pose);
            const Vector3 origin = replica.bite_position;
            if (fish_motion::bite_reaches_player(bite, net_local_id(), stalker_exposed(local.flags),
                                                pose_age, boomerang_distance_sq(origin, local))) {
                void* health = stalker_field<void*>(g_local_player, a.player_live);
                const auto& alive = g_creature_probe.alive;
                if (a.ready && probe_exists(health) && alive.call(health, alive.method)) {
                    a.take_damage.call(health, bite.damage, {}, 0, go, a.take_damage.method);
                    LOG_INFO("stalker: applied host bite serial=%u damage=%.1f", bite.serial, bite.damage);
                }
            }
        }
    }
    if (replica.bite_animation_until && (!visible || now >= replica.bite_animation_until)) {
        set_stalker_bite_animation(go, false);
        replica.bite_animation_until = 0;
    }
}

void reset_stalker_attacks() { g_stalker_hits.clear(); }
