#include "managed_reference.h"

// A new path, separate from the failed archive in player_marker.inc. The native
// PingInstance lifecycle owns registration and UI projection. No vehicle, beacon,
// save identifier or custom Canvas is created.
struct PeerHudApi {
    bool ready = false;
    Entry<void (*)(void*,void*,void*)> ctor;
    Entry<bool (*)(void*,void*)> exists;
    Entry<bool (*)(void*,void*)> initialized;
    Entry<GetPositionFn> ping_position;
    void* ping_type = nullptr;
    void* hud_type = nullptr;
    void* origin = nullptr;
    void* id = nullptr;
    void* label = nullptr;
    void* type = nullptr;
    void* visible = nullptr;
    void* display = nullptr;
    void* range = nullptr;
    void* min_distance = nullptr;
    void* hud_pool = nullptr;
    void* hud_prefab = nullptr;
    void* hud_canvas = nullptr;
    void* widget_type = nullptr;
    void* widget_label = nullptr;
    void* widget_icon_color = nullptr;
    void* widget_scale = nullptr;
} g_peer_hud_api;

struct PeerHudMarker {
    ManagedRoot go,transform,ping;
    uint32_t peer=0,context=0;
    bool registered=false;
    double created_at=0,next_create=0,next_report=0;
    player_marker::PoseLease lease;
    unsigned widget_reports=0;
} g_peer_hud;

ManagedRoot::Handle hud_root_create(void* value) {
    return il2cpp_gchandle_new(reinterpret_cast<Il2CppObject*>(value),false);
}
void* hud_root_get(ManagedRoot::Handle handle) {return il2cpp_gchandle_get_target(handle);}
bool hud_retain(ManagedRoot& root,void* value) {
    return root.retain(value,hud_root_create,hud_root_get,il2cpp_gchandle_free);
}
bool hud_live(void* object) {
    auto& a=g_peer_hud_api;
    return object&&a.exists&&a.exists.call(object,a.exists.method);
}
void hud_marker_destroy(const char* reason) {
    auto& m=g_peer_hud;
    void* go=m.go.get();
    if(hud_live(go)) {
        // Only our dedicated GO; never deactivate/destroy the player's avatar.
        g_set_active.call(go,false,g_set_active.method); // OnDisable unregisters immediately.
        g_object_destroy.call(go,g_object_destroy.method);
    }
    if(go)LOG_INFO("peer HUD: removed peer=%08x reason=%s",m.peer,reason);
    m.ping.clear();m.transform.clear();m.go.clear();
    m.peer=0;m.registered=false;m.created_at=0;
}

bool hud_resolve_api() {
    auto& a=g_peer_hud_api;
    if(a.ready)return true;
    if(!il2cpp_field_set_value_object||!il2cpp_field_get_value||!il2cpp_gchandle_new||
       !il2cpp_gchandle_get_target||!il2cpp_gchandle_free||!g_object_destroy||!g_set_active||
       !g_add_component||!g_get_go_transform||!g_set_position||!g_find_objects_all||
       !g_array_get_length||!g_array_get_value)return false;
    auto* go=find_class_in_domain("UnityEngine","GameObject");
    auto* ping=find_class_in_domain("","PingInstance");
    auto* hud=find_class_in_domain("","uGUI_Pings");
    auto* object=find_class_in_domain("UnityEngine","Object");
    if(!go||!ping||!hud||!object)return false;
    a.ctor.bind(find_method_typed(go,".ctor",1,"system.string"));
    a.exists.bind(find_method(object,"op_Implicit",1));
    a.initialized.bind(find_method(ping,"get_Initialized",0));
    a.ping_position.bind(find_method(ping,"GetPosition",0));
    a.ping_type=il2cpp_type_get_object(il2cpp_class_get_type(ping));
    a.hud_type=il2cpp_type_get_object(il2cpp_class_get_type(hud));
    a.origin=ping_field_of(ping,"origin");a.id=ping_field_of(ping,"_id");
    a.label=ping_field_of(ping,"_label");a.type=ping_field_of(ping,"pingType");
    a.visible=ping_field_of(ping,"visible");a.display=ping_field_of(ping,"displayPingInManager");
    a.range=ping_field_of(ping,"range");a.min_distance=ping_field_of(ping,"minDist");
    a.hud_pool=ping_field_of(hud,"poolPings");a.hud_prefab=ping_field_of(hud,"prefabPing");
    a.hud_canvas=ping_field_of(hud,"pingCanvas");
    if(auto* widget=find_class_in_domain("","uGUI_Ping")) {
        a.widget_type=il2cpp_type_get_object(il2cpp_class_get_type(widget));
        a.widget_label=ping_field_of(widget,"_label");
        a.widget_icon_color=ping_field_of(widget,"_iconColor");
        a.widget_scale=ping_field_of(widget,"_scale");
    }
    a.ready=a.ctor&&a.exists&&a.initialized&&a.ping_position&&a.ping_type&&a.hud_type&&a.origin&&a.id&&
        a.label&&a.type&&a.visible&&a.display&&a.range&&a.min_distance&&a.hud_pool&&
        a.hud_prefab&&a.hud_canvas;
    if(a.ready)LOG_INFO("peer HUD: API ready; checked object references, native Start lifecycle");
    return a.ready;
}

