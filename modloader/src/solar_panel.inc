// SolarPanel uses the exact-base attachment transport, with native power behaviour.
// Only metadata-verified component fields are read; no relay is chosen by distance.
struct SolarPanelApi {
    bool ready=false,failed=false;
    void *type=nullptr,*source=nullptr,*relay=nullptr,*power=nullptr,*capacity=nullptr,*connection=nullptr;
} g_solar_panel_api;

bool build_solar_resolve() {
    auto& a=g_solar_panel_api;if(a.ready)return true;
    auto* solar=build_class("SolarPanel");auto* source=build_class("PowerSource");
    if(!solar||!source)return false;
    a.type=il2cpp_type_get_object(il2cpp_class_get_type(solar));
    a.source=build_field_on(solar,"powerSource",0x20);
    a.relay=build_field_on(solar,"relay",0x28);
    a.power=build_field_on(source,"power",0x20);
    a.capacity=build_field_on(source,"maxPower",0x24);
    a.connection=build_field_on(source,"connectedRelay",0x30);
    a.ready=a.type&&a.source&&a.relay&&a.power&&a.capacity&&a.connection;
    if(a.ready)LOG_INFO("solar: API ready tech=1517; native completion, generation and relay connection");
    return a.ready;
}

void* build_solar_component(void* go) {
    if(!build_solar_resolve())return nullptr;
    void* list=g_locker_api.children.call(go,g_solar_panel_api.type,true,g_locker_api.children.method);
    if(!list||g_array_get_length.call(list,g_array_get_length.method)!=1)return nullptr;
    void* solar=g_array_get_value.call(list,0,g_array_get_value.method);
    if(!build_live(solar))return nullptr;
    void *source=nullptr,*relay=nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(solar),g_solar_panel_api.source,&source);
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(solar),g_solar_panel_api.relay,&relay);
    return build_live(source)&&build_live(relay)?solar:nullptr;
}

void build_solar_diagnostics() {
    static double next=0;
    if(build_now()<next||!g_solar_panel_api.ready||g_solar_panel_api.failed)return;
    next=build_now()+5;
    void* list=g_find_objects_all.call(g_solar_panel_api.type,true,g_find_objects_all.method);
    const int n=list?g_array_get_length.call(list,g_array_get_length.method):0;
    for(int i=0;i<n;++i) {
        void* solar=g_array_get_value.call(list,i,g_array_get_value.method);
        if(!build_live(solar))continue;
        void* go=g_get_game_object.call(solar,g_get_game_object.method);
        void* c=g_get_component.call(go,g_build_type,g_get_component.method);
        bool complete=false;
        if(!build_live(c)||build_is_preview(c))continue;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(c),g_build_complete_field,&complete);
        if(!complete)continue;
        void *source=nullptr,*connected=nullptr;float power=0,capacity=0;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(solar),g_solar_panel_api.source,&source);
        if(!build_live(source))continue;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(source),g_solar_panel_api.power,&power);
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(source),g_solar_panel_api.capacity,&capacity);
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(source),g_solar_panel_api.connection,&connected);
        char id[kWorldEventObjectIdSize]{};void* uid=build_uid(c);
        if(!uid||!string_to_ascii(g_unique_get_id.call(uid,g_unique_get_id.method),id,sizeof(id)))continue;
        LOG_INFO("solar: native power id=%s charge=%.2f capacity=%.2f source_connected=%u",
                 id,power,capacity,unsigned(build_live(connected)));
    }
}
