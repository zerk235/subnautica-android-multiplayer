// SetState is called through ConstructableBase's vtable slot 13 by Construct().
// Android 1.22.83315: Construct 0x2CB35C0 loads klass+0x208, then calls it.
struct CompletedBaseBuild {
    bool used = false;
    WorldEvent state{};
    base_geometry::GhostEnvelope geometry;
    double next_send = 0;
    double expires = 0;
};
CompletedBaseBuild g_completed_base_builds[128];
BuildSetStateFn g_base_set_state_original = nullptr;
bool g_base_completion_hook_attempted = false;
double g_base_completion_next_attempt = 0;

bool build_queue_base_change(WorldEvent state, base_geometry::GhostEnvelope geometry, uint8_t kind) {
    state.kind = kind;
    state.origin_id = 0;
    state.sequence = 0;
    state.constructed_amount = 1.0f; // last completed snapshot, also for Remove
    state.build_flags |= kWorldBuildComplete | kWorldBuildHasGeometry;
    std::vector<uint8_t> encoded;
    if (!base_geometry::encode_envelope(geometry, &encoded)) return false;
    const uint64_t hash = base_geometry::envelope_fingerprint(encoded.data(), encoded.size());
    state.geometry_hash_low = static_cast<uint32_t>(hash);
    state.geometry_hash_high = static_cast<uint32_t>(hash >> 32);
    CompletedBaseBuild* slot = nullptr;
    for (auto& pending : g_completed_base_builds) {
        if (pending.used && strcmp(pending.state.object_id, state.object_id) == 0) { slot = &pending; break; }
        if (!pending.used && !slot) slot = &pending;
    }
    auto* identity = build_synced_corridor(state.object_id, true);
    if (!slot || !identity) { LOG_WARN("build: base change queue/identity table full id=%s", state.object_id); return false; }
    if (kind == kWorldEventRemove) identity->removed = true;
    else if (identity->removed) return false;
    identity->state = state;
    identity->geometry = geometry;
#if defined(SNMOD_BASE_GEOMETRY_APPLY)
    if (kind==kWorldEventBuild) build_record_piece_root(state);
#endif
    slot->used = true;
    slot->state = state;
    slot->geometry = std::move(geometry);
    slot->next_send = 0;
    slot->expires = build_now() + kBuildJobDeadlineSeconds;
    LOG_INFO("build: confirmed %s id=%s target=%s offset=(%d,%d,%d) ghost=%s tech=%d",
             kind == kWorldEventRemove ? "deconstruction" : "completion", state.object_id,
             slot->geometry.target_base_id.c_str(), slot->geometry.target_offset[0],
             slot->geometry.target_offset[1], slot->geometry.target_offset[2], slot->geometry.ghost_class.c_str(), state.tech_type);
    return true;
}

bool build_is_preview(void* component) {
    void* preview = g_build_ghost.call(g_build_ghost.method);
    if (!build_live(preview)) return false;
    void* go = g_get_game_object.call(component, g_get_game_object.method);
    return base_geometry::preview_related(go, preview, [](void* node) -> void* {
        void* transform = g_gameobject_get_transform.call(node, g_gameobject_get_transform.method);
        void* parent = transform ? g_transform_get_parent.call(transform, g_transform_get_parent.method) : nullptr;
        return parent ? g_get_game_object.call(parent, g_get_game_object.method) : nullptr;
    });
}

