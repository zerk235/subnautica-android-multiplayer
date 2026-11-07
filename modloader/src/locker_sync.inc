// First ordinary interior Constructable: floor Locker only. Contents are local.
using LockerChildrenFn=void* (*)(void*,void*,bool,void*);
using LockerRotationFn=void (*)(void*,Quaternion,void*);
struct LockerApi {
    bool ready=false,failed=false,hooks=false;
    void *root_type=nullptr,*storage_type=nullptr,*base_field=nullptr,*container_field=nullptr;
    Entry<BuildObjectFn> modules_root;
    Entry<LockerChildrenFn> children;
    Entry<HeldTransformPointFn> inverse_point;
    Entry<BuildSetPositionFn> set_local_position;
    Entry<LockerRotationFn> set_local_rotation;
    Entry<BuildSetStateFn> set_state;
    using AllowedFn=uint8_t (*)(void*,void**,void*);
    Entry<AllowedFn> allowed;
    using EmptyFn=uint8_t (*)(void*,void*);
    Entry<EmptyFn> empty;
} g_locker_api;
BuildSetStateFn g_locker_set_state_original=nullptr;
BuildObjectFn g_locker_deconstruction_original=nullptr;

bool build_locker_resolve() {
    auto& a=g_locker_api;if(a.ready)return true;
    auto* root=build_class("BaseRoot");auto* sub=build_class("SubRoot");
    auto* storage=build_class("StorageContainer");
    auto* go=find_class_in_domain("UnityEngine","GameObject");
    auto* tr=find_class_in_domain("UnityEngine","Transform");
    if(!root||!sub||!storage||!go||!tr)return false;
    a.root_type=il2cpp_type_get_object(il2cpp_class_get_type(root));
    a.storage_type=il2cpp_type_get_object(il2cpp_class_get_type(storage));
    a.base_field=build_field_on(root,"baseComp",0x2B0);
    a.container_field=build_field_on(storage,"<container>k__BackingField",0x68);
    a.modules_root.bind(find_method(sub,"GetModulesRoot",0));
    a.children.bind(build_typed_method(go,"GetComponentsInChildren",2,"System.Type"));
    a.inverse_point.bind(find_method(tr,"InverseTransformPoint",1));
    a.set_local_position.bind(find_method(tr,"set_localPosition",1));
    a.set_local_rotation.bind(find_method(tr,"set_localRotation",1));
    a.set_state.bind(find_method(g_build_class,"SetState",2));
    a.allowed.bind(find_method(g_build_class,"DeconstructionAllowed",1));
    a.empty.bind(find_method(storage,"IsEmpty",0));
    a.ready=a.root_type&&a.storage_type&&a.base_field&&a.container_field&&a.modules_root&&a.children&&
        a.inverse_point&&a.set_local_position&&a.set_local_rotation&&a.set_state&&a.allowed&&a.empty;
    if(a.ready)LOG_INFO("locker: API ready tech=1504 exact BaseRoot parent; contents local");
    return a.ready;
}

void* build_locker_storage(void* go,bool require_container) {
    void* list=g_locker_api.children.call(go,g_locker_api.storage_type,true,g_locker_api.children.method);
    if(!list||g_array_get_length.call(list,g_array_get_length.method)!=1)return nullptr;
    void* storage=g_array_get_value.call(list,0,g_array_get_value.method);
    if(!build_live(storage))return nullptr;
    void* container=nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(storage),g_locker_api.container_field,&container);
    return !require_container||container ? storage : nullptr;
}

