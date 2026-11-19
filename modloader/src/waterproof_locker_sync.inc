// Portable locker motion updates the game object in place so its storage stays intact.
constexpr uint32_t kPropulsionCannonTech = 757;
constexpr double kLockerMotionInterval = 0.15;

void* waterproof_pickup(void* go) {
    if (!build_live(go) || !g_pickupable_type || !g_pickup_get_tech ||
        !g_get_component || g_get_component_is_string) return nullptr;
    void* pickup = g_get_component.call(go, g_pickupable_type, g_get_component.method);
    return build_live(pickup) &&
           g_pickup_get_tech.call(pickup, g_pickup_get_tech.method) == item_drop_sync::kWaterproofLocker
        ? pickup : nullptr;
}

void* waterproof_find(const char* id) {
    if (!g_unique_try_get || !g_get_game_object) return nullptr;
    ManagedRoot key;
    if (!prefab_retain(key, il2cpp_string_new(id))) return nullptr;
    void* registered = nullptr;
    if (!g_unique_try_get.call(key.get(), &registered, g_unique_try_get.method) ||
        !build_live(registered)) return nullptr;
    return waterproof_pickup(g_get_game_object.call(registered, g_get_game_object.method));
}

void* waterproof_world_object(void* pickup) {
    if (!build_live(pickup)) return nullptr;
    void* go = g_get_game_object.call(pickup, g_get_game_object.method);
    if (!go || !g_go_get_active_self.call(go, g_go_get_active_self.method) ||
        (g_pickup_get_attached && g_pickup_get_attached.call(pickup, g_pickup_get_attached.method)) ||
        object_in_local_inventory(go)) return nullptr;
    return go;
}

struct WaterproofMotionApi {
    void* weapon_type = nullptr;
    void* cannon_field = nullptr;
    Entry<BuildObjectFn> grabbed;
    Entry<ManagedFn> release;
} g_waterproof_motion_api;

void* waterproof_cannon(void* held) {
    auto& api = g_waterproof_motion_api;
    if (!api.weapon_type || !api.grabbed || !api.release || !api.cannon_field) {
        auto* weapon = build_class("PropulsionCannonWeapon");
        auto* cannon = build_class("PropulsionCannon");
        if (!weapon || !cannon) return nullptr;
        api.weapon_type = il2cpp_type_get_object(il2cpp_class_get_type(weapon));
        api.cannon_field = build_field_on(weapon, "propulsionCannon", 0x110);
        api.grabbed.bind(find_method(cannon, "get_grabbedObject", 0));
        api.release.bind(find_method(cannon, "ReleaseGrabbedObject", 0));
        if (!api.weapon_type || !api.grabbed || !api.release || !api.cannon_field)
            return nullptr;
        LOG_INFO("waterproof locker: propulsion API ready");
    }
    void* go = g_get_game_object.call(held, g_get_game_object.method);
    void* weapon = go ? g_get_component.call(go, api.weapon_type, g_get_component.method) : nullptr;
    void* cannon = nullptr;
    if (build_live(weapon))
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(weapon), api.cannon_field, &cannon);
    return build_live(cannon) ? cannon : nullptr;
}

struct WaterproofRemoteMotion {
    WorldEvent pose{};
    ManagedRoot pickup;
    ManagedRoot body;
    double arrived = 0;
    double retry_at = 0;
    bool native_kinematic = false;
};
std::map<std::string, WaterproofRemoteMotion> g_waterproof_remote;
struct WaterproofLocalMotion {
    ManagedRoot pickup;
    char id[kWorldEventObjectIdSize]{};
    double next_send = 0;
    double released_at = 0;
    bool sent = false;
} g_waterproof_local;
Entry<SetVectorFn> g_waterproof_set_velocity;

void waterproof_restore_physics(WaterproofRemoteMotion& motion, bool release) {
    if (void* body = motion.body.get(); build_live(body)) {
        g_rb_set_kinematic.call(body, motion.native_kinematic, g_rb_set_kinematic.method);
        if (release && !motion.native_kinematic && g_waterproof_set_velocity) {
            const auto& p = motion.pose;
            g_waterproof_set_velocity.call(body, {p.vx, p.vy, p.vz}, g_waterproof_set_velocity.method);
        }
    }
    motion.body.clear();
}

void reset_waterproof_motion() {
    for (auto& entry : g_waterproof_remote) waterproof_restore_physics(entry.second, false);
    g_waterproof_remote.clear();
    g_waterproof_local.pickup.clear();
    g_waterproof_local.id[0] = '\0';
    g_waterproof_local.next_send = g_waterproof_local.released_at = 0;
    g_waterproof_local.sent = false;
}