bool hud_canvas_ready() {
    auto& a=g_peer_hud_api;
    void* list=g_find_objects_all.call(a.hud_type,false,g_find_objects_all.method);
    if(!list)return false;
    ManagedRoot root;
    if(!hud_retain(root,list))return false;
    const int n=g_array_get_length.call(root.get(),g_array_get_length.method);
    if(n<1||n>8)return false;
    for(int i=0;i<n;++i) {
        void* hud=g_array_get_value.call(root.get(),i,g_array_get_value.method);
        if(!hud_live(hud))continue;
        void* pool=nullptr;void* prefab=nullptr;void* canvas=nullptr;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(hud),a.hud_pool,&pool);
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(hud),a.hud_prefab,&prefab);
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(hud),a.hud_canvas,&canvas);
        if(pool&&hud_live(prefab)&&hud_live(canvas))return true;
    }
    return false;
}

bool hud_marker_create(const PeerState& peer) {
    auto& a=g_peer_hud_api;auto& m=g_peer_hud;
    if(!hud_resolve_api()||!hud_canvas_ready())return false;
    ManagedRoot name;
    if(!hud_retain(name,il2cpp_string_new("SNMP_PeerHud")))return false;
    auto* klass=find_class_in_domain("UnityEngine","GameObject");
    void* go=il2cpp_object_new(klass);
    if(!go||!hud_retain(m.go,go))return false;
    a.ctor.call(m.go.get(),name.get(),a.ctor.method);
    g_set_active.call(m.go.get(),false,g_set_active.method);
    if(!hud_retain(m.transform,g_get_go_transform.call(m.go.get(),g_get_go_transform.method))) {
        hud_marker_destroy("no transform");return false;
    }
    Vector3 position{peer.x,peer.y+1.6f,peer.z};
    g_set_position.call(m.transform.get(),position,g_set_position.method);
    if(!hud_retain(m.ping,g_add_component.call(m.go.get(),a.ping_type,g_add_component.method))) {
        hud_marker_destroy("no PingInstance");return false;
    }
    auto* ping=reinterpret_cast<Il2CppObject*>(m.ping.get());
    ManagedRoot id,label;
    char text[96];snprintf(text,sizeof(text),"snmp-peer-%08x-%08x-%u",net_local_id(),peer.id,g_world_context_generation);
    if(!hud_retain(id,il2cpp_string_new(text))||!hud_retain(label,il2cpp_string_new("Напарник"))||
       !managed_reference_set_checked(ping,a.origin,reinterpret_cast<Il2CppObject*>(m.transform.get()))||
       !managed_reference_set_checked(ping,a.id,reinterpret_cast<Il2CppObject*>(id.get()))||
       !managed_reference_set_checked(ping,a.label,reinterpret_cast<Il2CppObject*>(label.get()))) {
        hud_marker_destroy("reference validation failed");return false;
    }
    int32_t type=2; // The existing Seamoth icon, with a distinct player label.
    bool visible=true,display=true;
    // UpdatePings uses range as FADE-IN width: alpha = lerp(alphaMin,alphaMax,
    // clamp((distance-minDist)/range)). 500 made a nearby marker almost invisible.
    float range=1.0f,min_distance=0.5f;
    il2cpp_field_set_value(ping,a.type,&type);
    il2cpp_field_set_value(ping,a.visible,&visible);
    il2cpp_field_set_value(ping,a.display,&display);
    il2cpp_field_set_value(ping,a.range,&range);
    il2cpp_field_set_value(ping,a.min_distance,&min_distance);
    if(a.initialized.call(m.ping.get(),a.initialized.method)) {
        hud_marker_destroy("unexpected early initialization");return false;
    }
    // This is the exact native getter reached by the old crash's 0x2c98904 frame.
    // Prove it reads our intended transform before HUD registration can see it.
    const Vector3 actual=a.ping_position.call(m.ping.get(),a.ping_position.method);
    if(!std::isfinite(actual.x)||!std::isfinite(actual.y)||!std::isfinite(actual.z)||
       fabsf(actual.x-position.x)>0.01f||fabsf(actual.y-position.y)>0.01f||fabsf(actual.z-position.z)>0.01f) {
        hud_marker_destroy("native position validation failed");return false;
    }
    m.peer=peer.id;m.context=g_world_context_generation;m.created_at=build_now();m.widget_reports=0;
    // OnEnable sees initialized=false; the game's Start then calls Initialize exactly
    // once. No manual registration, no stack references, no copied scene identifier.
    g_set_active.call(m.go.get(),true,g_set_active.method);
    LOG_INFO("peer HUD: created peer=%08x origin=%p reference_verified=1 native_position_verified=1 type=2 context=%u",m.peer,m.transform.get(),m.context);
    return true;
}

