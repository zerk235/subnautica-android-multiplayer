#include "bioreactor_state.h"

struct BioreactorApi {
    bool ready=false,failed=false;
    Il2CppClass* items=nullptr;
    void *type=nullptr,*container=nullptr,*source=nullptr,*count=nullptr,*storage_root=nullptr;
    void *face=nullptr,*constructed=nullptr,*module_prefab=nullptr;
    Entry<BuildObjectFn> get_container;
    Entry<bool (*)(int32_t,void*)> can_add;
    Entry<void* (*)(void*,HatchFace,void*)> get_module;
    Entry<void* (*)(void*,void*,HatchFace,void*)> spawn_module;
    Entry<void (*)(void*,float,void*)> set_constructed;
} g_bioreactor_api;

bool build_bioreactor_resolve() {
    auto& a=g_bioreactor_api;
    if(a.ready)return true;
    auto* reactor=build_class("BaseBioReactor");
    a.items=build_class("ItemsContainer");
    if(!reactor||!a.items)return false;
    a.type=il2cpp_type_get_object(il2cpp_class_get_type(reactor));
    a.container=build_field_on(reactor,"_container",0x50);
    a.source=build_field_on(reactor,"_powerSource",0x60);
    a.count=build_field_on(a.items,"<count>k__BackingField",0x68);
    a.storage_root=build_field_on(reactor,"storageRoot",0x48);
    a.face=build_field_on(reactor,"_moduleFace",0x24);
    a.constructed=build_field_on(reactor,"_constructed",0x34);
    auto* base=build_class("Base");
    a.module_prefab=build_field_on(build_class("BaseAddFaceGhost"),"modulePrefab",0x68);
    a.get_module.bind(find_method(base,"GetModule",1));
    a.spawn_module.bind(find_method(base,"SpawnModule",2));
    a.set_constructed.bind(find_method(reactor,"set_constructed",1));
    a.get_container.bind(find_method(reactor,"get_container",0));
    a.can_add.bind(find_method(reactor,"CanAdd",1));
    a.ready=a.type&&a.container&&a.source&&a.count&&a.storage_root&&a.face&&a.constructed&&
        a.module_prefab&&a.get_module&&a.spawn_module&&a.set_constructed&&a.get_container&&a.can_add;
    if(a.ready)LOG_INFO("bioreactor: API ready tech=5528");
    return a.ready;
}

void* build_bioreactor_component(void* go,bool require_ready) {
    if(!go||!build_bioreactor_resolve())return nullptr;
    void* list=g_locker_api.children.call(go,g_bioreactor_api.type,true,g_locker_api.children.method);
    if(!list||g_array_get_length.call(list,g_array_get_length.method)!=1)return nullptr;
    void* reactor=g_array_get_value.call(list,0,g_array_get_value.method);
    if(!build_live(reactor))return nullptr;
    if(require_ready) {
        void* source=nullptr;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(reactor),g_bioreactor_api.source,&source);
        if(!build_live(source))return nullptr;
    }
    return reactor;
}

int32_t build_bioreactor_fuel(void* reactor) {
    if(!reactor)return -1;
    void* container=nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(reactor),g_bioreactor_api.container,&container);
    if(!container)return 0;
    auto* object=reinterpret_cast<Il2CppObject*>(container);
    if(il2cpp_object_get_class(object)!=g_bioreactor_api.items)return -1;
    int32_t count=-1;
    il2cpp_field_get_value(object,g_bioreactor_api.count,&count);
    return count;
}

void* build_bioreactor_at(void* base,HatchFace face) {
    auto& a=g_bioreactor_api;
    void* reactor=a.get_module.call(base,face,a.get_module.method);
    if(!build_live(reactor))return nullptr;
    auto* object=reinterpret_cast<Il2CppObject*>(reactor);
    if(il2cpp_object_get_class(object)!=build_class("BaseBioReactor"))return nullptr;
    return reactor;
}

