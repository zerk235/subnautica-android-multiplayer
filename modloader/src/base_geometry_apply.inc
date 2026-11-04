// Experimental game-thread path for a completed single-cell corridor attached to an
// existing base. This is intentionally excluded unless SNMOD_BASE_GEOMETRY_APPLY is set.
struct BuildGridPoint { int32_t x, y, z; };
using BuildSetSizeFn = void (*)(void*, BuildGridPoint, void*);
using BuildGetCellFn = uint8_t (*)(void*, BuildGridPoint, void*);
using BuildCellIndexFn = int32_t (*)(void*, BuildGridPoint, void*);
using BuildSetGhostFn = void (*)(void*, void*, void*);
using BuildDeserializeFn = void (*)(void*, void*, void*);
using BuildSetBoolFn = void (*)(void*, bool, void*);
using BuildArrayClearFn = void (*)(void*, int32_t, int32_t, void*);

struct AppliedCorridor {
    bool used = false;
    bool verified = false;
    uint32_t origin = 0;
    char object_id[kWorldEventObjectIdSize]{};
};
constexpr size_t kAppliedCorridorLimit = 128;
AppliedCorridor g_applied_corridors[kAppliedCorridorLimit];

AppliedCorridor* build_corridor_record(const WorldEvent& e, bool allocate) {
    AppliedCorridor* free_slot = nullptr;
    for (auto& record : g_applied_corridors) {
        if (record.used && strcmp(record.object_id, e.object_id) == 0)
            return &record;
        if (!record.used && !free_slot) free_slot = &record;
    }
    return allocate ? free_slot : nullptr;
}

struct CorridorApi {
    bool attempted = false, ready = false;
    struct Il2CppClass *base = nullptr, *ghost = nullptr;
    void *base_is_ghost = nullptr, *base_is_placed = nullptr, *placement_ghost = nullptr;
    void *cell_objects = nullptr, *deserialization_finished = nullptr;
    Entry<BuildArrayClearFn> clear_array;
    Entry<BuildSetSizeFn> set_size;
    Entry<BuildGetCellFn> get_cell,get_raw_cell;
    Entry<BuildCellIndexFn> cell_index;
    Entry<BuildSetGhostFn> set_placement_ghost;
    Entry<BuildDeserializeFn> deserialize;
    Entry<ManagedFn> finish_deserialization, place;
    Entry<BuildSetBoolFn> set_ghost_visible;
    Entry<BuildSetStateFn> set_state;
};
CorridorApi g_corridor_api;

bool build_resolve_corridor_api() {
    auto& a = g_corridor_api;
    if (a.attempted) return a.ready;
    a.attempted = true;
    if (!build_resolve_ghost_capture() || !g_instantiate || !g_set_parent ||
        !g_get_component || !g_gameobject_get_transform || !g_array_get_length ||
        !il2cpp_field_set_value_object) return false;
    a.base = build_class("Base"); a.ghost = build_class("BaseGhost");
    if (!a.base || !a.ghost || !g_build_base_class) return false;
    a.base_is_ghost = build_field_on(a.base, "isGhost", 0x80);
    a.base_is_placed = build_field_on(a.base, "isPlaced", 0x48);
    a.placement_ghost = build_field_on(a.base, "placementGhost", 0x28);
    a.cell_objects = build_field_on(a.base, "cellObjects", 0xF0);
    a.deserialization_finished = build_field_on(a.base, "deserializationFinished", 0x164);
    auto* array_class = il2cpp_get_corlib && il2cpp_class_from_name ?
        il2cpp_class_from_name(il2cpp_get_corlib(), "System", "Array") : nullptr;
    a.clear_array.bind(build_typed_method(array_class, "Clear", 3, "System.Array"));
    a.set_size.bind(build_typed_method(a.base, "SetSize", 1, "Int3"));
    a.get_cell.bind(build_typed_method(a.base, "GetCell", 1, "Int3"));
    a.get_raw_cell.bind(build_typed_method(a.base,"GetRawCellType",1,"Int3"));
    a.cell_index.bind(build_typed_method(a.base, "GetCellIndex", 1, "Int3"));
    a.set_placement_ghost.bind(find_method(a.base, "SetPlacementGhost", 1));
    a.deserialize.bind(find_method(a.base, "OnProtoDeserialize", 1));
    a.finish_deserialization.bind(find_method(a.base, "FinishDeserialization", 0));
    a.place.bind(find_method(a.ghost, "Place", 0));
    a.set_ghost_visible.bind(build_typed_method(g_build_base_class, "SetGhostVisible", 1,
                                                "System.Boolean"));
    a.set_state.bind(find_method(g_build_base_class, "SetState", 2));
    a.ready = a.base_is_ghost && a.base_is_placed && a.placement_ghost && a.set_size &&
              a.cell_objects && a.deserialization_finished && a.clear_array &&
              a.get_cell && a.get_raw_cell && a.cell_index && a.set_placement_ghost && a.deserialize &&
              a.finish_deserialization && a.place && a.set_ghost_visible && a.set_state;
    LOG_INFO("build: corridor apply API %s", a.ready ? "ready" : "unavailable");
    return a.ready;
}

