// Returns true when done (including a permanent refusal); false requests a retry.
bool build_apply_corridor_removal_inner(void* target, const WorldEvent& e,
                                        const base_geometry::GhostEnvelope& geometry) {
    if (g_corridor_removal_failed || e.kind != kWorldEventRemove || e.face_has_value ||
        !base_geometry::supported_existing_piece(geometry, e.tech_type) || geometry.object_id != e.object_id)
        return true;
    if (!build_removal_identity_matches(e,geometry)) {
        LOG_WARN("build: corridor removal refused id=%s: conflicting placement identity", e.object_id);
        return true;
    }
    if (build_cell_has_replacement(geometry,e.object_id)) return true;
    const bool may_remove_last=build_base_birth_known(geometry.target_base_id);
    if (!build_resolve_corridor_removal()) return true;
    auto& a = g_corridor_removal_api;
    const BuildGridPoint cell{geometry.target_offset[0], geometry.target_offset[1], geometry.target_offset[2]};
    const uint8_t current = g_corridor_api.get_cell.call(target, cell, g_corridor_api.get_cell.method);
    if (current == 0) return true;
    if (current != base_geometry::piece_cell_type(e.tech_type)) { LOG_WARN("build: corridor removal refused: cell changed type id=%s", e.object_id); return true; }
    if (!build_corridor_glass_matches(target, cell, e.tech_type)) {
        LOG_WARN("build: corridor removal refused: glass changed id=%s", e.object_id);
        return true;
    }
    if (base_geometry::multi_cell_piece(e.tech_type)) {
        if (!(e.tech_type==5500 ? build_room_footprint_matches(target,geometry,false) :
              build_foundation_footprint_matches(target,geometry,false)) ||
            (!may_remove_last && !build_foundation_has_other_root(target))) return true;
    } else {
    // Never remove the final cell/base through this first implementation.
    void* cells = nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(target), g_ghost_capture.cells, &cells);
    const int32_t length = cells ? g_array_get_length.call(cells, g_array_get_length.method) : 0;
    if (length <= 0 || length > 1048576) return true;
    int occupied = 0;
    auto* bytes = reinterpret_cast<uint8_t*>(cells) + 0x20;
    for (int32_t i=0; i<length && occupied<2; ++i) if (bytes[i]) ++occupied;
    if (occupied < 2 && !may_remove_last) return true;
    }
    void* list = g_find_objects_all.call(a.type, true, g_find_objects_all.method);
    const int32_t count = list ? g_array_get_length.call(list, g_array_get_length.method) : 0;
    void* deconstructable = nullptr;
    for (int32_t i=0; i<count; ++i) {
        void* candidate = g_array_get_value.call(list, i, g_array_get_value.method);
        if (!build_live(candidate)) continue;
        int32_t bounds[6]{}, recipe=0;
        uint8_t face[20]{};
        auto* object = reinterpret_cast<Il2CppObject*>(candidate);
        il2cpp_field_get_value(object, a.bounds, bounds);
        il2cpp_field_get_value(object, a.recipe, &recipe);
        il2cpp_field_get_value(object, a.face, face);
        if (recipe != e.tech_type || face[0] ||
            !base_geometry::piece_deconstruction_bounds(geometry,e.tech_type,bounds)) continue;
        void* go = g_get_game_object.call(candidate, g_get_game_object.method);
        bool belongs = false;
        walk_parents(go, [&](void* node) {
            void* base = g_get_component.call(node, g_build_base_geometry_type, g_get_component.method);
            if (base != target) return false;
            belongs = true;
            return true;
        });
        if (!belongs) continue;
        if (deconstructable) { LOG_WARN("build: corridor removal ambiguous id=%s", e.object_id); return true; }
        deconstructable = candidate;
    }
    if (!deconstructable) { LOG_WARN("build: corridor removal waiting for exact cell handle id=%s", e.object_id); return false; }
    void* reason = nullptr;
    if (!a.allowed.call(deconstructable, &reason, a.allowed.method)) {
        LOG_WARN("build: corridor removal blocked by local game rules id=%s", e.object_id);
        return false;
    }
    if (!build_bind_removal_identity(e,geometry)) return true;
    const int prior_remote = g_applying_remote;
    const char* stage = "clear-cell";
    ++g_applying_remote;
    try {
        // Same single-cell and repair methods called by BaseDeconstructable.Deconstruct
        // (2A919E4, 2A91AF8, 2A91B04, 2A91B10), with no temporary resource ghost.
        a.clear_cell.call(target, cell, a.clear_cell.method);
        if (may_remove_last && a.is_empty.call(target,nullptr,a.is_empty.method)) {
            // Same final-base branch as native BaseDeconstructable.Deconstruct:
            // IsEmpty -> OnPreDestroy -> Destroy(gameObject). No resource ghost.
            stage="pre-destroy";a.pre_destroy.call(target,a.pre_destroy.method);
            void* go=g_get_game_object.call(target,g_get_game_object.method);
            stage="destroy-empty-base";g_object_destroy.call(go,g_object_destroy.method);
            build_synced_corridor(e.object_id)->removal_applied=true;
            LOG_INFO("build: empty new base removed id=%s base=%s verified=1",e.object_id,geometry.target_base_id.c_str());
            g_applying_remote=prior_remote;
            return true;
        }
        stage = "fix-floors"; a.fix_floors.call(target, a.fix_floors.method);
        stage = "fix-links"; a.fix_links.call(target, a.fix_links.method);
        stage = "rebuild"; a.rebuild.call(target, a.rebuild.method);
        const uint8_t actual = g_corridor_api.get_cell.call(target, cell, g_corridor_api.get_cell.method);
        const bool verified=actual==0 &&
            (e.tech_type!=5509 || build_foundation_footprint_matches(target,geometry,true)) &&
            (e.tech_type!=5500 || build_room_footprint_matches(target,geometry,true));
        LOG_INFO("build: corridor removed id=%s target_cell=%u verified=%u tech=%d", e.object_id, actual, unsigned(verified),e.tech_type);
        if (!verified) g_corridor_removal_failed = true;
    } catch (...) {
        g_corridor_removal_failed = true;
        LOG_WARN("build: corridor removal exception id=%s stage=%s; removal disabled until restart", e.object_id, stage);
    }
    g_applying_remote = prior_remote;
    return true; // once mutated, never repeat a partly applied removal
}

bool build_apply_corridor_removal(void* target, const WorldEvent& e,
                                  const base_geometry::GhostEnvelope& geometry) {
    const int prior_remote = g_applying_remote;
    try {
        const bool done = build_apply_corridor_removal_inner(target, e, geometry);
        g_applying_remote = prior_remote;
        return done;
    } catch (...) {
        g_applying_remote = prior_remote;
        g_corridor_removal_failed = true;
        LOG_WARN("build: corridor removal validation exception id=%s; removal disabled until restart", e.object_id);
        return true;
    }
}
