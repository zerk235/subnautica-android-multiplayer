bool g_ladder_apply_failed=false;
HatchFace build_ladder_face(const base_geometry::GhostEnvelope& g,int y,int h) {
    return {{g.target_offset[0],g.target_offset[1]+y,g.target_offset[2]},y==0 ? 4 : y==h-1 ? 5 : 0};
}
bool build_ladder_readback(void* target,const base_geometry::GhostEnvelope& g,int h,bool removed) {
    for (int y=0;y<h;++y) {
        auto f=build_ladder_face(g,y,h);
        const int v=g_hatch_capture.get_face.call(target,f,g_hatch_capture.get_face.method);
        // ClearFace restores Solid(1). FixCorridorLinks then normalizes an
        // endpoint adjoining a corridor/connector to Hole(9), without a ladder.
        const bool endpoint=y==0 || y==h-1;
        if (removed ? (v!=0 && v!=1 && !(endpoint && v==9)) : v!=5) {
            LOG_WARN("build: ladder readback refused y=%d direction=%d actual=%d removed=%u",
                y,f.direction,v,unsigned(removed));
            return false;
        }
    }
    return true;
}
bool build_apply_ladder_inner(void* target,const WorldEvent& e,const base_geometry::GhostEnvelope& wire_geometry) {
    base_geometry::GhostEnvelope g;
    if (!build_map_base_geometry(target,wire_geometry,&g)) return true;
    const int h=base_geometry::ladder_height(g,e.tech_type);
    if (!h || g.object_id!=e.object_id || e.face_has_value ||
        !(e.build_flags & kWorldBuildAnchored) || !(e.build_flags & kWorldBuildComplete) ||
        !(e.build_flags & kWorldBuildHasGeometry) ||
        (e.kind!=kWorldEventBuild && e.kind!=kWorldEventRemove)) return true;
    if (g_ladder_apply_failed || !build_resolve_ladder_capture() || !build_resolve_hatch_apply()) return true;
    const bool remove=e.kind==kWorldEventRemove;
    const auto lower=build_ladder_face(g,0,h),upper=build_ladder_face(g,h-1,h);
    if (!build_ladder_path_cells(target,lower.cell,h)) return false;
    HatchPoint exit{};
    if (!g_ladder_capture.exit_cell.call(target,lower.cell,4,&exit,g_ladder_capture.exit_cell.method) ||
        exit.x!=upper.cell.x || exit.y!=upper.cell.y || exit.z!=upper.cell.z) return false;
    if (remove) {
        if (!build_removal_identity_matches(e,g) ||
            build_ladder_has_replacement(g,e.object_id) || !build_ladder_readback(target,g,h,false)) return true;
        // The game can expose the deconstructor at either end of the same ladder.
        if (!build_hatch_removal_allowed(target,lower,5504) &&
            !build_hatch_removal_allowed(target,upper,5504)) return false;
        if (!build_bind_removal_identity(e,wire_geometry)) return true;
    } else {
        if (build_corridor_removed(e.object_id) || build_corridor_record(e,false)) return true;
        HatchFace actual{};
        if (!g_ladder_capture.can_set.call(target,lower,&actual,g_ladder_capture.can_set.method) ||
            actual.cell.x!=upper.cell.x || actual.cell.y!=upper.cell.y || actual.cell.z!=upper.cell.z || actual.direction!=5) return false;
        for (int y=0;y<h;++y) {
            auto f=build_ladder_face(g,y,h);
            int v=g_hatch_capture.get_face.call(target,f,g_hatch_capture.get_face.method);
            if (y==0 || y==h-1) { if (v!=1 && v!=9) return true; }
            else if (v!=0 && v!=1) return true;
        }
        if (build_ladder_has_replacement(g,e.object_id) || !build_synced_corridor(e.object_id,true)) return true;
        auto* record=build_corridor_record(e,true);
        if (!record) return true;
        record->used=true;record->origin=e.origin_id;
        snprintf(record->object_id,sizeof(record->object_id),"%s",e.object_id);
    }
    ++g_applying_remote;
    if (remove) {
        // Native ClearFace(Ladder) clears both ends and all connector markers once.
        g_hatch_apply.clear_face.call(target,lower,5,g_hatch_apply.clear_face.method);
    } else {
        for (int y=0;y<h;++y)
            g_hatch_apply.set_face.call(target,build_ladder_face(g,y,h),5,g_hatch_apply.set_face.method);
    }
    auto& a=g_corridor_removal_api;
    a.fix_floors.call(target,a.fix_floors.method);
    a.fix_links.call(target,a.fix_links.method);
    a.rebuild.call(target,a.rebuild.method);
    const bool verified=build_ladder_readback(target,g,h,remove);
    if (!verified) g_ladder_apply_failed=true;
    else if (!remove) {
        build_remember_corridor(e,wire_geometry);
        build_corridor_record(e,false)->verified=true;
    }
    LOG_INFO("build: ladder %s id=%s lower=(%d,%d,%d) height=%d verified=%u",
        remove ? "removed" : "applied",e.object_id,lower.cell.x,lower.cell.y,lower.cell.z,h,unsigned(verified));
    return true;
}
bool build_apply_ladder(void* target,const WorldEvent& e,const base_geometry::GhostEnvelope& g) {
    const int prior=g_applying_remote;bool done=true;
    try { done=build_apply_ladder_inner(target,e,g); }
    catch (...) {
        g_ladder_apply_failed=true;
        LOG_WARN("build: ladder native exception id=%s; ladder apply disabled until restart",e.object_id);
    }
    g_applying_remote=prior;return done;
}
