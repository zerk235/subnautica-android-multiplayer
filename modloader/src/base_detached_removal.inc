// Native Deconstruct destroys an empty Base and calls ClearTargetBase on its
// resource ghost BEFORE ProgressDeconstruction. Preserve the exact cell frame
// at placement, and use it only for an explicit final demolition of that ghost.
void build_record_piece_root(const WorldEvent& state,void* completed_base=nullptr) {
    auto* entry=build_synced_corridor(state.object_id);
    if (!entry || entry->removed || base_geometry::piece_cell_type(state.tech_type)<0 ||
        !build_base_birth_known(entry->geometry.target_base_id) || !build_resolve_hatch_capture()) return;
    const auto& geometry=entry->geometry;
    // The caller may own the just-completed native Base. Its placementGhost is
    // destroyed at end-of-frame, so the normal replay lookup still sees it busy.
    void* target=completed_base ? completed_base : build_corridor_target(geometry.target_base_id,geometry.target_base_anchor,false);
    if (!target) return;
    const auto& cell=geometry.target_offset;
    auto p=g_hatch_capture.grid_world.call(target,{cell[0],cell[1],cell[2]},g_hatch_capture.grid_world.method);
    void* go=g_get_game_object.call(target,g_get_game_object.method);
    void* tf=go ? g_gameobject_get_transform.call(go,g_gameobject_get_transform.method) : nullptr;
    if (!tf) return;
    auto q=g_get_rotation.call(tf,g_get_rotation.method);
    entry->root_pose={p.x,p.y,p.z,q.x,q.y,q.z,q.w};
    entry->root_pose_valid=true;
    LOG_INFO("build: new-base cell frame recorded id=%s point=(%.3f,%.3f,%.3f)",state.object_id,p.x,p.y,p.z);
}

SyncedCorridor* build_detached_demolition(void* component,const WorldEvent& state,const base_geometry::GhostEnvelope& geometry) {
    if (!geometry.target_base_id.empty() || geometry.ghost_class!="BaseGhost" ||
        base_geometry::piece_cell_type(state.tech_type)<0) return nullptr;
    auto canonical=geometry;canonical.ghost_class=base_geometry::piece_ghost(state.tech_type);
    if (!base_geometry::standalone_piece_topology(canonical,state.tech_type)) return nullptr;
    void* model=nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(component),g_ghost_capture.model,&model);
    void* base=build_live(model) ? g_get_component.call(model,g_build_base_geometry_type,g_get_component.method) : nullptr;
    Vector3 p{};
    if (!build_piece_root_world(base,geometry.geometry,&p)) return nullptr;
    void* tf=g_gameobject_get_transform.call(model,g_gameobject_get_transform.method);
    if (!tf) return nullptr;
    const auto q=g_get_rotation.call(tf,g_get_rotation.method);
    SyncedCorridor* found=nullptr;
    for (auto& entry:g_synced_corridors) {
        if (!entry.used || entry.removed || !entry.root_pose_valid || entry.state.tech_type!=state.tech_type ||
            !build_base_birth_known(entry.geometry.target_base_id)) continue;
        const auto& pose=entry.root_pose;
        const float dx=p.x-pose[0],dy=p.y-pose[1],dz=p.z-pose[2];
        const float dot=q.x*pose[3]+q.y*pose[4]+q.z*pose[5]+q.w*pose[6];
        if (dx*dx+dy*dy+dz*dz>0.000025f || std::fabs(dot)<0.99999f ||
            build_base_id_exists(entry.geometry.target_base_id)) continue;
        if (found) return nullptr; // identical overlapping bases remain ambiguous
        found=&entry;
    }
    return found;
}
