// Opt-in, finite diagnostic only. No creature packets, spawning or saved state.
constexpr const char* kCreatureProbeRequest="/sdcard/Android/data/com.unknownworlds.subnautica/files/mod_log/creature_probe.request";
struct CreatureProbeSaved {void* component=nullptr;bool enabled=false;};
struct CreatureProbe {
    bool ready=false,failed=false,active=false,frozen=false;
    double requested=0,started=0,next=0,next_file=0;
    void* fish=nullptr;void* body=nullptr;void* roots=nullptr;
    bool kinematic=false;Vector3 initial{},freeze_position{};
    float maximum_drift=0;unsigned reenabled=0;
    char id[128]{};
    CreatureProbeSaved saved[48]{};int count=0;
    Entry<PredicateFn> exists;
    Entry<void* (*)(void*,void*,bool,void*)> components;
    Entry<void (*)(void*,bool,void*)> allow;
    Entry<PredicateFn> alive;
    void* live_type=nullptr;
    void* fish_type=nullptr;void* action_type=nullptr;void* waterpark_type=nullptr;
    Il2CppClass* fish_class=nullptr;void* waterpark_field=nullptr;
} g_creature_probe;
bool probe_exists(void* object){auto& p=g_creature_probe;return object&&p.exists&&p.exists.call(object,p.exists.method);}
bool probe_resolve() {
    auto& p=g_creature_probe;if(p.ready)return true;if(p.failed)return false;
    auto* creature=build_class("Creature");p.fish_class=build_class("Boomerang");
    auto* action=build_class("CreatureAction");auto* park=build_class("WaterParkCreature");
    if(!creature||!p.fish_class||!action||!park||!g_behaviour_set_enabled||!g_behaviour_get_enabled||!g_contents_api.array_set)return false;
    p.fish_type=il2cpp_type_get_object(il2cpp_class_get_type(p.fish_class));
    p.action_type=il2cpp_type_get_object(il2cpp_class_get_type(action));
    p.waterpark_type=il2cpp_type_get_object(il2cpp_class_get_type(park));
    p.waterpark_field=build_field_on(build_class("WaterParkItem"),"currentWaterPark",0x30);
    p.exists.bind(find_method(find_class_in_domain("UnityEngine","Object"),"op_Implicit",1));
    p.components.bind(build_typed_method(g_gameobject_class,"GetComponentsInChildren",2,"System.Type"));
    p.allow.bind(find_method(creature,"AllowCreatureUpdates",1));
    auto* live=build_class("LiveMixin");p.alive.bind(find_method(live,"IsAlive",0));
    if(live)p.live_type=il2cpp_type_get_object(il2cpp_class_get_type(live));
    if(!p.exists||!p.components||!p.allow||!p.alive||!p.live_type||!p.waterpark_field||!g_rb_get_kinematic||!g_rb_set_kinematic||!g_rigidbody_type) {
        static double next=0;if(build_now()>=next){next=build_now()+10;
            LOG_WARN("creature probe: API deferred exists=%u components=%u allow=%u alive=%u waterpark=%u rigidbody=%u",unsigned(bool(p.exists)),unsigned(bool(p.components)),unsigned(bool(p.allow)),unsigned(bool(p.alive)),unsigned(p.waterpark_field!=nullptr),unsigned(bool(g_rb_get_kinematic)&&bool(g_rb_set_kinematic)&&g_rigidbody_type));}
        return false;
    }
    p.roots=il2cpp_array_new(find_class_in_domain("System","Object"),50);
    if(!p.roots||!il2cpp_gchandle_new(reinterpret_cast<Il2CppObject*>(p.roots),true)){p.failed=true;return false;}
    p.ready=true;LOG_INFO("creature probe: API ready; explicit request required; guest only; 5s baseline + 20s freeze + 5s restored");return true;
}
void probe_root(int index,void* object) {
    auto& a=g_contents_api;auto& p=g_creature_probe;
    a.array_set.call(p.roots,object,index,a.array_set.method);
}
void probe_add(void* component) {
    auto& p=g_creature_probe;if(!probe_exists(component)||p.count>=48)return;
    for(int i=0;i<p.count;++i)if(p.saved[i].component==component)return;
    p.saved[p.count]={component,g_behaviour_get_enabled.call(component,g_behaviour_get_enabled.method)};
    probe_root(p.count+2,component);++p.count;
}
void probe_indices(void* component,const char* stage) {
    if(!probe_exists(component))return;
    auto* klass=il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(component));
    const char* name=il2cpp_class_get_name(klass);
    const char* fields[]={"<scheduledUpdateIndex>k__BackingField","<managedUpdateIndex>k__BackingField","<managedFixedUpdateIndex>k__BackingField"};
    for(const auto* fieldname:fields) {
        for(auto* current=klass;current;current=il2cpp_class_get_parent(current)) {
            void* iter=nullptr;bool found=false;
            for(auto* f=il2cpp_class_get_fields(current,&iter);f;f=il2cpp_class_get_fields(current,&iter))if(!strcmp(il2cpp_field_get_name(f),fieldname)) {
                int value=0;il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(component),f,&value);
                LOG_INFO("creature probe: stage=%s component=%s field=%s index=%d enabled=%u",stage,name,fieldname,value,unsigned(g_behaviour_get_enabled.call(component,g_behaviour_get_enabled.method)));found=true;break;
            }
            if(found)break;
        }
    }
}
void probe_restore(const char* reason) {
    auto& p=g_creature_probe;if(!p.active)return;
    if(p.frozen) {
        // Restoring Behaviour invokes the game's own registration callbacks.
        if(probe_exists(p.body))g_rb_set_kinematic.call(p.body,p.kinematic,g_rb_set_kinematic.method);
        for(int i=p.count-1;i>=0;--i)if(probe_exists(p.saved[i].component))
            g_behaviour_set_enabled.call(p.saved[i].component,p.saved[i].enabled,g_behaviour_set_enabled.method);
        if(probe_exists(p.fish)&&p.saved[0].enabled)p.allow.call(p.fish,true,p.allow.method);
        for(int i=0;i<p.count;++i)probe_indices(p.saved[i].component,"restored");
    }
    LOG_INFO("creature probe: restored id=%s reason=%s max_freeze_drift=%.5f reenabled=%u",p.id,reason,p.maximum_drift,p.reenabled);
    p.frozen=false;
}
void probe_finish(const char* reason) {
    auto& p=g_creature_probe;probe_restore(reason);
    p.active=false;p.requested=0;p.fish=nullptr;p.body=nullptr;p.count=0;
    if(p.roots)for(int i=0;i<50;++i)probe_root(i,nullptr);
}
bool probe_bind(void* chosen,float distance) {
    auto& p=g_creature_probe;if(!chosen)return false;
    void* go=g_get_game_object.call(chosen,g_get_game_object.method);
    void* body=g_get_component.call(go,g_rigidbody_type,g_get_component.method);
    if(!probe_exists(body))return false;
    std::string id;if(!contents_id(chosen,&id))return false;
    snprintf(p.id,sizeof(p.id),"%s",id.c_str());p.fish=chosen;p.body=body;
    p.count=0;probe_root(0,chosen);probe_root(1,body);probe_add(chosen);
    void* actions=p.components.call(go,p.action_type,true,p.components.method);
    if(actions){int count=g_array_get_length.call(actions,g_array_get_length.method);if(count>32){probe_finish("too many actions");return false;}
        for(int i=0;i<count;++i)probe_add(g_array_get_value.call(actions,i,g_array_get_value.method));}
    const char* names[]={"SwimBehaviour","SplineFollowing","Locomotion","WorldForces","EcoTarget","Pickupable"};
    for(const auto* name:names){auto* c=build_class(name);if(!c)continue;void* type=il2cpp_type_get_object(il2cpp_class_get_type(c));probe_add(g_get_component.call(go,type,g_get_component.method));}
    p.kinematic=g_rb_get_kinematic.call(body,g_rb_get_kinematic.method);
    p.active=true;p.started=build_now();p.next=p.started;p.maximum_drift=0;p.reenabled=0;
    p.initial=g_get_position.call(g_get_transform.call(chosen,g_get_transform.method),g_get_position.method);
    for(int i=0;i<p.count;++i)probe_indices(p.saved[i].component,"baseline");
    LOG_INFO("creature probe: selected id=%s distance=%.2f components=%d body_kinematic=%u",p.id,distance,p.count,unsigned(p.kinematic));return true;
}
bool probe_select(const LocalState& local) {
    auto& p=g_creature_probe;
    void* list=g_find_objects_all.call(p.fish_type,false,g_find_objects_all.method);if(!list)return false;
    const int n=g_array_get_length.call(list,g_array_get_length.method);float best=25*25;void* chosen=nullptr;
    for(int i=0;i<n&&i<4096;++i) {
        void* fish=g_array_get_value.call(list,i,g_array_get_value.method);if(!probe_exists(fish))continue;
        void* go=g_get_game_object.call(fish,g_get_game_object.method);
        if(!go||object_in_local_inventory(go))continue;
        void* live=g_get_component.call(go,p.live_type,g_get_component.method);
        if(!live||!p.alive.call(live,p.alive.method))continue;
        void* park=g_get_component.call(go,p.waterpark_type,g_get_component.method);void* owner=nullptr;
        if(park)il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(park),p.waterpark_field,&owner);
        if(owner)continue;
        void* t=g_get_transform.call(fish,g_get_transform.method);Vector3 pos=g_get_position.call(t,g_get_position.method);
        float d=(pos.x-local.x)*(pos.x-local.x)+(pos.y-local.y)*(pos.y-local.y)+(pos.z-local.z)*(pos.z-local.z);
        if(d<best){best=d;chosen=fish;}
    }
    return probe_bind(chosen,sqrtf(best));
}

