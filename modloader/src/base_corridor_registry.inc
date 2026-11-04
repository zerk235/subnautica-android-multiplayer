// Session-local identity survives destruction of the temporary ConstructableBase.
// A demolition ghost gets a new ID, so resolve it by exact base ID/anchor/cell.
struct SyncedCorridor {
    bool used = false;
    bool removed = false;
    WorldEvent state{};
    base_geometry::GhostEnvelope geometry;
};
SyncedCorridor g_synced_corridors[128];

SyncedCorridor* build_synced_corridor(const char* id, bool allocate = false) {
    SyncedCorridor* empty = nullptr;
    for (auto& entry : g_synced_corridors) {
        if (entry.used && strcmp(entry.state.object_id, id) == 0) return &entry;
        if (!entry.used && !empty) empty = &entry;
    }
    if (allocate && empty) {
        empty->used = true;
        snprintf(empty->state.object_id, sizeof(empty->state.object_id), "%s", id);
        return empty;
    }
    return nullptr;
}

// A loaded Base has no temporary ConstructableBase and therefore no placement
// record in this process. An explicit demolition may bind that missing record,
// but only AFTER the receiver verifies the exact native handle and game rules.
// Never reinterpret an existing, different record as a saved object.
bool build_removal_identity_matches(const WorldEvent& state,const base_geometry::GhostEnvelope& geometry) {
    if (state.kind!=kWorldEventRemove || geometry.object_id!=state.object_id) return false;
    const auto* entry=build_synced_corridor(state.object_id);
    if (!entry || (entry->state.tech_type==0 && entry->geometry.object_id.empty())) return true;
    return entry->state.tech_type==state.tech_type &&
        entry->geometry.object_id==state.object_id &&
        base_geometry::same_corridor_cell(entry->geometry,geometry) &&
        entry->state.geometry_hash_low==state.geometry_hash_low &&
        entry->state.geometry_hash_high==state.geometry_hash_high;
}

bool build_bind_removal_identity(const WorldEvent& state,const base_geometry::GhostEnvelope& geometry) {
    if (!build_removal_identity_matches(state,geometry)) return false;
    auto* entry=build_synced_corridor(state.object_id,true);
    if (!entry) return false;
    entry->state=state;
    entry->geometry=geometry;
    entry->removed=true;
    return true;
}

bool build_corridor_removed(const char* id) {
    auto* entry = build_synced_corridor(id);
    return entry && entry->removed;
}

void build_remember_corridor(const WorldEvent& state, const base_geometry::GhostEnvelope& geometry) {
    auto* entry = build_synced_corridor(state.object_id, true);
    if (!entry || entry->removed) return;
    entry->state = state;
    entry->geometry = geometry;
}

SyncedCorridor* build_synced_cell(const base_geometry::GhostEnvelope& geometry) {
    SyncedCorridor* found = nullptr;
    for (auto& entry : g_synced_corridors) {
        if (!entry.used || entry.removed || (base_geometry::face_type_for_tech(entry.state.tech_type)>=0 || entry.state.tech_type==5504) || entry.geometry.target_base_id.empty() ||
            !base_geometry::same_corridor_cell(entry.geometry, geometry)) continue;
        if (found) return nullptr; // two identities at the same cell are ambiguous
        found = &entry;
    }
    return found;
}

SyncedCorridor* build_synced_face(const base_geometry::GhostEnvelope& geometry) {
    SyncedCorridor* found=nullptr;
    for (auto& entry:g_synced_corridors) {
        if (!entry.used || entry.removed || base_geometry::face_type_for_tech(entry.state.tech_type)<0 ||
            !base_geometry::same_structural_face(entry.geometry,geometry)) continue;
        if (found) return nullptr;
        found=&entry;
    }
    return found;
}

// Unlike unique lookup, a removal guard must reject ANY replacement, including
// an ambiguous pair of live records occupying the same physical wall face.
bool build_face_has_replacement(const base_geometry::GhostEnvelope& geometry,const char* original_id) {
    for (const auto& entry:g_synced_corridors) {
        if (entry.used && !entry.removed && strcmp(entry.state.object_id,original_id)!=0 &&
            (base_geometry::same_structural_face(entry.geometry,geometry) ||
             base_geometry::ladder_faces_overlap(entry.geometry,geometry))) return true;
    }
    return false;
}

bool build_cell_has_replacement(const base_geometry::GhostEnvelope& geometry,const char* original_id) {
    for (const auto& entry:g_synced_corridors) {
        if (entry.used && !entry.removed && strcmp(entry.state.object_id,original_id)!=0 &&
            base_geometry::face_type_for_tech(entry.state.tech_type)<0 && entry.state.tech_type!=5504 &&
            base_geometry::piece_footprints_overlap(entry.geometry,geometry)) return true;
    }
    return false;
}

SyncedCorridor* build_synced_ladder(const base_geometry::GhostEnvelope& geometry) {
    SyncedCorridor* found=nullptr;
    for (auto& e:g_synced_corridors) {
        if (!e.used || e.removed || e.state.tech_type!=5504 || !base_geometry::same_ladder(e.geometry,geometry)) continue;
        if (found) return nullptr;
        found=&e;
    }
    return found;
}
bool build_ladder_has_replacement(const base_geometry::GhostEnvelope& geometry,const char* id) {
    for (const auto& e:g_synced_corridors) {
        if (e.used && !e.removed && strcmp(e.state.object_id,id)!=0 &&
            base_geometry::ladder_faces_overlap(geometry,e.geometry)) return true;
    }
    return false;
}