bool build_corridor_glass_matches(void* target, BuildGridPoint cell, int32_t tech_type) {
    const int32_t index = g_corridor_api.cell_index.call(target, cell, g_corridor_api.cell_index.method);
    void* glass = nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(target), g_ghost_capture.glass, &glass);
    const int32_t length = glass ? g_array_get_length.call(glass, g_array_get_length.method) : 0;
    return index >= 0 && index < length &&
        (reinterpret_cast<uint8_t*>(glass) + 0x20)[index] == uint8_t(base_geometry::corridor_glass(tech_type));
}

#include "base_foundation_readback.inc"

// An exact ID is essential: proximity alone can attach a corridor to the wrong base.
void* build_corridor_target(const std::string& expected_id, const std::array<int32_t, 3>& expected_anchor) {
    if (!build_resolve_corridor_api() || expected_id.empty()) return nullptr;
    void* list = g_find_objects_all.call(g_build_base_geometry_type, true, g_find_objects_all.method);
    if (!list) return nullptr;
    const int32_t count = g_array_get_length.call(list, g_array_get_length.method);
    void* found = nullptr;
    bool anchor_matches = false;
    for (int32_t i = 0; i < count; ++i) {
        void* candidate = g_array_get_value.call(list, i, g_array_get_value.method);
        if (!build_live(candidate)) continue;
        bool is_ghost = false, is_placed = false;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(candidate), g_corridor_api.base_is_ghost, &is_ghost);
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(candidate), g_corridor_api.base_is_placed, &is_placed);
        if (is_ghost || !is_placed) continue;
        void* uid = build_uid(candidate);
        char id[kWorldEventObjectIdSize]{};
        if (!uid || !string_to_ascii(g_unique_get_id.call(uid, g_unique_get_id.method), id, sizeof(id)) ||
            expected_id != id) continue;
        if (found) return nullptr; // duplicate IDs are ambiguous even if only one anchor matches
        found = candidate;
        int32_t anchor[3]{};
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(candidate), g_ghost_capture.anchor, anchor);
        anchor_matches = anchor[0] == expected_anchor[0] && anchor[1] == expected_anchor[1] &&
                         anchor[2] == expected_anchor[2];
    }
    if (!found || !anchor_matches) return nullptr;
    void* current_ghost = nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(found), g_corridor_api.placement_ghost,
                           &current_ghost);
    return build_live(current_ghost) ? nullptr : found;
}

bool build_corridor_copy_array(void* base, void* field, const std::vector<uint8_t>& bytes) {
    void* array = nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(base), field, &array);
    if (!array || g_array_get_length.call(array, g_array_get_length.method) !=
                      static_cast<int32_t>(bytes.size())) return false;
    memcpy(reinterpret_cast<uint8_t*>(array) + 0x20, bytes.data(), bytes.size());
    return true;
}

bool build_corridor_set_reference(void* object, void* field, void* reference) {
    // This API takes the managed reference itself, never the address of a local
    // pointer. The latter stores a stack address and corrupts the managed heap.
    il2cpp_field_set_value_object(reinterpret_cast<Il2CppObject*>(object), field,
                                  reinterpret_cast<Il2CppObject*>(reference));
    void* actual = nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(object), field, &actual);
    return actual == reference;
}

bool build_corridor_copy_masks(void* base, void* field, const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) {
        return build_corridor_set_reference(base, field, nullptr);
    }
    void* array = nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(base), field, &array);
    if (!array) {
        if (!il2cpp_get_corlib || !il2cpp_class_from_name || !il2cpp_array_new) return false;
        auto* cls = il2cpp_class_from_name(il2cpp_get_corlib(), "System", "Byte");
        if (!cls) return false;
        array = il2cpp_array_new(cls, static_cast<unsigned int>(bytes.size()));
        if (!array) return false;
        if (!build_corridor_set_reference(base, field, array)) return false;
    }
    return build_corridor_copy_array(base, field, bytes);
}