bool build_locker_capture(void* c,WorldEvent* out) {
    if(!build_locker_resolve()||!build_capture(c,out)||out->tech_type!=interior_build::kLocker||
       il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(c))!=g_build_class)return false;
    bool inside=false;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(c),g_build_inside_field,&inside);
    void* go=g_get_game_object.call(c,g_get_game_object.method);
    void *base=nullptr,*root=nullptr;
    walk_parents(go,[&](void* node){
        void* r=g_get_component.call(node,g_locker_api.root_type,g_get_component.method);
        if(!build_live(r))return false;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(r),g_locker_api.base_field,&base);
        root=r;return true;
    });
    if(!inside||!build_live(base)||!root) {
        LOG_INFO("locker: capture deferred id=%s inside=%u base=%p",out->object_id,unsigned(inside),base);return false;
    }
    void* uid=build_uid(base);char id[kWorldEventObjectIdSize]{};
    if(!uid||!string_to_ascii(g_unique_get_id.call(uid,g_unique_get_id.method),id,sizeof(id))||!id[0])return false;
    void* modules=g_locker_api.modules_root.call(root,g_locker_api.modules_root.method);
    if(!build_live(modules))return false;
    const auto p=g_locker_api.inverse_point.call(modules,{out->x,out->y,out->z},g_locker_api.inverse_point.method);
    const auto q=g_get_rotation.call(modules,g_get_rotation.method);
    HeldRotation local{};
    if(!held_rotation_relative({q.x,q.y,q.z,q.w},{out->qx,out->qy,out->qz,out->qw},&local))return false;
    guid_from_string(id,out->interior_parent_guid);
    const float pose[7]={p.x,p.y,p.z,local.x,local.y,local.z,local.w};
    memcpy(out->interior_local_pose,pose,sizeof(pose));
    out->build_flags=kWorldBuildComplete|kWorldBuildInterior|kWorldBuildIsInside|kWorldBuildAttached;
    out->constructed_amount=1.0f;out->kind=kWorldEventBuild;
    return build_class_bytes(g_build_class_id.call(out->tech_type,g_build_class_id.method),out->class_id)&&
        interior_build::payload_valid(*out);
}

void build_locker_queue(WorldEvent state,uint8_t kind) {
    state.kind=kind;state.origin_id=state.sequence=0;
    if(!g_locker_registry.accept(state)){LOG_WARN("locker: identity/allocation refused id=%s",state.object_id);return;}
    auto* r=g_locker_registry.find(state.object_id);r->publish=true;
    r->next_send=0;r->expires=build_now()+kBuildJobDeadlineSeconds;
    LOG_INFO("locker: confirmed %s id=%s parent=%02x%02x%02x%02x local=(%.3f,%.3f,%.3f)",
        kind==kWorldEventRemove?"demolition":"completion",state.object_id,
        state.interior_parent_guid[0],state.interior_parent_guid[1],state.interior_parent_guid[2],state.interior_parent_guid[3],
        state.interior_local_pose[0],state.interior_local_pose[1],state.interior_local_pose[2]);
}

uint8_t build_locker_set_state(void* self,uint8_t value,uint8_t set_amount,void* method) {
    bool was_complete=true;WorldEvent state{};
    const bool eligible=value&&!g_applying_remote&&g_build_baseline&&g_role_known;
    if(eligible)il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(self),g_build_complete_field,&was_complete);
    return base_geometry::observe_completion(value,was_complete,eligible&&build_is_preview(self),
        g_applying_remote!=0,eligible,
        [&](){return build_locker_capture(self,&state);},
        [&](){return g_locker_set_state_original(self,value,set_amount,method);},
        [&](){build_locker_queue(state,kWorldEventBuild);},
        [&](const char* stage){LOG_WARN("locker: completion observation failed stage=%s",stage);});
}
void* build_locker_deconstruction(void* self,void* method) {
    WorldEvent state{};float amount=1;bool complete=true;
    const bool eligible=!g_applying_remote&&g_build_baseline&&g_role_known;
    if(eligible){
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(self),g_build_amount_field,&amount);
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(self),g_build_complete_field,&complete);
    }
    return base_geometry::observe_deconstruction(amount,complete,g_applying_remote!=0,eligible,
        eligible&&build_is_preview(self),[&](){return build_locker_capture(self,&state);},
        [&](){return g_locker_deconstruction_original(self,method);},
        [&](){build_locker_queue(state,kWorldEventRemove);},
        [&](const char* stage){LOG_WARN("locker: demolition observation failed stage=%s",stage);});
}
void build_locker_install_hooks() {
    auto& a=g_locker_api;if(a.hooks||!build_locker_resolve())return;
    uint16_t slot=0,count=0;
    auto* completion=find_method(g_build_class,"SetState",2);
    auto* demolition=find_method(g_build_class,"ProgressDeconstruction",0);
    void* cp=base_geometry::completion_vtable_entry(g_build_class,completion,&slot,&count,13);
    void* dp=base_geometry::completion_vtable_entry(g_build_class,demolition,&slot,&count,16);
    if(!cp||!dp)return;
    if(!g_locker_set_state_original) {
        g_locker_set_state_original=reinterpret_cast<BuildSetStateFn>(*static_cast<void**>(cp));
        if(!write_pointer(cp,reinterpret_cast<void*>(&build_locker_set_state))){g_locker_set_state_original=nullptr;return;}
    }
    if(!g_locker_deconstruction_original) {
        g_locker_deconstruction_original=reinterpret_cast<BuildObjectFn>(*static_cast<void**>(dp));
        if(!write_pointer(dp,reinterpret_cast<void*>(&build_locker_deconstruction))){g_locker_deconstruction_original=nullptr;return;}
    }
    a.hooks=true;LOG_INFO("locker: completion and explicit demolition hooks ready slots=13,16");
}
void build_locker_send_pending() {
    const double now=build_now();
    for(auto& entry:g_locker_registry.records) {
        auto& r=entry.second;
        if(!r.publish)continue;
        if(now>r.expires){r.publish=false;continue;}
        if(now<r.next_send||!net_peer_count())continue;
        net_post_world_event(r.state,true);r.next_send=now+kBuildRepairSeconds;
    }
}