bool build_bioreactor_location(void* reactor,void** base,HatchFace* face) {
    auto& a=g_bioreactor_api;
    if(!build_locker_resolve()||!build_bioreactor_resolve()||!build_live(reactor))return false;
    auto* object=reinterpret_cast<Il2CppObject*>(reactor);
    float constructed=0;
    il2cpp_field_get_value(object,a.constructed,&constructed);
    if(constructed!=1.0f)return false;
    il2cpp_field_get_value(object,a.face,face);
    if(face->direction<0||face->direction>=6)return false;
    *base=nullptr;
    walk_parents(g_get_game_object.call(reactor,g_get_game_object.method),[&](void* node){
        void* root=g_get_component.call(node,g_locker_api.root_type,g_get_component.method);
        if(!build_live(root))return false;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(root),g_locker_api.base_field,base);
        return true;
    });
    if(!build_live(*base))return false;
    // IBaseModule.moduleFace is anchor-relative. Base.GetModule takes grid coordinates.
    int32_t anchor[3]{};
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(*base),g_ghost_capture.anchor,anchor);
    face->cell={face->cell.x+anchor[0],face->cell.y+anchor[1],face->cell.z+anchor[2]};
    return build_bioreactor_at(*base,*face)==reactor;
}

bool build_bioreactor_descriptor(void* reactor,base_geometry::GhostEnvelope* geometry) {
    void* base=nullptr;HatchFace face{};
    if(!build_resolve_hatch_capture()||!build_bioreactor_location(reactor,&base,&face))return false;
    char id[kWorldEventObjectIdSize]{};
    void* uid=build_uid(base);
    if(!uid||!string_to_ascii(g_unique_get_id.call(uid,g_unique_get_id.method),id,sizeof(id))||!id[0])return false;
    geometry->target_base_id=id;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(base),g_ghost_capture.anchor,geometry->target_base_anchor.data());
    *geometry=base_geometry::face_descriptor(std::move(*geometry),{face.cell.x,face.cell.y,face.cell.z},
                                             face.direction,base_geometry::kBioreactorTech);
    return base_geometry::face_direction(*geometry,base_geometry::kBioreactorTech)>=0;
}

void build_publish_saved_bioreactors() {
    if(!g_local_is_host||!g_role_known||g_world_live_since<=0||
       build_now()-g_world_live_since<kReidSettleSeconds||!build_resolve_hatch_capture()||
       !build_bioreactor_resolve())return;
    void* list=g_find_objects_all.call(g_bioreactor_api.type,false,g_find_objects_all.method);
    const int count=list?g_array_get_length.call(list,g_array_get_length.method):0;
    for(int i=0;i<count;++i) {
        void* reactor=g_array_get_value.call(list,i,g_array_get_value.method);
        base_geometry::GhostEnvelope geometry;
        if(!build_bioreactor_descriptor(reactor,&geometry)||build_synced_face(geometry))continue;
        WorldEvent state{};
        void* uid=build_uid(reactor);
        if(!uid||!string_to_ascii(g_unique_get_id.call(uid,g_unique_get_id.method),state.object_id,sizeof(state.object_id))||
           !state.object_id[0]||build_corridor_removed(state.object_id))continue;
        state.tech_type=base_geometry::kBioreactorTech;
        state.qw=1;
        state.build_flags=kWorldBuildAnchored;
        guid_from_string(state.object_id,state.guid);
        geometry.object_id=state.object_id;
        if(build_class_bytes(g_build_class_id.call(state.tech_type,g_build_class_id.method),state.class_id))
            build_queue_base_change(state,std::move(geometry),kWorldEventBuild);
    }
}