struct CorridorAttempt {
    void* object = nullptr;
    void* ghost = nullptr;
    const char* stage = "validate";
    bool placed = false;
};
// A caught game exception stops further attempts until the process is restarted.
bool g_corridor_apply_failed = false;

bool build_corridor_storage_valid(void* base, const base_geometry::Snapshot& snapshot) {
    auto& a = g_ghost_capture;
    int32_t shape[4]{};
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(base), a.shape, shape);
    if (shape[0] != snapshot.shape[0] || shape[1] != snapshot.shape[1] ||
        shape[2] != snapshot.shape[2] || shape[3] != shape[0] * shape[1]) return false;
    size_t count = 0;
    if (!base_geometry::cell_count(snapshot.shape, &count)) return false;
    auto matches = [&](void* field, size_t length, bool nullable = false) {
        void* array = nullptr;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(base), field, &array);
        return !array ? nullable : g_array_get_length.call(array, g_array_get_length.method) ==
                                 static_cast<int32_t>(length);
    };
    return matches(a.faces, count * 6) && matches(a.cells, count) &&
           matches(a.links, count) && matches(a.glass, count) && matches(a.masks, count, true) &&
           matches(g_corridor_api.cell_objects, count);
}

bool build_apply_completed_corridor_inner(void* prefab, void* target, const WorldEvent& e,
                                         const base_geometry::GhostEnvelope& snapshot,
                                         CorridorAttempt& attempt) {
    if (!build_payload_valid(e) || !(e.build_flags & kWorldBuildComplete) || e.face_has_value ||
        !base_geometry::supported_existing_piece(snapshot, e.tech_type) ||
        snapshot.object_id != e.object_id || !build_live(prefab) || !build_live(target) ||
        !build_resolve_corridor_api() || g_build_id_root_count >= kBuildKnownLimit ||
        !build_corridor_record(e, true)) {
        LOG_WARN("build: piece replay prerequisites refused id=%s tech=%d",e.object_id,e.tech_type);
        return false;
    }

    // A connector must not overwrite a differing local cell when the worlds
    // are temporarily out of step. Retry until the target cell becomes free.
    if ((e.tech_type==5519 || base_geometry::multi_cell_piece(e.tech_type)) &&
        !build_piece_footprint_free(target,snapshot,e.tech_type)) return false;
    const Vector3 position{e.x, e.y, e.z};
    const Quaternion rotation{e.qx, e.qy, e.qz, e.qw};
    attempt.stage = "instantiate";
    void* go = g_instantiate.call(prefab, position, rotation, g_instantiate.method);
    attempt.object = go;
    if (!build_live(go)) return false;
    void* c = g_get_component.call(go, g_build_type, g_get_component.method);
    void* uid = c ? build_uid(c) : nullptr;
    void* transform = g_gameobject_get_transform.call(go, g_gameobject_get_transform.method);
    void* model = nullptr;
    if (build_live(c)) il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(c), g_ghost_capture.model, &model);
    void* ghost = build_live(model) ? g_get_component.call(model, g_ghost_capture.ghost_type,
                                                            g_get_component.method) : nullptr;
    attempt.ghost = ghost;
    void* ghost_base = build_live(model) ? g_get_component.call(model, g_build_base_geometry_type,
                                                                 g_get_component.method) : nullptr;
    void* ghost_base_field = nullptr;
    if (build_live(ghost)) il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(ghost),
                                                  g_ghost_capture.ghost_base, &ghost_base_field);
    struct Il2CppClass* cls = build_live(ghost) ? il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(ghost)) : nullptr;
    const char* class_name = cls ? il2cpp_class_get_name(cls) : nullptr;
    Entry<ManagedFn> setup;
    if (cls) setup.bind(find_method(cls, "SetupGhost", 0));
    void* target_go = g_get_game_object.call(target, g_get_game_object.method);
    void* target_transform = target_go ? g_gameobject_get_transform.call(target_go,
                                                g_gameobject_get_transform.method) : nullptr;
    bool ok = build_live(c) && uid && transform && build_live(ghost) && build_live(ghost_base) &&
              ghost_base_field == ghost_base && target_transform && class_name &&
              strcmp(class_name, base_geometry::piece_ghost(e.tech_type)) == 0 && setup;
    if (ok && base_geometry::multi_cell_piece(e.tech_type)) {
        int32_t actual_tech=0;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(c),g_build_tech_field,&actual_tech);
        ok=actual_tech==e.tech_type;
        if (!ok) LOG_WARN("build: multi-cell prefab mismatch id=%s expected=%d actual=%d",e.object_id,e.tech_type,actual_tech);
    }
    if (ok && base_geometry::corridor_shape(e.tech_type)>=0) {
        void* shape_field = build_field_on(cls, "corridor", 0x4C);
        void* glass_field = build_field_on(cls, "isGlass", 0x49);
        int32_t shape = -1;
        bool glass = false;
        if (shape_field && glass_field) {
            il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(ghost), shape_field, &shape);
            il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(ghost), glass_field, &glass);
        }
        ok = shape_field && glass_field && base_geometry::corridor_prefab_matches(e.tech_type, shape, glass);
        if (!ok) LOG_WARN("build: corridor prefab mismatch id=%s tech=%d shape=%d glass=%u",
                          e.object_id, e.tech_type, shape, unsigned(glass));
    }
    if (ok) {
        void* id = il2cpp_string_new(e.object_id);
        unsigned int root = id ? il2cpp_gchandle_new(reinterpret_cast<Il2CppObject*>(id), true) : 0;
        ok = build_keep_id_root(root);
        if (ok) g_uid_set_id.call(uid, id, g_uid_set_id.method);
    }
    if (!ok) {
        LOG_WARN("build: piece prefab initialization refused id=%s tech=%d ghost=%s",e.object_id,e.tech_type,class_name ? class_name : "null");
        return false; // The wrapper owns cleanup until Place starts.
    }

    build_write_anchors(c, e);
    ++g_applying_remote;
    attempt.stage = "setup-ghost";
    setup.call(ghost, setup.method);
    const BuildGridPoint size{snapshot.geometry.shape[0], snapshot.geometry.shape[1], snapshot.geometry.shape[2]};
    // SetSize returns early for an equal shape. Force fresh storage rather than
    // retaining the prefab's generated cell bindings and auxiliary arrays.
    int32_t empty_shape[4]{};
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(ghost_base), g_ghost_capture.shape, empty_shape);
    attempt.stage = "set-size";
    g_corridor_api.set_size.call(ghost_base, size, g_corridor_api.set_size.method);
    auto& a = g_ghost_capture;
    ok = build_corridor_copy_array(ghost_base, a.faces, snapshot.geometry.faces) &&
         build_corridor_copy_array(ghost_base, a.cells, snapshot.geometry.cells) &&
         build_corridor_copy_array(ghost_base, a.links, snapshot.geometry.links) &&
         build_corridor_copy_masks(ghost_base, a.masks, snapshot.geometry.masks) &&
         build_corridor_copy_array(ghost_base, a.glass, snapshot.geometry.glass);
    if (!ok) {
        --g_applying_remote;
        LOG_WARN("build: corridor arrays incompatible id=%s", e.object_id);
        return false;
    }
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(ghost_base), a.cell_offset,
                           const_cast<int32_t*>(snapshot.geometry.cell_offset.data()));
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(ghost_base), a.anchor,
                           const_cast<int32_t*>(snapshot.geometry.anchor.data()));
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(ghost), a.offset,
                           const_cast<int32_t*>(snapshot.target_offset.data()));
    attempt.stage = "deserialize";
    g_corridor_api.deserialize.call(ghost_base, nullptr, g_corridor_api.deserialize.method);

    // FinishDeserialization binds the existing child transforms again. The old
    // references must be cleared through System.Array so the GC barrier is respected.
    if (!build_corridor_storage_valid(ghost_base, snapshot.geometry)) {
        LOG_WARN("build: corridor storage invalid before reconstruction id=%s", e.object_id);
        --g_applying_remote;
        return false;
    }
    void* cell_objects = nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(ghost_base), g_corridor_api.cell_objects, &cell_objects);
    const int32_t object_count = g_array_get_length.call(cell_objects, g_array_get_length.method);
    attempt.stage = "clear-cell-bindings";
    g_corridor_api.clear_array.call(cell_objects, 0, object_count, g_corridor_api.clear_array.method);
    bool unfinished = false;
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(ghost_base),
                           g_corridor_api.deserialization_finished, &unfinished);
    LOG_INFO("build: corridor storage prepared id=%s cells=%d offset=(%d,%d,%d)",
             e.object_id, object_count, snapshot.target_offset[0], snapshot.target_offset[1],
             snapshot.target_offset[2]);

    // From here the game begins mutating the target base. Record first, so any failed
    // postcondition cannot cause a retry to apply the same corridor a second time.
    AppliedCorridor* record = build_corridor_record(e, true);
    record->used = true; record->origin = e.origin_id;
    snprintf(record->object_id, sizeof(record->object_id), "%s", e.object_id);
    attempt.stage = "register-ghost";
    g_corridor_api.set_placement_ghost.call(target, ghost, g_corridor_api.set_placement_ghost.method);
    if (!build_corridor_set_reference(ghost, a.target, target)) return false;
    attempt.stage = "finish-deserialization";
    g_corridor_api.finish_deserialization.call(ghost_base, g_corridor_api.finish_deserialization.method);
    if (!build_corridor_storage_valid(ghost_base, snapshot.geometry)) {
        LOG_WARN("build: corridor storage invalid after reconstruction id=%s", e.object_id);
        return false;
    }
    attempt.stage = "place";
    g_corridor_api.set_ghost_visible.call(c, false, g_corridor_api.set_ghost_visible.method);
    attempt.placed = true; // Place itself can throw after changing the target.
    g_corridor_api.place.call(ghost, g_corridor_api.place.method);
    g_set_parent.call(transform, target_transform, true, g_set_parent.method);
    attempt.stage = "set-state";
    g_corridor_api.set_state.call(c, 0, 0, g_corridor_api.set_state.method);
    bool still_constructed = true;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(c), g_build_complete_field, &still_constructed);
    if (still_constructed) {
        --g_applying_remote;
        LOG_WARN("build: corridor could not enter construction state id=%s", e.object_id);
        return false; // record remains: never retry after touching the target base
    }
    float amount = 1.0f;
    il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(c), g_build_amount_field, &amount);
    const uint8_t transitioned = g_corridor_api.set_state.call(c, 1, 0, g_corridor_api.set_state.method);
    --g_applying_remote;
    const BuildGridPoint cell{snapshot.target_offset[0], snapshot.target_offset[1], snapshot.target_offset[2]};
    const uint8_t actual = build_live(target) ?
        g_corridor_api.get_cell.call(target, cell, g_corridor_api.get_cell.method) : 255;
    record->verified = transitioned && actual == base_geometry::piece_cell_type(e.tech_type) && build_corridor_glass_matches(target, cell, e.tech_type);
    if (record->verified && e.tech_type==5509) record->verified=build_foundation_footprint_matches(target,snapshot,false);
    if (record->verified && e.tech_type==5500) record->verified=build_room_footprint_matches(target,snapshot,false);
    if (record->verified) build_remember_corridor(e, snapshot);
    LOG_INFO("build: corridor applied id=%s transition=%u target_cell=%u verified=%u tech=%d",
             e.object_id, static_cast<unsigned>(transitioned), static_cast<unsigned>(actual),
             static_cast<unsigned>(record->verified), e.tech_type);
    return record->verified;
}