uint8_t build_completed_set_state(void* self, uint8_t value, uint8_t set_amount, void* method) {
    WorldEvent state{};
    base_geometry::GhostEnvelope geometry;
#if defined(SNMOD_BASE_GEOMETRY_APPLY)
    FirstBaseCapture first_base;
#endif
    bool was_complete = true;
    const bool eligible = value && !g_applying_remote && g_build_baseline && g_role_known;
    if (eligible)
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(self), g_build_complete_field, &was_complete);
    const bool preview = eligible && !was_complete && build_is_preview(self);
    return base_geometry::observe_completion(value != 0, was_complete, preview,
        g_applying_remote != 0, eligible,
        [&]() {
            if (!build_capture(self, &state) || !(state.build_flags & kWorldBuildAnchored)) return false;
            geometry.object_id = state.object_id;
            if (state.tech_type==5504) {
                if (state.face_has_value || !build_capture_ladder(self,&geometry)) return false;
            } else if (base_geometry::face_type_for_tech(state.tech_type)>=0) {
                // moduleFace links appliances; a hatch is its own structural face.
                // Room face arrays can exceed shape*6; they are not hatch metadata.
                if ((state.face_has_value && state.tech_type!=base_geometry::kBioreactorTech) ||
                    !build_capture_hatch(self,&geometry,state.tech_type)) {
                    if(state.tech_type==base_geometry::kBioreactorTech)
                        LOG_WARN("bioreactor: construction face capture failed id=%s",state.object_id);
                    return false;
                }
                state.face_has_value=0;
            } else {
                int32_t offset[3]{}, anchor[3]{};
                char target[kWorldEventObjectIdSize]{};
                const char* cls = nullptr;
                if (!build_capture_geometry(self,&geometry.geometry,offset,anchor,target,sizeof(target),&cls,state.tech_type)) return false;
                geometry.target_base_id=target;
                geometry.ghost_class=cls;
                geometry.target_offset={offset[0],offset[1],offset[2]};
                geometry.target_base_anchor={anchor[0],anchor[1],anchor[2]};
                if (geometry.target_base_id.empty() && base_geometry::standalone_piece_topology(geometry,state.tech_type)) {
#if defined(SNMOD_BASE_GEOMETRY_APPLY)
                    if (!build_capture_first_base(self,geometry,&first_base)) return false;
#else
                    return false;
#endif
                } else if (!base_geometry::supported_existing_piece(geometry,state.tech_type)) {
                    LOG_WARN("build: completed piece unsupported id=%s tech=%d ghost=%s target=%s offset=(%d,%d,%d) anchor=(%d,%d,%d); new standalone bases are not supported",
                             state.object_id,state.tech_type,cls,target,offset[0],offset[1],offset[2],anchor[0],anchor[1],anchor[2]);
                    return false;
                }
            }
            void* class_text = g_build_class_id.call(state.tech_type, g_build_class_id.method);
            return build_class_bytes(class_text, state.class_id);
        },
        [&]() { return g_base_set_state_original(self, value, set_amount, method); },
        [&]() {
#if defined(SNMOD_BASE_GEOMETRY_APPLY)
            if (first_base.ghost && !build_finish_first_base(first_base,&geometry,state.tech_type)) {
                LOG_WARN("build: first base capture failed after native completion id=%s",state.object_id);
                return;
            }
#endif
            if (build_queue_base_change(state, std::move(geometry), kWorldEventBuild)) {
#if defined(SNMOD_BASE_GEOMETRY_APPLY)
                if (first_base.completed_base) build_record_piece_root(state,first_base.completed_base);
#endif
            }
        },
        [&](const char* stage) {
            LOG_WARN("build: completion observation failed stage=%s; original game transition preserved", stage);
        });
}

void build_install_completion_hook() {
    if (g_base_completion_hook_attempted || !g_build_base_enabled) return;
    if (build_now() < g_base_completion_next_attempt) return;
    g_base_completion_next_attempt = build_now() + 5.0;
    auto* method = find_method(g_build_base_class, "SetState", 2);
    if (!method) { LOG_WARN("build: completion hook waiting for SetState metadata"); return; }
    uint16_t slot=0, count=0;
    void* address = base_geometry::completion_vtable_entry(g_build_base_class, method, &slot, &count);
    if (!address) {
        LOG_WARN("build: completion hook waiting for validated vtable slot=%u count=%u", slot, count);
        return;
    }
    g_base_set_state_original = reinterpret_cast<BuildSetStateFn>(*reinterpret_cast<void**>(address));
    const bool ok = write_pointer(address, reinterpret_cast<void*>(&build_completed_set_state));
    g_base_completion_hook_attempted = ok;
    LOG_INFO("build: completion hook %s slot=%u count=%u (base scan publishing disabled)", ok ? "ready" : "FAILED", slot, count);
}

void build_send_completed() {
    const double now = build_now();
    for (auto& pending : g_completed_base_builds) {
        if (!pending.used) continue;
        if (pending.state.kind != kWorldEventRemove && build_corridor_removed(pending.state.object_id)) {
            pending = CompletedBaseBuild{};
            continue;
        }
        if (now > pending.expires) { pending = CompletedBaseBuild{}; continue; }
#if defined(SNMOD_BASE_GEOMETRY_APPLY)
        auto* identity=build_synced_corridor(pending.state.object_id);
        if (pending.state.kind==kWorldEventBuild && identity && !identity->root_pose_valid)
            build_record_piece_root(pending.state);
#endif
        if (now < pending.next_send || net_peer_count() == 0) continue;
        if (!net_geometry_enqueue(pending.geometry)) { pending.next_send = now + 1.0; continue; }
        net_post_world_event(pending.state, true);
        pending.next_send = now + kBuildRepairSeconds;
    }
}

BuildObjectFn g_base_deconstruction_original = nullptr;
bool g_base_deconstruction_hook_ready = false;
double g_base_deconstruction_next_attempt = 0;

