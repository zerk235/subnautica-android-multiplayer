// Player.SetHeadVisible(false) uses Renderer.shadowCastingMode=ShadowsOnly (3),
// not GameObject.SetActive. A body copy inherits that first-person render state.
// Fix only known head renderers on this copy; suit selection and the source stay
// unchanged. Inactive suit heads remain inactive and cannot overlap the peer's gear.
bool avatar_head_mesh_name(const char* name) {
    return name && (!strcmp(name,"diveSuit_head_geo") ||
        !strcmp(name,"radiationSuit_head_geo") || !strcmp(name,"scuba_head"));
}
bool restore_avatar_head_visibility(void* clone_go,void* source_go,uint32_t peer_id) {
    if (!clone_go || clone_go==source_go || clone_go==g_local_game_object ||
        !g_get_component || !g_get_go_transform || !g_get_game_object || !g_get_name ||
        !g_get_child_count || !g_get_child || !g_head_get_shadow || !g_head_set_shadow ||
        !(g_get_component_is_string ? g_renderer_name_str : g_renderer_type_obj)) return false;
    void* stack[512]; size_t top=0,visited=0; unsigned found=0,changed=0;
    stack[top++]=clone_go;
    bool okay=true;
    while (top && visited++<512) {
        void* go=stack[--top];
        // Never follow an unexpected reference back into the original first-person tree.
        if (go==source_go || go==g_local_game_object) { okay=false; continue; }
        char name[128]{};
        read_managed_name(g_get_name.call(go,g_get_name.method),name,sizeof(name));
        if (avatar_head_mesh_name(name)) {
            void* renderer=g_get_component.call(go,g_get_component_is_string ?
                g_renderer_name_str : g_renderer_type_obj,g_get_component.method);
            if (renderer && g_get_game_object.call(renderer,g_get_game_object.method)==go) {
                ++found;
                int before=g_head_get_shadow.call(renderer,g_head_get_shadow.method);
                if (before==3) {
                    g_head_set_shadow.call(renderer,1,g_head_set_shadow.method);
                    ++changed;
                }
                int after=g_head_get_shadow.call(renderer,g_head_get_shadow.method);
                okay=okay && after!=3;
                bool active=g_go_get_active_hierarchy &&
                    g_go_get_active_hierarchy.call(go,g_go_get_active_hierarchy.method);
                LOG_INFO("avatar head: peer=%08x name=%s renderer=%p active=%u shadow=%d->%d",
                    peer_id,name,renderer,unsigned(active),before,after);
            } else {
                okay=false;
                LOG_WARN("avatar head: peer=%08x name=%s missing owned Renderer",peer_id,name);
            }
        }
        void* tf=g_get_go_transform.call(go,g_get_go_transform.method);
        int32_t n=tf ? g_get_child_count.call(tf,g_get_child_count.method) : 0;
        if (n<0 || n>512) { okay=false; continue; }
        for (int32_t i=0;i<n;++i) {
            void* child=g_get_child.call(tf,i,g_get_child.method);
            void* child_go=child ? g_get_game_object.call(child,g_get_game_object.method) : nullptr;
            if (!child_go) continue;
            if (top>=512) { okay=false; break; }
            stack[top++]=child_go;
        }
    }
    bool complete=okay && !top && found>0;
    LOG_INFO("avatar head: peer=%08x scanned=%u found=%u changed=%u complete=%u",
        peer_id,unsigned(visited),found,changed,unsigned(complete));
    return complete;
}