bool build_apply_completed_corridor(void* prefab, void* target, const WorldEvent& e,
                                    const base_geometry::GhostEnvelope& snapshot) {
    if (g_corridor_apply_failed) return false;
    CorridorAttempt attempt;
    const int prior_remote = g_applying_remote;
    bool verified = false;
    try {
        verified = build_apply_completed_corridor_inner(prefab, target, e, snapshot, attempt);
    } catch (...) {
        // IL2CPP throws C++ exceptions. Never let one skip the remote-state reset
        // or repeat a partly applied build on the following Player.Update.
        g_corridor_apply_failed = true;
        LOG_WARN("build: corridor game exception id=%s stage=%s; apply disabled until restart",
                 e.object_id, attempt.stage);
    }
    g_applying_remote = prior_remote;
    if (!verified && attempt.object && !attempt.placed) {
        try {
            if (!build_live(attempt.object)) return false;
            void* placement = nullptr;
            if (build_live(target) && g_corridor_api.placement_ghost) {
                il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(target),
                                       g_corridor_api.placement_ghost, &placement);
                if (placement == attempt.ghost) {
                    if (!build_corridor_set_reference(target, g_corridor_api.placement_ghost, nullptr)) {
                        g_corridor_apply_failed = true;
                        LOG_WARN("build: corridor reference cleanup failed id=%s", e.object_id);
                    }
                }
            }
            g_object_destroy.call(attempt.object, g_object_destroy.method);
        } catch (...) {
            g_corridor_apply_failed = true;
            LOG_WARN("build: corridor cleanup failed id=%s; apply disabled", e.object_id);
        }
    }
    return verified;
}

