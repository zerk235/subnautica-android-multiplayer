#include "base_live_storage.inc"

bool build_map_base_geometry(void* target,const base_geometry::GhostEnvelope& source,base_geometry::GhostEnvelope* out) {
    if (!build_live(target)) return false;
    std::array<int32_t,3> anchor{};
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(target),g_ghost_capture.anchor,anchor.data());
    return base_geometry::remap_target_anchor(source,anchor,out);
}

bool build_recorded_frame_matches(void* target,const SyncedCorridor& entry) {
    if (!entry.root_pose_valid || !build_resolve_hatch_capture()) return false;
    base_geometry::GhostEnvelope mapped;
    if (!build_map_base_geometry(target,entry.geometry,&mapped)) return false;
    const auto& cell=mapped.target_offset;
    const auto point=g_hatch_capture.grid_world.call(target,{cell[0],cell[1],cell[2]},g_hatch_capture.grid_world.method);
    void* go=g_get_game_object.call(target,g_get_game_object.method);
    void* tf=go ? g_gameobject_get_transform.call(go,g_gameobject_get_transform.method) : nullptr;
    if (!tf) return false;
    const auto q=g_get_rotation.call(tf,g_get_rotation.method);
    const auto& recorded=entry.root_pose;
    const float dx=point.x-recorded[0],dy=point.y-recorded[1],dz=point.z-recorded[2];
    const float distance=dx*dx+dy*dy+dz*dz;
    const float dot=q.x*recorded[3]+q.y*recorded[4]+q.z*recorded[5]+q.w*recorded[6];
    return std::isfinite(distance) && std::isfinite(dot) && distance<=0.0004f && std::fabs(dot)>=0.9999f;
}

bool build_recorded_base_frame_ready(void* target,const std::string& id) {
    bool matched=false;
    for (const auto& entry:g_synced_corridors) {
        if (!entry.used || !entry.root_pose_valid || entry.geometry.target_base_id!=id) continue;
        if (!build_recorded_frame_matches(target,entry)) return false;
        matched=true;
    }
    return matched;
}

// Base.OnProtoDeserialize sets isPlaced, but BaseGhost.Finish can create a complete
// live base with that flag still false. Only a successfully recorded session birth
// can replace that flag: require its original transform and native storage. The
// integer grid is reindexed by EnsureSize when extending towards negative cells;
// the old birth cell's integer index is not a stable world-space reference.
bool build_session_base_frame_ready(void* target,const std::string& id) {
    const SyncedCorridor* birth=nullptr;
    for (const auto& entry:g_synced_corridors) {
        if (!entry.used || !entry.geometry.creates_base || entry.geometry.target_base_id!=id) continue;
        if (birth) return false;
        birth=&entry;
    }
    if (!birth || !build_recorded_frame_matches(target,*birth)) return false;
    if (!base_geometry::valid_base_pose(birth->geometry) ||
        !build_completed_base_storage_valid(target)) return false;
    void* go=g_get_game_object.call(target,g_get_game_object.method);
    void* transform=go ? g_gameobject_get_transform.call(go,g_gameobject_get_transform.method) : nullptr;
    if (!transform) return false;
    const auto point=g_get_position.call(transform,g_get_position.method);
    const auto q=g_get_rotation.call(transform,g_get_rotation.method);
    const auto& recorded=birth->geometry.base_pose;
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
        if (require_anchor && !anchor_matches && build_recorded_base_frame_ready(candidate,expected_id)) {
            anchor_matches=true;
            if (build_now()>=next_report) {
                next_report=build_now()+5;
                LOG_INFO("build: target anchor translated id=%s actual=(%d,%d,%d) snapshot=(%d,%d,%d)",
                    id,anchor[0],anchor[1],anchor[2],expected_anchor[0],expected_anchor[1],expected_anchor[2]);
            }
        }
        if(require_anchor && !anchor_matches && build_now()>=next_report) {
            next_report=build_now()+5;
            LOG_INFO("build: target anchor deferred id=%s actual=(%d,%d,%d) expected=(%d,%d,%d)",id,anchor[0],anchor[1],anchor[2],expected_anchor[0],expected_anchor[1],expected_anchor[2]);
        }
    }
    if (!found || (require_anchor && !anchor_matches)) {
        // A silent miss here stalls every anchored replay: report what the world holds.
        if (!found && build_now()>=next_report) {
            next_report=build_now()+5;
            char ids[256]="";
            int shown=0;
            for (int32_t i=0;i<count&&shown<3;++i) {
                void* candidate=g_array_get_value.call(list,i,g_array_get_value.method);
                if (!build_live(candidate)) { ++shown; continue; }
                void* uid=build_uid(candidate);
                char actual[kWorldEventObjectIdSize]{};
                const bool have=uid&&string_to_ascii(g_unique_get_id.call(uid,g_unique_get_id.method),actual,sizeof(actual));
                bool is_ghost=false,is_placed=false;
                il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(candidate),g_corridor_api.base_is_ghost,&is_ghost);
                il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(candidate),g_corridor_api.base_is_placed,&is_placed);
                char one[96];
                snprintf(one,sizeof(one),"%s(g%u p%u) ",have?actual:"<noid>",unsigned(is_ghost),unsigned(is_placed));
                strncat(ids,one,sizeof(ids)-strlen(ids)-1);
                ++shown;
            }
            LOG_WARN("build: target missing id=%s bases=%d first=%s",expected_id.c_str(),count,ids);
        }
        return nullptr;
    }
    void* current_ghost = nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(found), g_corridor_api.placement_ghost,
                           &current_ghost);
    if(build_live(current_ghost) && build_now()>=next_report){next_report=build_now()+5;LOG_INFO("build: target busy id=%s placement=%p",expected_id.c_str(),current_ghost);}
    return build_live(current_ghost) ? nullptr : found;
}