void* build_locker_parent(const WorldEvent& e) {
    if(!build_resolve_corridor_api()||!build_resolve_ghost_capture())return nullptr;
    void* list=g_find_objects_all.call(g_locker_api.root_type,true,g_find_objects_all.method);
    int n=list?g_array_get_length.call(list,g_array_get_length.method):0;
    void* found=nullptr;
    for(int i=0;i<n;++i) {
        void* root=g_array_get_value.call(list,i,g_array_get_value.method);
        if(!build_live(root))continue;
        void* base=nullptr;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(root),g_locker_api.base_field,&base);
        void* uid=build_uid(base);char id[kWorldEventObjectIdSize]{};uint8_t guid[16]{};
        if(!uid||!string_to_ascii(g_unique_get_id.call(uid,g_unique_get_id.method),id,sizeof(id)))continue;
        guid_from_string(id,guid);if(memcmp(guid,e.interior_parent_guid,16))continue;
        if(found)return nullptr;
        std::array<int32_t,3> anchor{};
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(base),g_ghost_capture.anchor,anchor.data());
        if(build_corridor_target(id,anchor)!=base)return nullptr;
        found=g_locker_api.modules_root.call(root,g_locker_api.modules_root.method);
        if(!build_live(found))return nullptr;
    }
    return found;
}
struct LockerRemoteScope {int prior=g_applying_remote;LockerRemoteScope(){++g_applying_remote;}~LockerRemoteScope(){g_applying_remote=prior;}};