void hud_report_widget() {
    auto& a=g_peer_hud_api;auto& m=g_peer_hud;
    if(m.widget_reports>=3||!a.widget_type||!a.widget_label||!a.widget_icon_color||!a.widget_scale)return;
    ++m.widget_reports;
    void* label=nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(m.ping.get()),a.label,&label);
    void* list=g_find_objects_all.call(a.widget_type,true,g_find_objects_all.method);
    ManagedRoot root;
    if(!list||!hud_retain(root,list))return;
    int matched=0;const int n=g_array_get_length.call(root.get(),g_array_get_length.method);
    for(int i=0;i<n&&i<256;++i) {
        void* widget=g_array_get_value.call(root.get(),i,g_array_get_value.method);
        if(!hud_live(widget))continue;
        void* candidate=nullptr;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(widget),a.widget_label,&candidate);
        if(!label||candidate!=label)continue;
        ++matched;float rgba[4]{};float scale=0;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(widget),a.widget_icon_color,rgba);
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(widget),a.widget_scale,&scale);
        void* go=g_get_game_object.call(widget,g_get_game_object.method);
        const bool active=go&&g_go_get_active_hierarchy&&g_go_get_active_hierarchy.call(go,g_go_get_active_hierarchy.method);
        LOG_INFO("peer HUD: widget peer=%08x active=%u icon_alpha=%.3f scale=%.3f",m.peer,unsigned(active),rgba[3],scale);
    }
    LOG_INFO("peer HUD: widget matches=%d peer=%08x",matched,m.peer);
}

void tick_player_hud_marker(const LocalState& local,bool enabled) {
    auto& m=g_peer_hud;auto& a=g_peer_hud_api;
    const auto state=net_session_state();
    if(!enabled||!local.valid||(state!=kSessionHost&&state!=kSessionGuest)) {
        if(m.go.get())hud_marker_destroy("disabled/world not ready");
        m.lease.clear();
        return;
    }
    const double now=build_now();
    PeerState peers[kMaxPeers];const size_t count=net_copy_peers(peers,kMaxPeers);
    const PeerState* peer=nullptr;
    for(size_t i=0;i<count;++i) {
        if(!peers[i].active||!peers[i].seq_valid||!std::isfinite(peers[i].x)||
           !std::isfinite(peers[i].y)||!std::isfinite(peers[i].z))continue;
        if(!peer)peer=&peers[i];
        if(peers[i].id==m.peer){peer=&peers[i];break;}
    }
    if(!peer){if(m.go.get())hud_marker_destroy("peer unavailable");return;}
    if(!m.lease.update(peer->id,peer->sequence,g_world_context_generation,now,kPeerTimeout)) {
        if(m.go.get())hud_marker_destroy("pose expired");
        return;
    }
    if(m.go.get()&&(m.peer!=peer->id||m.context!=g_world_context_generation||
       !hud_live(m.go.get())||!hud_live(m.transform.get())||!hud_live(m.ping.get())))
        hud_marker_destroy("peer/context/object changed");
    if(!m.go.get()) {
        if(now<m.next_create)return;
        m.next_create=now+5;
        if(!hud_marker_create(*peer))LOG_INFO("peer HUD: waiting for initialized native HUD");
        return;
    }
    void* origin=nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(m.ping.get()),a.origin,&origin);
    if(origin!=m.transform.get()){hud_marker_destroy("origin changed");return;}
    g_set_position.call(m.transform.get(),Vector3{peer->x,peer->y+1.6f,peer->z},g_set_position.method);
    if(!m.registered) {
        if(a.initialized.call(m.ping.get(),a.initialized.method)) {
            m.registered=true;
            LOG_INFO("peer HUD: native initialization complete peer=%08x",m.peer);
        } else if(now-m.created_at>3){hud_marker_destroy("Start did not initialize");return;}
    }
    if(now>=m.next_report) {
        m.next_report=now+5;
        LOG_INFO("peer HUD: alive peer=%08x registered=%u reference_verified=1 seq=%u",m.peer,unsigned(m.registered),peer->sequence);
        if(m.registered&&now-m.created_at>0.5)hud_report_widget();
    }
}
