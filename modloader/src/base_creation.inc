// Native BaseGhost.Finish creates the first Base only after SetState(complete).
// Capture its generated identity after that call; never guess by proximity.
struct FirstBaseCapture {
    void* ghost=nullptr;
    void* completed_base=nullptr;
    Vector3 root_world{};
};

bool build_piece_root_world(void* base,const base_geometry::Snapshot& topology,Vector3* out) {
    if (!out || !build_live(base) || !build_resolve_hatch_capture() || !g_corridor_api.bounds) return false;
    const auto bounds=g_corridor_api.bounds.call(base,g_corridor_api.bounds.method);
    if (int64_t(bounds.max.x)-bounds.min.x+1!=topology.shape[0] ||
        int64_t(bounds.max.y)-bounds.min.y+1!=topology.shape[1] ||
        int64_t(bounds.max.z)-bounds.min.z+1!=topology.shape[2]) return false;
    *out=g_hatch_capture.grid_world.call(base,{bounds.min.x,bounds.min.y,bounds.min.z},g_hatch_capture.grid_world.method);
    return std::isfinite(out->x) && std::isfinite(out->y) && std::isfinite(out->z);
}

bool build_capture_first_base(void* component,const base_geometry::GhostEnvelope& geometry,FirstBaseCapture* out) {
    if (!out || !geometry.target_base_id.empty() || !build_resolve_corridor_api()) return false;
    void* model=nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(component),g_ghost_capture.model,&model);
    void* ghost=build_live(model) ? g_get_component.call(model,g_ghost_capture.ghost_type,g_get_component.method) : nullptr;
    void *base=nullptr,*target=nullptr;
    if (!build_live(ghost)) return false;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(ghost),g_ghost_capture.ghost_base,&base);
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(ghost),g_ghost_capture.target,&target);
    if (build_live(target) || !build_piece_root_world(base,geometry.geometry,&out->root_world) ||
        g_build_id_root_count>=kBuildKnownLimit) return false;
    // Preserve the existing process-lifetime ownership policy (see build_sync.inc). Keep a bounded
    // root for the destroyed model's managed wrapper, just as for assigned IDs.
    if (!build_keep_id_root(il2cpp_gchandle_new(reinterpret_cast<Il2CppObject*>(ghost),true))) return false;
    out->ghost=ghost;
    return true;
}

bool build_finish_first_base(FirstBaseCapture& capture,base_geometry::GhostEnvelope* geometry,int32_t tech) {
    if (!capture.ghost || !geometry) return false;
    void* target=nullptr;
    // Unity has destroyed the native model, but the pinned managed wrapper still
    // holds the target Base that Finish assigned before destroying that model.
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(capture.ghost),g_ghost_capture.target,&target);
    if (!build_live(target) || !build_resolve_hatch_capture()) return false;
    void* uid=build_uid(target);
    char id[kWorldEventObjectIdSize]{};
    if (!uid || !string_to_ascii(g_unique_get_id.call(uid,g_unique_get_id.method),id,sizeof(id)) || !id[0]) return false;
    geometry->target_base_id=id;
    const auto cell=g_hatch_capture.world_grid.call(target,capture.root_world,g_hatch_capture.world_grid.method);
    geometry->target_offset={cell.x,cell.y,cell.z};
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(target),g_ghost_capture.anchor,geometry->target_base_anchor.data());
    void* go=g_get_game_object.call(target,g_get_game_object.method);
    void* tf=go ? g_gameobject_get_transform.call(go,g_gameobject_get_transform.method) : nullptr;
    if (!tf) return false;
    const auto p=g_get_position.call(tf,g_get_position.method);
    const auto q=g_get_rotation.call(tf,g_get_rotation.method);
    geometry->base_pose={p.x,p.y,p.z,q.x,q.y,q.z,q.w};
    geometry->creates_base=true;
    capture.completed_base=target;
    return base_geometry::supported_piece_placement(*geometry,tech) && base_geometry::valid_base_pose(*geometry);
}

bool build_created_base_present(void* target,const WorldEvent& e,const base_geometry::GhostEnvelope& geometry) {
    if (!build_live(target) || !geometry.creates_base || !base_geometry::supported_piece_placement(geometry,e.tech_type)) return false;
    void* go=g_get_game_object.call(target,g_get_game_object.method);
    void* tf=go ? g_gameobject_get_transform.call(go,g_gameobject_get_transform.method) : nullptr;
    if (!tf) return false;
    const auto p=g_get_position.call(tf,g_get_position.method);
    const auto q=g_get_rotation.call(tf,g_get_rotation.method);
    const auto& pose=geometry.base_pose;
    const float dx=p.x-pose[0],dy=p.y-pose[1],dz=p.z-pose[2];
    const float dot=q.x*pose[3]+q.y*pose[4]+q.z*pose[5]+q.w*pose[6];
    if (dx*dx+dy*dy+dz*dz>0.0004f || std::fabs(dot)<0.9999f) return false;
    int32_t anchor[3]{};
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(target),g_ghost_capture.anchor,anchor);
    for (size_t i=0;i<3;++i) if (anchor[i]!=geometry.target_base_anchor[i]) return false;
    const BuildGridPoint cell{geometry.target_offset[0],geometry.target_offset[1],geometry.target_offset[2]};
    if (g_corridor_api.get_raw_cell.call(target,cell,g_corridor_api.get_raw_cell.method)!=base_geometry::piece_cell_type(e.tech_type) ||
        !build_corridor_glass_matches(target,cell,e.tech_type)) return false;
    return e.tech_type==5500 ? build_room_footprint_matches(target,geometry,false) :
           e.tech_type==5509 ? build_foundation_footprint_matches(target,geometry,false) : true;
}