bool build_apply_locker(const WorldEvent& e) {
    if(!interior_build::payload_valid(e)||g_locker_api.failed)return true;
    if(!build_locker_resolve())return false;
    auto* record=g_locker_registry.find(e.object_id);
    if(!record||!interior_build::same_placement(record->state,e))return true;
    const bool remove=e.kind==kWorldEventRemove;
    if((!remove&&record->removed)||(remove&&record->removal_applied))return true;
    void* owned=nullptr;const char* stage="parent";
    try {
        void* parent=build_locker_parent(e);if(!parent)return false;
        void* existing=build_find(e);WorldEvent actual{};
        if(existing) {
            if(!build_locker_capture(existing,&actual)||!interior_build::same_placement(actual,e)) {
                LOG_WARN("locker: existing placement mismatch id=%s",e.object_id);return true;
            }
            if(!remove) {
                bool complete=false;il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(existing),g_build_complete_field,&complete);
                return complete&&build_locker_storage(g_get_game_object.call(existing,g_get_game_object.method),true);
            }
            void* go=g_get_game_object.call(existing,g_get_game_object.method);
            void* storage=build_locker_storage(go,true);void* reason=nullptr;
            if(!storage||!g_locker_api.empty.call(storage,g_locker_api.empty.method)||
               !g_locker_api.allowed.call(existing,&reason,g_locker_api.allowed.method)) {
                LOG_INFO("locker: removal deferred id=%s storage not empty/ready or native permission blocked",e.object_id);return false;
            }
            LockerRemoteScope scope;
            stage="remove-state";g_locker_api.set_state.call(existing,0,0,g_locker_api.set_state.method);
            stage="destroy";g_object_destroy.call(go,g_object_destroy.method);
            record->removal_applied=true;
            LOG_INFO("locker: removed id=%s verified=1",e.object_id);return true;
        }
        if(remove){record->removal_applied=true;LOG_INFO("locker: already absent id=%s",e.object_id);return true;}
        void* class_text=g_build_class_id.call(e.tech_type,g_build_class_id.method);uint8_t bytes[16]{};
        if(!build_class_bytes(class_text,bytes)||memcmp(bytes,e.class_id,16))return true;
        void* prefab=nullptr;g_build_scene_prefab.call(class_text,&prefab,g_build_scene_prefab.method);
        if(!build_live(prefab))prefab=find_prefab_source(uint32_t(e.tech_type));
        if(!build_live(prefab))return false;
        void* pc=g_get_component.call(prefab,g_build_type,g_get_component.method);int32_t tech=0;
        if(!pc||il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(pc))!=g_build_class)return true;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(pc),g_build_tech_field,&tech);
        if(tech!=interior_build::kLocker||!build_locker_storage(prefab,false)||g_build_id_root_count>=kBuildKnownLimit)return true;
        LockerRemoteScope scope;
        stage="instantiate";owned=g_build_instantiate.call(prefab,e.tech_type,false,g_build_instantiate.method);
        if(!build_live(owned))return false;
        void* c=g_get_component.call(owned,g_build_type,g_get_component.method);
        void* uid=build_uid(c);void* tr=g_gameobject_get_transform.call(owned,g_gameobject_get_transform.method);
        if(!c||!uid||!tr)throw "locker components missing";
        void* id=il2cpp_string_new(e.object_id);
        if(!id||!build_keep_id_root(il2cpp_gchandle_new(reinterpret_cast<Il2CppObject*>(id),true)))throw "locker ID root unavailable";
        g_uid_set_id.call(uid,id,g_uid_set_id.method);
        stage="parent-and-pose";g_set_parent.call(tr,parent,false,g_set_parent.method);
        const auto* p=e.interior_local_pose;
        g_locker_api.set_local_position.call(tr,{p[0],p[1],p[2]},g_locker_api.set_local_position.method);
        g_locker_api.set_local_rotation.call(tr,{p[3],p[4],p[5],p[6]},g_locker_api.set_local_rotation.method);
        bool yes=true;
        il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(c),g_build_inside_field,&yes);
        il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(c),g_build_attached_field,&yes);
        stage="native-completion";
        g_locker_api.set_state.call(c,0,0,g_locker_api.set_state.method);
        float amount=1;il2cpp_field_set_value(reinterpret_cast<Il2CppObject*>(c),g_build_amount_field,&amount);
        g_locker_api.set_state.call(c,1,0,g_locker_api.set_state.method);
        bool complete=false;il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(c),g_build_complete_field,&complete);
        void* storage=build_locker_storage(owned,true);
        if(!complete||!storage||!g_locker_api.empty.call(storage,g_locker_api.empty.method)||
           !build_locker_capture(c,&actual)||!interior_build::same_placement(actual,e))throw "locker completion readback failed";
        owned=nullptr;
        LOG_INFO("locker: applied id=%s verified=1 parent=exact local_pose=matched storage=ready",e.object_id);
        return true;
    } catch(...) {
        g_locker_api.failed=true;
        try {if(owned&&build_live(owned))g_object_destroy.call(owned,g_object_destroy.method);}catch(...){}
        LOG_WARN("locker: exception id=%s stage=%s; locker replay disabled until restart",e.object_id,stage);
        return true;
    }
}