using BuildClearCellFn = void (*)(void*, BuildGridPoint, void*);
using BuildDeconstructionAllowedFn = uint8_t (*)(void*, void**, void*);
struct CorridorRemovalApi {
    bool attempted = false, ready = false;
    void *type = nullptr, *bounds = nullptr, *face = nullptr, *recipe = nullptr;
    Entry<BuildClearCellFn> clear_cell;
    Entry<ManagedFn> fix_floors, fix_links, rebuild;
    Entry<BuildDeconstructionAllowedFn> allowed;
};
CorridorRemovalApi g_corridor_removal_api;
bool g_corridor_removal_failed = false;

bool build_resolve_corridor_removal() {
    auto& a = g_corridor_removal_api;
    if (a.attempted) return a.ready;
    a.attempted = true;
    if (!build_resolve_corridor_api()) return false;
    auto* cls = build_class("BaseDeconstructable");
    const void* type = cls ? il2cpp_class_get_type(cls) : nullptr;
    a.type = type ? il2cpp_type_get_object(type) : nullptr;
    a.bounds = build_field_on(cls, "bounds", 0x24);
    a.face = build_field_on(cls, "face", 0x3C);
    a.recipe = build_field_on(cls, "recipe", 0x60);
    a.allowed.bind(find_method(cls, "DeconstructionAllowed", 1));
    a.clear_cell.bind(build_typed_method(g_corridor_api.base, "ClearCell", 1, "Int3"));
    a.fix_floors.bind(find_method(g_corridor_api.base, "FixRoomFloors", 0));
    a.fix_links.bind(find_method(g_corridor_api.base, "FixCorridorLinks", 0));
    a.rebuild.bind(find_method(g_corridor_api.base, "RebuildGeometry", 0));
    a.ready = a.type && a.bounds && a.face && a.recipe && a.allowed && a.clear_cell && a.fix_floors && a.fix_links && a.rebuild;
    LOG_INFO("build: corridor removal API %s", a.ready ? "ready" : "unavailable");
    return a.ready;
}

