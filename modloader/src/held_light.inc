// Keep authored lamp transforms under the held visual. Only the owner's sampled
// light state drives them; a copied ToggleLights must never consume local energy.
void* g_toggle_lights_type=nullptr;
void* g_toggle_lights_parent=nullptr;
void* g_toggle_lights_active=nullptr;

template<class Visit> void visit_held_lights(void* root,Visit visit) {
    if (!root || !g_light_type_obj || g_get_component_is_string) return;
    void* stack[128];size_t top=0,visited=0;stack[top++]=root;
    while (top && visited++<128) {
        void* go=stack[--top];
        void* lamp=g_get_component.call(go,g_light_type_obj,g_get_component.method);
        if (lamp) visit(lamp,go);
        void* tf=g_get_go_transform.call(go,g_get_go_transform.method);
        int32_t n=tf ? g_get_child_count.call(tf,g_get_child_count.method) : 0;
        for (int32_t i=0;i<n && top<128;++i) {
            void* child=g_get_child.call(tf,i,g_get_child.method);
            void* child_go=child ? g_get_game_object.call(child,g_get_game_object.method) : nullptr;
            if (child_go) stack[top++]=child_go;
        }
    }
}

void read_local_held_light(LocalState* local) {
    local->flags&=~kFlagToolLight;
    if (!held_light_supported(local->held_tech) || !g_inventory_class ||
        !g_inventory_get_held || !g_pickup_get_tech || !g_behaviour_get_enabled ||
        !g_go_get_active_hierarchy || !g_toggle_lights_type || !g_toggle_lights_active) return;
    void* inventory=read_static_object_field(g_inventory_class,"main");
    void* held=inventory ? g_inventory_get_held.call(inventory,g_inventory_get_held.method) : nullptr;
    if (!held || g_pickup_get_tech.call(held,g_pickup_get_tech.method)!=local->held_tech) return;
    void* go=g_get_game_object.call(held,g_get_game_object.method);
    void* toggle=find_component_in_hierarchy(go,g_toggle_lights_type,nullptr,0);
    bool active=false;
    if (toggle) il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(toggle),g_toggle_lights_active,&active);
    if (!active) return;
    visit_held_lights(go,[&](void* lamp,void* lamp_go) {
        if (g_behaviour_get_enabled.call(lamp,g_behaviour_get_enabled.method) &&
            g_go_get_active_hierarchy.call(lamp_go,g_go_get_active_hierarchy.method)) local->flags|=kFlagToolLight;
    });
}

bool prepare_remote_held_light(RemoteAvatar* avatar,void* prop,uint32_t tech) {
    if (!held_light_supported(tech) || !g_toggle_lights_type || !g_toggle_lights_parent ||
        !g_behaviour_set_enabled || !g_behaviour_get_enabled) return false;
    void* toggle=find_component_in_hierarchy(prop,g_toggle_lights_type,nullptr,0);
    if (!toggle) return false;
    void* parent=nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(toggle),g_toggle_lights_parent,&parent);
    void* root=g_get_go_transform.call(prop,g_get_go_transform.method);
    void* parent_tf=parent ? g_get_go_transform.call(parent,g_get_go_transform.method) : nullptr;
    if (!parent_tf || !prop_owns_transform(root,parent_tf)) return false;
    g_behaviour_set_enabled.call(toggle,false,g_behaviour_set_enabled.method);
    avatar->held_lights_parent=parent;
    avatar->held_light_count=0;
    visit_held_lights(prop,[&](void* lamp,void*) {
        if (avatar->held_light_count<8) {
            unsigned i=avatar->held_light_count++;
            avatar->held_lights[i]=lamp;
            avatar->held_light_enabled[i]=g_behaviour_get_enabled.call(lamp,g_behaviour_get_enabled.method);
        }
        g_behaviour_set_enabled.call(lamp,false,g_behaviour_set_enabled.method);
    });
    g_set_active.call(parent,false,g_set_active.method);
    return true;
}

void apply_peer_held_light(RemoteAvatar* avatar,const PeerState& peer) {
    if (!avatar->held_prop || !avatar->held_lights_parent) return;
    bool on=held_light_visible(avatar->held_tech,peer.held_tech,(peer.flags&kFlagToolLight)!=0);
    g_set_active.call(avatar->held_lights_parent,on,g_set_active.method);
    for (unsigned i=0;i<avatar->held_light_count;++i)
        g_behaviour_set_enabled.call(avatar->held_lights[i],on && avatar->held_light_enabled[i],g_behaviour_set_enabled.method);
    if ((g_avatar_frame%120)==0)
        LOG_INFO("held light rx: peer=%08x tech=%u on=%u lamps=%u",avatar->id,avatar->held_tech,unsigned(on),avatar->held_light_count);
}