void waterproof_before_item_event(const WorldEvent& event) {
    auto it = g_waterproof_remote.find(event.object_id);
    if (it == g_waterproof_remote.end()) return;
    auto& motion = it->second;
    waterproof_restore_physics(motion, false);
    motion.pickup.clear();
    if (motion.pose.origin_id != event.origin_id || motion.pose.sequence <= event.sequence)
        g_waterproof_remote.erase(it);
}

void waterproof_receive_motion(const WorldEvent& event) {
    if (!item_drop_sync::valid_locker_motion(event)) return;
    uint8_t guid[16];
    guid_from_string(event.object_id, guid);
    if (memcmp(guid, event.guid, sizeof(guid))) return;
    auto it = g_waterproof_remote.find(event.object_id);
    if (it == g_waterproof_remote.end()) {
        if (g_waterproof_remote.size() >= 16) return;
        it = g_waterproof_remote.try_emplace(event.object_id).first;
    }
    it->second.pose = event;
    it->second.arrived = monotonic_seconds();
}

Quaternion waterproof_blend_rotation(Quaternion from, const WorldEvent& to, float amount) {
    const float dot = from.x * to.qx + from.y * to.qy + from.z * to.qz + from.w * to.qw;
    const float signed_amount = dot < 0 ? -amount : amount;
    const float rest = 1.0f - amount;
    Quaternion q{from.x * rest + to.qx * signed_amount, from.y * rest + to.qy * signed_amount,
                 from.z * rest + to.qz * signed_amount, from.w * rest + to.qw * signed_amount};
    const float length = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    return length > 0.001f ? Quaternion{q.x/length, q.y/length, q.z/length, q.w/length}
                           : Quaternion{to.qx, to.qy, to.qz, to.qw};
}

bool waterproof_apply_motion(WaterproofRemoteMotion& motion, double now, float blend) {
    const auto& pose = motion.pose;
    const bool moving = pose.build_flags == item_drop_sync::kLockerMoving;
    if (now - motion.arrived > (motion.pickup.get() ? 1.25 : 4.0)) return false;
    if (!motion.pickup.get()) {
        if (now < motion.retry_at) return true;
        motion.retry_at = now + 0.25;
        void* pickup = waterproof_find(pose.object_id);
        if (!waterproof_world_object(pickup) || !prefab_retain(motion.pickup, pickup)) return true;
    }
    void* go = waterproof_world_object(motion.pickup.get());
    if (!go) return false;
    void* tr = g_get_go_transform.call(go, g_get_go_transform.method);
    if (!tr) return false;
    if (moving && !motion.body.get()) {
        void* body = g_get_component.call(go, g_rigidbody_type, g_get_component.method);
        if (build_live(body) && prefab_retain(motion.body, body)) {
            motion.native_kinematic = g_rb_get_kinematic.call(body, g_rb_get_kinematic.method);
            g_rb_set_kinematic.call(body, true, g_rb_set_kinematic.method);
        }
    }
    if (moving) {
        const auto p = g_get_position.call(tr, g_get_position.method);
        const auto q = g_get_rotation.call(tr, g_get_rotation.method);
        g_set_position.call(tr, {p.x + (pose.x-p.x)*blend, p.y + (pose.y-p.y)*blend,
                                 p.z + (pose.z-p.z)*blend}, g_set_position.method);
        g_set_rotation.call(tr, waterproof_blend_rotation(q, pose, blend), g_set_rotation.method);
        return true;
    }
    g_set_position.call(tr, {pose.x, pose.y, pose.z}, g_set_position.method);
    g_set_rotation.call(tr, {pose.qx, pose.qy, pose.qz, pose.qw}, g_set_rotation.method);
    if (!motion.body.get()) {
        // A quick throw can send only the release sample.
        void* body = g_get_component.call(go, g_rigidbody_type, g_get_component.method);
        if (build_live(body) && prefab_retain(motion.body, body))
            motion.native_kinematic = g_rb_get_kinematic.call(body, g_rb_get_kinematic.method);
    }
    waterproof_restore_physics(motion, true);
    LOG_INFO("waterproof locker: motion released id=%s peer=%08x", pose.object_id, pose.origin_id);
    return false;
}

bool waterproof_send_motion(void* pickup, const char* id, bool moving) {
    void* go = waterproof_world_object(pickup);
    if (!go) return false;
    Vector3 p{}, velocity{};
    Quaternion q{};
    pickup_pose(pickup, &p, &q);
    if (void* body = g_get_component.call(go, g_rigidbody_type, g_get_component.method);
        build_live(body) && g_rb_get_velocity)
        velocity = g_rb_get_velocity.call(body, g_rb_get_velocity.method);
    WorldEvent event{};
    event.kind = kWorldEventLockerMotion;
    event.tech_type = item_drop_sync::kWaterproofLocker;
    event.build_flags = moving ? item_drop_sync::kLockerMoving : 0;
    snprintf(event.object_id, sizeof(event.object_id), "%s", id);
    guid_from_string(event.object_id, event.guid);
    event.x = p.x; event.y = p.y; event.z = p.z;
    event.qx = q.x; event.qy = q.y; event.qz = q.z; event.qw = q.w;
    event.vx = velocity.x; event.vy = velocity.y; event.vz = velocity.z;
    return item_drop_sync::valid_locker_motion(event) && net_try_post_world_event(event);
}

