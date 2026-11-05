bool build_corridor_storage_valid(void* base, const base_geometry::Snapshot& snapshot);

// Base.OnProtoDeserialize sets isPlaced, but BaseGhost.Finish can create a complete
// live base with that flag still false. Only a successfully recorded session birth
// can replace that flag: require its original grid/world frame and native storage.
bool build_session_base_frame_ready(void* target,const std::string& id) {
    const SyncedCorridor* birth=nullptr;
    for (const auto& entry:g_synced_corridors) {
        if (!entry.used || !entry.geometry.creates_base || entry.geometry.target_base_id!=id) continue;
        if (birth) return false;
        birth=&entry;
    }
    if (!birth || !birth->root_pose_valid || !build_resolve_hatch_capture()) return false;
    int32_t shape[4]{};
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(target),g_ghost_capture.shape,shape);
    base_geometry::Snapshot storage;
    storage.shape={shape[0],shape[1],shape[2]};
    if (!build_corridor_storage_valid(target,storage)) return false;
    const auto& cell=birth->geometry.target_offset;
    const auto point=g_hatch_capture.grid_world.call(target,{cell[0],cell[1],cell[2]},g_hatch_capture.grid_world.method);
    void* go=g_get_game_object.call(target,g_get_game_object.method);
    void* transform=go ? g_gameobject_get_transform.call(go,g_gameobject_get_transform.method) : nullptr;
    if (!transform) return false;
    const auto q=g_get_rotation.call(transform,g_get_rotation.method);
    const auto& recorded=birth->root_pose;
    const float dx=point.x-recorded[0],dy=point.y-recorded[1],dz=point.z-recorded[2];
    const float distance=dx*dx+dy*dy+dz*dz;
    const float dot=q.x*recorded[3]+q.y*recorded[4]+q.z*recorded[5]+q.w*recorded[6];
    return std::isfinite(distance) && std::isfinite(dot) && distance<=0.0004f && std::fabs(dot)>=0.9999f;
}

// An exact ID is essential: proximity alone can attach a corridor to the wrong base.
void* build_corridor_target(const std::string& expected_id, const std::array<int32_t, 3>& expected_anchor,bool require_anchor=true) {
    if (!build_resolve_corridor_api() || expected_id.empty()) return nullptr;
    void* list = g_find_objects_all.call(g_build_base_geometry_type, true, g_find_objects_all.method);
    if (!list) return nullptr;
    const int32_t count = g_array_get_length.call(list, g_array_get_length.method);
    void* found = nullptr;
    bool anchor_matches = false;
    static double next_report=0;
    for (int32_t i = 0; i < count; ++i) {
        void* candidate = g_array_get_value.call(list, i, g_array_get_value.method);
        if (!build_live(candidate)) continue;
        bool is_ghost = false, is_placed = false;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(candidate), g_corridor_api.base_is_ghost, &is_ghost);
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(candidate), g_corridor_api.base_is_placed, &is_placed);
        void* uid = build_uid(candidate);
        char id[kWorldEventObjectIdSize]{};
        if (!uid || !string_to_ascii(g_unique_get_id.call(uid, g_unique_get_id.method), id, sizeof(id)) ||
            expected_id != id) continue;
        const bool session_ready=!is_ghost && !is_placed && build_session_base_frame_ready(candidate,expected_id);
        if(is_ghost || (!is_placed && !session_ready)) {
            if(build_now()>=next_report){next_report=build_now()+5;LOG_INFO("build: target deferred id=%s ghost=%u placed=%u",id,unsigned(is_ghost),unsigned(is_placed));}
            continue;
        }
        if(session_ready && build_now()>=next_report) {
            next_report=build_now()+5;
            LOG_INFO("build: target ready from verified session birth id=%s placed=0",id);
        }
        if (found) return nullptr; // duplicate IDs are ambiguous even if only one anchor matches
        found = candidate;
        int32_t anchor[3]{};
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(candidate), g_ghost_capture.anchor, anchor);
        anchor_matches = anchor[0] == expected_anchor[0] && anchor[1] == expected_anchor[1] &&
                         anchor[2] == expected_anchor[2];
        if(require_anchor && !anchor_matches && build_now()>=next_report) {
            next_report=build_now()+5;
            LOG_INFO("build: target anchor deferred id=%s actual=(%d,%d,%d) expected=(%d,%d,%d)",id,anchor[0],anchor[1],anchor[2],expected_anchor[0],expected_anchor[1],expected_anchor[2]);
        }
    }
    if (!found || (require_anchor && !anchor_matches)) return nullptr;
    void* current_ghost = nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(found), g_corridor_api.placement_ghost,
                           &current_ghost);
    if(build_live(current_ghost) && build_now()>=next_report){next_report=build_now()+5;LOG_INFO("build: target busy id=%s placement=%p",expected_id.c_str(),current_ghost);}
    return build_live(current_ghost) ? nullptr : found;
}
