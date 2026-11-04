// Run only on the Unity thread. Pose carries identity, never a cross-device pointer.
bool avatar_object_live(RemoteAvatar* avatar) {
    const bool alive=avatar && avatar->game_object && (!g_build_alive || build_live(avatar->game_object));
    if (!alive && g_build_alive)
        for (auto& arms:g_remote_arms) if (arms && !build_live(arms)) arms=nullptr;
    return alive;
}
bool avatar_environment_id(void* go,uint8_t id[16]) {
    if (!go || !g_unique_identifier_type || !g_unique_get_id || !g_get_component) return false;
    void* uid=nullptr;
    walk_parents(go,[&](void* node) {
        uid=g_get_component.call(node,g_unique_identifier_type,g_get_component.method);
        return uid!=nullptr;
    });
    char text[128]{};
    if (!uid || !string_to_ascii(g_unique_get_id.call(uid,g_unique_get_id.method),text,sizeof(text)) || !text[0]) return false;
    guid_from_string(text,id);
    return true;
}
void read_avatar_environment(void* player,LocalState* state) {
    memset(state->environment_id,0,sizeof(state->environment_id));
    if (!(state->flags & kFlagInside) || !g_avatar_get_environment || !g_build_alive) return;
    void* environment=g_avatar_get_environment.call(player,g_avatar_get_environment.method);
    if (!build_live(environment)) return;
    avatar_environment_id(environment,state->environment_id);
}
void update_avatar_environment_inner(RemoteAvatar* avatar,const PeerState& peer) {
    if (!avatar->sky_applier || !g_set_parent || !g_avatar_apply_environment || !g_build_alive) return;
    uint8_t wanted[16]{};
    if (peer.flags & kFlagInside) memcpy(wanted,peer.environment_id,sizeof(wanted));
    const uint8_t zero[16]{};
    const bool inside=memcmp(wanted,zero,sizeof(wanted))!=0;
    const bool changed=memcmp(wanted,avatar->sky_environment_id,sizeof(wanted))!=0;
    if (!changed && avatar->sky_environment_bound==inside) return;
    if (!changed && g_avatar_frame<avatar->sky_environment_next) return;
    if (changed) {
        memcpy(avatar->sky_environment_id,wanted,sizeof(wanted));
        avatar->sky_environment_next=0;
    }
    avatar->sky_environment_next=g_avatar_frame+30;
    // Detach old environment immediately on exit/change, preserving world pose.
    if (avatar->sky_environment_bound) {
        g_set_parent.call(avatar->transform,nullptr,true,g_set_parent.method);
        avatar->sky_environment_bound=false;
        g_avatar_apply_environment.call(avatar->sky_applier,nullptr,g_avatar_apply_environment.method);
    }
    if (!inside) {
        LOG_INFO("sky environment: peer=%08x outdoors",peer.id);
        return;
    }
    if (!g_avatar_subroot_type || !g_find_objects_all || !g_array_get_length || !g_array_get_value) return;
    void* list=g_find_objects_all.call(g_avatar_subroot_type,false,g_find_objects_all.method);
    const int count=list ? g_array_get_length.call(list,g_array_get_length.method) : 0;
    if (count<0 || count>4096) return;
    void* found=nullptr;
    for (int i=0;i<count;++i) {
        void* sub=g_array_get_value.call(list,i,g_array_get_value.method);
        if (!build_live(sub)) continue;
        void* go=g_get_game_object.call(sub,g_get_game_object.method);
        uint8_t id[16]{};
        if (!avatar_environment_id(go,id) || memcmp(id,wanted,sizeof(id))) continue;
        if (found) return; // duplicate identity is ambiguous
        found=go;
    }
    if (!found) {
        LOG_INFO("sky environment: peer=%08x waiting for shared interior %02x%02x%02x%02x",
                 peer.id,wanted[0],wanted[1],wanted[2],wanted[3]);
        return;
    }
    void* tf=g_get_go_transform.call(found,g_get_go_transform.method);
    if (!tf) return;
    g_set_parent.call(avatar->transform,tf,true,g_set_parent.method);
    // Native parent lookup now also works on later movement/power/environment updates.
    g_avatar_apply_environment.call(avatar->sky_applier,found,g_avatar_apply_environment.method);
    avatar->sky_environment_bound=true;
    LOG_INFO("sky environment: peer=%08x bound to shared interior %02x%02x%02x%02x",
             peer.id,wanted[0],wanted[1],wanted[2],wanted[3]);
    log_sky_applier_fields("interior-bound",avatar->sky_applier,-1);
}
void update_avatar_environment(RemoteAvatar* avatar,const PeerState& peer) {
    static bool failed=false;
    if (failed) return;
    try { update_avatar_environment_inner(avatar,peer); }
    catch (...) {
        failed=true;
        LOG_WARN("sky environment: game exception peer=%08x; environment binding disabled until restart",peer.id);
    }
}