void* build_final_deconstruction(void* self, void* method) {
    WorldEvent state{};
    base_geometry::GhostEnvelope geometry;
    float amount = 1.0f;
    bool constructed = true;
    const bool eligible = !g_applying_remote && g_build_baseline && g_role_known;
    if (eligible) {
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(self), g_build_amount_field, &amount);
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(self), g_build_complete_field, &constructed);
    }
    const bool preview = eligible && amount == 0 && !constructed && build_is_preview(self);
    return base_geometry::observe_deconstruction(amount, constructed, g_applying_remote != 0, eligible, preview,
        [&]() {
            WorldEvent demolition{};
            if (!build_capture(self, &demolition) ||
                (base_geometry::piece_cell_type(demolition.tech_type) < 0 && base_geometry::face_type_for_tech(demolition.tech_type)<0 && demolition.tech_type!=5504) ||
                (demolition.face_has_value && demolition.tech_type!=base_geometry::kBioreactorTech)) return false;
            demolition.face_has_value=0;
            if (demolition.tech_type==5504) {
                if (!build_capture_ladder(self,&geometry,true)) return false;
            } else if (base_geometry::face_type_for_tech(demolition.tech_type)>=0) {
                if (!build_capture_hatch(self,&geometry,demolition.tech_type)) return false;
            } else {
                int32_t offset[3]{}, anchor[3]{};
                char target[kWorldEventObjectIdSize]{};
                const char* cls = nullptr;
                if (!build_capture_geometry(self,&geometry.geometry,offset,anchor,target,sizeof(target),&cls,demolition.tech_type)) return false;
                geometry.target_base_id=target;
                geometry.ghost_class=cls;
                geometry.target_offset={offset[0],offset[1],offset[2]};
                geometry.target_base_anchor={anchor[0],anchor[1],anchor[2]};
                if (geometry.target_base_id.empty()) {
#if defined(SNMOD_BASE_GEOMETRY_APPLY)
                    auto* detached=build_detached_demolition(self,demolition,geometry);
                    if (detached) {
                        state=detached->state;geometry=detached->geometry;
                        LOG_INFO("build: final empty-base demolition matched id=%s base=%s",state.object_id,geometry.target_base_id.c_str());
                        return true;
                    }
#endif
                    LOG_WARN("build: detached demolition has no exact session cell tech=%d",demolition.tech_type);
                    return false;
                }
                if (!base_geometry::supported_existing_piece(geometry,demolition.tech_type)) return false;
            }
            auto* identity = demolition.tech_type==5504 ? build_synced_ladder(geometry) : base_geometry::face_type_for_tech(demolition.tech_type)>=0 ? build_synced_face(geometry) : build_synced_cell(geometry);
            if (!identity) {
                // Saved geometry survives reloading; its session-only placement
                // record does not. Capture the game's explicit demolition ghost
                // rather than requiring the user to rebuild this saved piece.
                const bool replaced=demolition.tech_type==5504 ? build_ladder_has_replacement(geometry,demolition.object_id) :
                    base_geometry::face_type_for_tech(demolition.tech_type)>=0 ? build_face_has_replacement(geometry,demolition.object_id) :
                    build_cell_has_replacement(geometry,demolition.object_id);
                if (replaced) { LOG_WARN("build: saved deconstruction refused: ambiguous/replaced geometry"); return false; }
                state=demolition;
                geometry.object_id=state.object_id;
                void* class_text=g_build_class_id.call(state.tech_type,g_build_class_id.method);
                if (!build_class_bytes(class_text,state.class_id)) return false;
                LOG_INFO("build: saved deconstruction captured id=%s target=%s tech=%d",state.object_id,geometry.target_base_id.c_str(),state.tech_type);
                return true;
            }
            if (identity->state.tech_type != demolition.tech_type) return false;
            state = identity->state;
            geometry = identity->geometry; // send the exact original snapshot/identity
            return true;
        },
        [&]() { return g_base_deconstruction_original(self, method); },
        [&]() { build_queue_base_change(state, std::move(geometry), kWorldEventRemove); },
        [&](const char* stage) { LOG_WARN("build: deconstruction observation failed stage=%s", stage); });
}

void build_install_deconstruction_hook() {
    if (g_base_deconstruction_hook_ready || !g_build_base_enabled || build_now() < g_base_deconstruction_next_attempt) return;
    g_base_deconstruction_next_attempt = build_now() + 5;
    auto* method = find_method(g_build_base_class, "ProgressDeconstruction", 0);
    uint16_t slot=0, count=0;
    void* address = base_geometry::completion_vtable_entry(g_build_base_class, method, &slot, &count, 16);
    if (!address) { LOG_WARN("build: deconstruction hook waiting slot=%u count=%u", slot, count); return; }
    g_base_deconstruction_original = reinterpret_cast<BuildObjectFn>(*reinterpret_cast<void**>(address));
    g_base_deconstruction_hook_ready = write_pointer(address, reinterpret_cast<void*>(&build_final_deconstruction));
    LOG_INFO("build: deconstruction hook %s slot=%u count=%u", g_base_deconstruction_hook_ready ? "ready" : "FAILED", slot, count);
}