bool build_apply_bioreactor(void* target,const WorldEvent& e,const base_geometry::GhostEnvelope& wire) try {
    LockerRemoteScope scope;
    base_geometry::GhostEnvelope geometry;
    // Every skip is silent by design; a stalled bioreactor needs to say why.
    static double next_skip_report=0;
    // The state line below used to share the skip window and always won it, which
    // silenced every retry reason. Keep the two budgets apart.
    static double next_state_report=0;
    static double next_prefab_report=0;
    auto skip=[&](const char* reason) {
        if (build_now()>=next_skip_report) {
            next_skip_report=build_now()+2.0;
            LOG_WARN("bioreactor: skip reason=%s id=%s tech=%d flags=%u face_value=%u",reason,e.object_id,
                     e.tech_type,unsigned(e.build_flags),unsigned(e.face_has_value));
        }
        return true;
    };
    // Retry paths were silent too; a job that only ever retries must name its blocker.
    auto retry=[&](const char* reason) {
        if (build_now()>=next_skip_report) {
            next_skip_report=build_now()+2.0;
            LOG_WARN("bioreactor: retry reason=%s id=%s tech=%d",reason,e.object_id,e.tech_type);
        }
        return false;
    };
    if(!build_map_base_geometry(target,wire,&geometry))return skip("map-geometry");
    if(wire.object_id!=e.object_id)return skip("object-id");
    if(e.face_has_value)return skip("face-has-value");
    if(!(e.build_flags&kWorldBuildComplete))return skip("not-complete");
    if(!(e.build_flags&kWorldBuildHasGeometry))return skip("no-geometry");
    const int direction=base_geometry::face_direction(geometry,e.tech_type);
    if(direction<0)return skip("descriptor");
    auto& a=g_bioreactor_api;
    if(a.failed)return true;
    if(!build_bioreactor_resolve()||!build_resolve_hatch_apply()||!build_locker_resolve())return retry("api");
    HatchFace face{{geometry.target_offset[0],geometry.target_offset[1],geometry.target_offset[2]},direction};
    constexpr uint8_t type=base_geometry::kBioreactorFace;
    const uint8_t current=g_hatch_capture.get_face.call(target,face,g_hatch_capture.get_face.method);
    const bool remove=e.kind==kWorldEventRemove;
    if (build_now()>=next_state_report) {
        next_state_report=build_now()+2.0;
        uint8_t ring[6]{};
        for(int d=0;d<6;++d)
            ring[d]=g_hatch_capture.get_face.call(target,HatchFace{face.cell,d},g_hatch_capture.get_face.method);
        // The answer to "why is the module missing" is usually this one flag. The module
        // predicate is the one the game itself uses for wall modules; CanSetFace only
        // covers room/corridor faces and rejects type 14 outright.
        HatchFace probe=face;
        uint8_t can=current==type?1:
            (g_hatch_apply.can_module&&g_hatch_apply.can_module.call(target,&probe,type,g_hatch_apply.can_module.method)?1:0);
        LOG_WARN("bioreactor: state id=%s base=%s cell=(%d,%d,%d) dir=%d current=%u can=%u can_dir=%d ring=%u%u%u%u%u%u kind=%d",
                 e.object_id,geometry.target_base_id.c_str(),face.cell.x,face.cell.y,face.cell.z,
                 direction,unsigned(current),unsigned(can),int(probe.direction),unsigned(ring[0]),
                 unsigned(ring[1]),unsigned(ring[2]),unsigned(ring[3]),unsigned(ring[4]),unsigned(ring[5]),
                 int(e.kind));
    }
    if(remove) {
        if(!build_removal_identity_matches(e,wire)||build_face_has_replacement(geometry,e.object_id))return skip("removal-identity");
        if(contents_locker_busy(e.object_id))return retry("contents-busy");
        if(current!=type)return skip("removal-face-absent");
        void* reactor=build_bioreactor_at(target,face);
        if(!bioreactor_sync::removal_allowed({reactor!=nullptr,build_bioreactor_fuel(reactor)})||
           !build_hatch_removal_allowed(target,face,e.tech_type))return retry("removal-not-allowed");
        if(!build_bind_removal_identity(e,wire))return true;
        a.set_constructed.call(reactor,0.0f,a.set_constructed.method);
        g_hatch_apply.clear_face.call(target,face,type,g_hatch_apply.clear_face.method);
    } else {
        if(build_corridor_removed(e.object_id)||build_face_has_replacement(geometry,e.object_id))return skip("already-removed");
        if(current==type&&build_bioreactor_at(target,face)) {
            build_remember_corridor(e,wire);
            return true;
        }
        if(current!=0&&current!=1&&current!=type)return skip("face-occupied");
        // Wall modules are validated by CanSetModule, not CanSetFace. It takes the face
        // by reference and may rewrite it, so the rewritten face is the one to spawn on.
        if(current!=type) {
            if(!g_hatch_apply.can_module)return skip("module-api");
            if(!g_hatch_apply.can_module.call(target,&face,type,g_hatch_apply.can_module.method))return retry("can-set");
            if(face.direction<0||face.direction>=6)return skip("module-face");
        }
        void* prefab=find_prefab_source(e.tech_type);
        if(build_now()>=next_prefab_report) {
            next_prefab_report=build_now()+5.0;
            LOG_WARN("bioreactor: prefab stage id=%s prefab=%p live=%u",e.object_id,prefab,
                     unsigned(build_live(prefab)));
        }
        if(!build_live(prefab))return retry("prefab");
        void* ghost=g_get_component.call(prefab,g_ghost_capture.ghost_type,g_get_component.method);
        if(!build_live(ghost)) {
            void* constructable=g_get_component.call(prefab,g_build_type,g_get_component.method);
            void* model=nullptr;
            if(build_live(constructable))
                il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(constructable),g_ghost_capture.model,&model);
            if(build_live(model))ghost=g_get_component.call(model,g_ghost_capture.ghost_type,g_get_component.method);
        }
        if(!build_live(ghost))return retry("prefab-ghost");
        auto* object=reinterpret_cast<Il2CppObject*>(ghost);
        auto* cls=il2cpp_object_get_class(object);
        // Wall modules are BaseAddModuleGhost, not BaseAddFaceGhost: accept any ghost that
        // carries the anchored face instead of pinning two class names.
        int ghost_face_offset=0;
        if(!build_ghost_anchored_face(cls,&ghost_face_offset))return skip("prefab-ghost-class");
        void* module_prefab=nullptr;
        il2cpp_field_get_value(object,a.module_prefab,&module_prefab);
        if(!build_live(module_prefab)||!build_bioreactor_component(module_prefab,false))return retry("module-prefab");
        void* go=a.spawn_module.call(target,module_prefab,face,a.spawn_module.method);
        void* reactor=build_bioreactor_component(go,false);
        if(!reactor) {
            if(build_live(go))g_object_destroy.call(go,g_object_destroy.method);
            LOG_WARN("bioreactor: spawn failed id=%s go=%p face=(%d,%d,%d) dir=%d",e.object_id,go,
                     face.cell.x,face.cell.y,face.cell.z,face.direction);
            return retry("spawn");
        }
        a.set_constructed.call(reactor,1.0f,a.set_constructed.method);
        g_hatch_apply.set_face.call(target,face,type,g_hatch_apply.set_face.method);
    }
    auto& rebuild=g_corridor_removal_api;
    rebuild.fix_floors.call(target,rebuild.fix_floors.method);
    rebuild.fix_links.call(target,rebuild.fix_links.method);
    rebuild.rebuild.call(target,rebuild.rebuild.method);
    const uint8_t actual=g_hatch_capture.get_face.call(target,face,g_hatch_capture.get_face.method);
    const bool verified=remove?actual!=type:actual==type&&build_bioreactor_at(target,face);
    if(verified&&!remove)build_remember_corridor(e,wire);
    if(verified&&remove)build_synced_corridor(e.object_id)->removal_applied=true;
    if(!verified)a.failed=true;
    LOG_INFO("bioreactor: %s id=%s base=%s face=%d verified=%u",remove?"removed":"applied",
             e.object_id,geometry.target_base_id.c_str(),direction,unsigned(verified));
    return true;
} catch(...) {
    g_bioreactor_api.failed=true;
    LOG_ERROR("bioreactor: native replay failed id=%s",e.object_id);
    return true;
}