void tick_creature_probe(const LocalState& local) try {
    auto& p=g_creature_probe;double now=build_now();
    if(p.active&&(!local.valid||!g_role_known||g_local_is_host||!contents_world_ready())){probe_finish("world/role changed");return;}
    if(!local.valid||!g_role_known||g_local_is_host||!contents_world_ready()||!probe_resolve())return;
    if(!p.active&&now>=p.next_file) {
        p.next_file=now+1;FILE* file=fopen(kCreatureProbeRequest,"rb");
        if(file){char request[64]{};size_t n=fread(request,1,sizeof(request)-1,file);fclose(file);unlink(kCreatureProbeRequest);
            constexpr char command[]="freeze-boomerang-20s";
            if(n==sizeof(command)-1&&!memcmp(request,command,sizeof(command)-1)){p.requested=now;LOG_INFO("creature probe: armed for 120s; searching within 25m");}}
    }
    if(!p.active){if(p.requested&&now-p.requested<120&&now>=p.next){p.next=now+1;probe_select(local);}else if(p.requested&&now-p.requested>=120){LOG_INFO("creature probe: no suitable fish within 25m; request expired");p.requested=0;}return;}
    if(!probe_exists(p.fish)||!probe_exists(p.body)){probe_finish("object unloaded/destroyed");return;}
    double elapsed=now-p.started;
    void* t=g_get_transform.call(p.fish,g_get_transform.method);Vector3 pos=g_get_position.call(t,g_get_position.method);
    if(elapsed>=5&&elapsed<25&&!p.frozen) {
        p.freeze_position=pos;p.allow.call(p.fish,false,p.allow.method);p.frozen=true;
        for(int i=0;i<p.count;++i)if(probe_exists(p.saved[i].component))g_behaviour_set_enabled.call(p.saved[i].component,false,g_behaviour_set_enabled.method);
        g_rb_set_kinematic.call(p.body,true,g_rb_set_kinematic.method);
        for(int i=0;i<p.count;++i)probe_indices(p.saved[i].component,"frozen");
        LOG_INFO("creature probe: freeze started id=%s; Animator not disabled",p.id);
    }
    if(p.frozen&&elapsed>=25)probe_restore("20s complete");
    if(p.frozen) {
        float d=sqrtf((pos.x-p.freeze_position.x)*(pos.x-p.freeze_position.x)+(pos.y-p.freeze_position.y)*(pos.y-p.freeze_position.y)+(pos.z-p.freeze_position.z)*(pos.z-p.freeze_position.z));
        p.maximum_drift=std::max(p.maximum_drift,d);
        for(int i=0;i<p.count;++i)if(probe_exists(p.saved[i].component)&&g_behaviour_get_enabled.call(p.saved[i].component,g_behaviour_get_enabled.method))++p.reenabled;
    }
    if(now>=p.next){p.next=now+1;LOG_INFO("creature probe: sample id=%s elapsed=%.1f frozen=%u position=%.3f,%.3f,%.3f drift=%.5f reenabled=%u",p.id,elapsed,unsigned(p.frozen),pos.x,pos.y,pos.z,p.maximum_drift,p.reenabled);}
    if(elapsed>=30)probe_finish("test complete");
} catch(...){probe_finish("native exception");g_creature_probe.failed=true;LOG_ERROR("creature probe: aborted after exception");}