// Returns true when done (including a permanent refusal); false requests a retry.
bool build_apply_corridor_removal_inner(void* target, const WorldEvent& e,
                                        const base_geometry::GhostEnvelope& geometry) {
    if (g_corridor_removal_failed || e.kind != kWorldEventRemove || e.face_has_value ||
        !base_geometry::supported_existing_piece(geometry, e.tech_type) || geometry.object_id != e.object_id)
        return true;
    auto* identity = build_synced_corridor(e.object_id);
    if (!identity || identity->geometry.object_id != e.object_id || identity->state.tech_type != e.tech_type ||
        !base_geometry::same_corridor_cell(identity->geometry, geometry) ||
        identity->state.geometry_hash_low != e.geometry_hash_low ||
        identity->state.geometry_hash_high != e.geometry_hash_high) {
        LOG_WARN("build: corridor removal refused id=%s: no matching session placement", e.object_id);
        return true;
    }
    if (build_cell_has_replacement(geometry,e.object_id)) return true;
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
            !build_foundation_has_other_root(target)) return true;
    } else {
    // Never remove the final cell/base through this first implementation.
    void* cells = nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(target), g_ghost_capture.cells, &cells);
    const int32_t length = cells ? g_array_get_length.call(cells, g_array_get_length.method) : 0;
    if (length <= 0 || length > 1048576) return true;
    int occupied = 0;
    auto* bytes = reinterpret_cast<uint8_t*>(cells) + 0x20;
    for (int32_t i=0; i<length && occupied<2; ++i) if (bytes[i]) ++occupied;
    if (occupied < 2) return true;
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
    const int prior_remote = g_applying_remote;
    const char* stage = "clear-cell";
    ++g_applying_remote;
    try {
        // Same single-cell and repair methods called by BaseDeconstructable.Deconstruct
        // (2A919E4, 2A91AF8, 2A91B04, 2A91B10), with no temporary resource ghost.
        a.clear_cell.call(target, cell, a.clear_cell.method);
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