void tick_waterproof_motion(const LocalState& local) {
    if (!local.valid || !g_role_known || !net_peer_count()) {
        if (!g_waterproof_remote.empty() || g_waterproof_local.pickup.get()) reset_waterproof_motion();
        return;
    }
    if (!g_build_alive || !g_get_component || g_get_component_is_string ||
        !g_get_go_transform || !g_get_position || !g_set_position ||
        !g_get_rotation || !g_set_rotation || !g_rigidbody_type || !g_rb_get_kinematic ||
        !g_rb_set_kinematic || !g_go_get_active_self) return;
    auto& motion = g_waterproof_local;
    if (local.held_tech != kPropulsionCannonTech && !motion.pickup.get() &&
        g_waterproof_remote.empty()) return;

    if (!g_waterproof_set_velocity) {
        auto* rigidbody = find_class_in_domain("UnityEngine", "Rigidbody");
        g_waterproof_set_velocity.bind(find_method(rigidbody, "set_linearVelocity", 1));
        if (!g_waterproof_set_velocity) g_waterproof_set_velocity.bind(find_method(rigidbody, "set_velocity", 1));
    }
    void* cannon = nullptr;
    void* grabbed = nullptr;
    char id[kWorldEventObjectIdSize]{};
    if (local.held_tech == kPropulsionCannonTech && g_inventory_get_held) {
        void* inventory = read_static_object_field(g_inventory_class, "main");
        void* held = inventory ? g_inventory_get_held.call(inventory, g_inventory_get_held.method) : nullptr;
        cannon = held ? waterproof_cannon(held) : nullptr;
        if (cannon) {
            auto& api = g_waterproof_motion_api;
            void* go = api.grabbed.call(cannon, api.grabbed.method);
            if (build_live(go)) {
                walk_parents(go, [&](void* node) {
                    grabbed = waterproof_pickup(node);
                    return grabbed != nullptr;
                });
            }
            if (!waterproof_world_object(grabbed) || !pickup_id(grabbed, id, sizeof(id)))
                grabbed = nullptr;
        }
    }

    const double now = monotonic_seconds();
    static double last = now;
    const float blend = float(1.0 - std::exp(-15.0 * std::min(now-last, 0.1)));
    last = now;
    for (auto it = g_waterproof_remote.begin(); it != g_waterproof_remote.end();) {
        auto& remote = it->second;
        const bool same_grab = grabbed && it->first == id;
        if (same_grab && !g_local_is_host && now - remote.arrived <= 1.25 &&
            remote.pose.build_flags == item_drop_sync::kLockerMoving) {
            // Two grabs of one locker resolve in favor of the host.
            auto& api = g_waterproof_motion_api;
            api.release.call(cannon, api.release.method);
            if (motion.pickup.get() == grabbed) motion.pickup.clear();
            grabbed = nullptr;
        }
        if ((same_grab && grabbed) || !waterproof_apply_motion(remote, now, blend)) {
            waterproof_restore_physics(remote, false);
            it = g_waterproof_remote.erase(it);
        } else ++it;
    }

    if (motion.pickup.get() && motion.pickup.get() != grabbed) {
        if (!motion.released_at) motion.released_at = now;
        if (waterproof_world_object(motion.pickup.get()) &&
            now - motion.released_at < 2.0) {
            // Allow the game's release/throw to reach the rigidbody before reading velocity.
            if (now - motion.released_at < 0.08 || net_pending_world_event_count() >= 16 ||
                !waterproof_send_motion(motion.pickup.get(), motion.id, false)) return;
            LOG_INFO("waterproof locker: local release id=%s", motion.id);
        }
        motion.pickup.clear();
    }
    if (!grabbed) return;
    if (motion.pickup.get() != grabbed) {
        if (!prefab_retain(motion.pickup, grabbed)) return;
        snprintf(motion.id, sizeof(motion.id), "%s", id);
        motion.next_send = 0;
        motion.sent = false;
    }
    motion.released_at = 0;
    if (now < motion.next_send || net_pending_world_event_count() >= 8) return;
    motion.next_send = now + kLockerMotionInterval;
    if (waterproof_send_motion(grabbed, motion.id, true)) {
        if (!motion.sent) LOG_INFO("waterproof locker: local grab id=%s", motion.id);
        motion.sent = true;
    }
}
