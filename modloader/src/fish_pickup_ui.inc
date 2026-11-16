// Normal Android reticle/touch button, backed by host-authorized pickup rather
// than by reactivating Pickupable/Creature on a purely visual replica.
struct FishRayHit {Vector3 point,normal;uint32_t face;float distance;float uv[2];int32_t collider;};
static_assert(sizeof(FishRayHit)==44,"RaycastHit layout from this game's metadata");
Entry<uint8_t (*)(Vector3,Vector3,FishRayHit*,float,int32_t,int32_t,void*)> g_fish_raycast;
Entry<void* (*)(FishRayHit*,void*)> g_fish_hit_collider;

bool fish_pickup_bind() {
    auto& a=g_fish_pickup_api;if(a.ready)return true;
    if(!g_contents_api.ready)return false;
    if(!a.item_pickup)a.item_pickup.bind(find_method(g_pickupable_class,"Pickup",1));
    if(!a.item_pickup){LOG_WARN("fish pickup: Pickupable.Pickup(bool) unavailable");return false;}
    auto* hand=build_class("GUIHand"),*reticle=build_class("HandReticle"),*input=build_class("GameInput");
    auto* player=build_class("Player"),*transform=find_class_in_domain("UnityEngine","Transform");
    a.hand_field=build_field_on(player,"guiHand",0x128);
    a.pickup.bind(build_typed_method(g_inventory_class,"Pickup",2,"Pickupable"));
    a.sound.bind(find_method(g_pickupable_class,"PlayPickupSound",0));
    a.late.bind(find_method(reticle,"LateUpdate",0));
    a.hand_mode.bind(find_method(hand,"GetMode",0));a.hand_pda.bind(find_method(hand,"IsPDAInUse",0));
    a.hand_camera.bind(find_method(hand,"GetPlayerCamera",0));a.hand_target.bind(find_method(hand,"GetActiveTarget",0));
    a.forward.bind(find_method(transform,"get_forward",0));
    a.button_down.bind(find_method(input,"GetButtonDown",1));
    a.reticle_pickup.bind(find_method(reticle,"SetPickupState",3));
    a.reticle_distance.bind(find_method(reticle,"SetTargetDistance",1));
    g_fish_raycast.bind(find_method(find_class_in_domain("UnityEngine","Physics"),"Raycast",6));
    g_fish_hit_collider.bind(find_method(find_class_in_domain("UnityEngine","RaycastHit"),"get_collider",0));
    a.ready=a.hand_field&&a.pickup&&a.late&&a.hand_mode&&a.hand_pda&&a.hand_camera&&a.hand_target&&a.forward&&
        a.button_down&&a.reticle_pickup&&a.reticle_distance&&g_fish_raycast&&g_fish_hit_collider;
    if(!a.ready)LOG_WARN("fish pickup: API fields=%u pickup=%u late=%u mode=%u pda=%u camera=%u target=%u forward=%u button=%u reticle=%u distance=%u ray=%u hit=%u",
        unsigned(bool(a.hand_field)),unsigned(bool(a.pickup)),unsigned(bool(a.late)),unsigned(bool(a.hand_mode)),unsigned(bool(a.hand_pda)),
        unsigned(bool(a.hand_camera)),unsigned(bool(a.hand_target)),unsigned(bool(a.forward)),unsigned(bool(a.button_down)),
        unsigned(bool(a.reticle_pickup)),unsigned(bool(a.reticle_distance)),unsigned(bool(g_fish_raycast)),unsigned(bool(g_fish_hit_collider)));
    return a.ready;
}
bool fish_pickup_child_of(void* go,void* root) {
    bool match=false;walk_parents(go,[&](void* n){if(n==root){match=true;return true;}return false;});return match;
}
void fish_pickup_request(const fish_motion::Message& pose) {
    auto& s=g_fish_pickup;
    if(!fish_pickup_live()||s.pending.used||s.removed.count(pose.fish))return;
    void* container=contents_inventory();auto& a=g_contents_api;
    const int tech=fish_motion::tech_for_species(pose.species);
    if(!container||!a.room_tech.call(container,tech,a.room_tech.method)) {
        fish_pickup_notice("SNMP: в инвентаре нет места для рыбы.");return;
    }
    lc::Message q;q.kind=lc::Request;q.recipient=s.host;q.revision=fish_pickup_revision(pose.fish);
    if(++s.counter==0)++s.counter;q.request=(uint64_t(net_local_id())<<32)^s.counter;
    fp::Identity id{pose.session,pose.fish,pose.prefab,pose.epoch};
    q.locker=fp::key(id);q.item={fp::guid(id.fish),tech,{}};
    if(!fish_pickup_send(q))return;
    s.pending={};s.pending.used=true;s.pending.request=q;s.got_done=false;s.next_retry=build_now()+2;
    LOG_INFO("fish pickup: guest requested item=%s request=%llu",q.item.id.c_str(),(unsigned long long)q.request);
}
void fish_pickup_click(void* pickup,void* hand,void* method) try {
    auto native=[&](){if(g_fish_click_original)g_fish_click_original(pickup,hand,method);};
    auto& s=g_fish_pickup;
    if(!g_role_known||!g_boomerang.runtime_ready||!probe_exists(pickup)){native();return;}
    void* go=g_get_game_object.call(pickup,g_get_game_object.method);
    const int tech=int(g_pickup_get_tech.call(pickup,g_pickup_get_tech.method));
    if(!fish_motion::supported_tech(tech)||!probe_exists(go)){native();return;}
    const auto species=fish_motion::species_for_tech(tech);
    void* type=synchronized_fish_type(species);
    void* fish=type?g_get_component.call(go,type,g_get_component.method):nullptr;
    if(!wild_synchronized_fish(fish,go,species)){native();return;}
    if(!fish_pickup_live()) {
        if(s.failed)fish_pickup_notice("SNMP: подбор рыбы остановлен; нужен диагностический отчёт.");
        else if(g_local_is_host&&!net_peer_count())native();
        return;
    }
    fish_motion::Message pose;
    if(!boomerang_identity(build_uid(pickup),pose)){native();return;}
    pose.species=species;
    pose.session=s.session;pose.epoch=s.epoch;pose.host=pose.sender=s.host;pose.nonce=1;pose.kind=fish_motion::LivePose;pose.sequence=1;
    if(s.removed.count(pose.fish))return;
    if(!g_local_is_host){fish_pickup_request(pose);return;}
    fp::Identity id{s.session,pose.fish,pose.prefab,s.epoch};const auto key=fp::key(id);
    auto found=s.hosts.find(key);
    if(found!=s.hosts.end()&&found->second.authority.reserved){fish_pickup_notice("SNMP: эту рыбу уже подбирает напарник.");return;}
    void* inventory=read_static_object_field(g_inventory_class,"main"),*container=contents_inventory();
    if(!inventory||!container||!g_contents_api.room.call(container,pickup,g_contents_api.room.method)){
        fish_pickup_notice("SNMP: в инвентаре нет места для рыбы.");return;
    }
    if(fish_pickup_revision(pose.fish)==UINT64_MAX)return;
    lc::Message done;done.kind=lc::Done;done.locker=key;done.recipient=0;done.revision=fish_pickup_revision(pose.fish)+1;
    if(++s.counter==0)++s.counter;done.request=(uint64_t(net_local_id())<<32)^s.counter;
    done.item.tech=tech;
    if(!contents_id(pickup,&done.item.id)||!fish_pickup_serialize(pickup,&done.item.data)||
        !fish_pickup_journal(done,net_local_id())){fish_pickup_notice("SNMP: не удалось подготовить журнал подбора.");return;}
    contents_suppress(pickup);bool ok=g_fish_pickup_api.pickup.call(inventory,pickup,false,g_fish_pickup_api.pickup.method);
    ContentsNativeItem actual;
    if(!ok||!fish_pickup_inventory_item(done,&actual)) {
        if(fish_pickup_inventory_item(done,&actual)){s.failed=true;return;}
        auto failed=done;failed.kind=lc::Reject;fish_pickup_journal(failed,net_local_id(),true);return;
    }
    if(!fish_pickup_journal(done,net_local_id(),true)){s.failed=true;return;}
    fish_pickup_done_local(done);done.item.data.clear();fish_pickup_send(done);
    LOG_INFO("fish pickup: host local inventory applied item=%s",done.item.id.c_str());
} catch(...){g_fish_pickup.failed=true;fish_pickup_notice("SNMP: ошибка локального подбора рыбы; сохрани диагностику.");}

void fish_pickup_ui(void* reticle) {
    auto& s=g_fish_pickup;auto& a=g_fish_pickup_api;
    if(!fish_pickup_live()||g_local_is_host||!probe_exists(g_local_player))return;
    void* hand=contents_field(g_local_player,a.hand_field);
    if(!probe_exists(hand)||a.hand_mode.call(hand,a.hand_mode.method)!=0||a.hand_pda.call(hand,a.hand_pda.method))return;
    void* camera=a.hand_camera.call(hand,a.hand_camera.method);if(!probe_exists(camera))return;
    void* tr=g_get_transform.call(camera,g_get_transform.method);if(!probe_exists(tr))return;
    const auto origin=g_get_position.call(tr,g_get_position.method),forward=a.forward.call(tr,a.forward.method);
    float nearest=fp::kUseDistance;BoomerangReplica* chosen=nullptr;
    for(auto& r:g_boomerang.replicas) {
        if(!r.handle||r.hidden||!r.written||r.receiver.age(build_now())>.5||fish_pickup_suppressed(r.identity))continue;
        const auto pos=r.last_written;
        float dx=pos.x-origin.x,dy=pos.y-origin.y,dz=pos.z-origin.z;
        const float along=dx*forward.x+dy*forward.y+dz*forward.z;
        if(along<=0||along>=nearest||dx*dx+dy*dy+dz*dz-along*along>.40f*.40f)continue;
        nearest=along;chosen=&r;
    }
    if(!chosen)return;
    void* go=chosen->visual?chosen->visual:g_get_game_object.call(chosen->fish,g_get_game_object.method);
    if(!probe_exists(go))return;
    void* other=a.hand_target.call(hand,a.hand_target.method);
    // Do not perform two interactions from the same tap when the native hand
    // already has a different object (e.g. a locker behind a visual fish).
    if(probe_exists(other)&&!fish_pickup_child_of(other,go))return;
    const auto pos=chosen->last_written;
    const float dx=pos.x-origin.x,dy=pos.y-origin.y,dz=pos.z-origin.z;
    const float distance=sqrtf(dx*dx+dy*dy+dz*dz);if(distance<.001f||distance>fp::kUseDistance)return;
    const Vector3 ray{dx/distance,dy/distance,dz/distance};FishRayHit hit{};
    if(g_fish_raycast.call(origin,ray,&hit,distance,-5,1,g_fish_raycast.method)) {
        void* collider=g_fish_hit_collider.call(&hit,g_fish_hit_collider.method);
        void* hit_go=probe_exists(collider)?g_get_game_object.call(collider,g_get_game_object.method):nullptr;
        if(!probe_exists(hit_go)||!fish_pickup_child_of(hit_go,go))return;
    }
    void* container=contents_inventory();
    const int tech=fish_motion::tech_for_species(chosen->identity.species);
    bool room=container&&g_contents_api.room_tech.call(container,tech,g_contents_api.room_tech.method);
    a.reticle_pickup.call(reticle,tech,room&&!s.pending.used,4,a.reticle_pickup.method);
    a.reticle_distance.call(reticle,distance,a.reticle_distance.method);
    static double next_hint=0;if(build_now()>=next_hint){next_hint=build_now()+5;LOG_INFO("fish pickup: reticle target=%s distance=%.2f room=%u pending=%u",fp::hex(chosen->identity.fish).c_str(),distance,unsigned(room),unsigned(s.pending.used));}
    if(room&&!s.pending.used&&a.button_down.call(4,a.button_down.method))fish_pickup_request(chosen->identity);
}
void fish_pickup_reticle(void* self,void* method) {
    try{fish_pickup_ui(self);}catch(...){g_fish_pickup.failed=true;LOG_ERROR("fish pickup: reticle failed; pickup disabled");}
    if(g_fish_reticle_original)g_fish_reticle_original(self,method);
}
void fish_pickup_install() {
    auto& a=g_fish_pickup_api;static double next=0;
    if(a.hooks||build_now()<next)return;next=build_now()+5;
    if(!fish_pickup_bind()){LOG_WARN("fish pickup: interaction API deferred");return;}
    auto* click=find_method(g_pickupable_class,"OnHandClick",1);
    if(!click)return;
    const auto slot=*reinterpret_cast<uint16_t*>(reinterpret_cast<char*>(click)+kMethodSlotOffset);
    const auto count=*reinterpret_cast<uint16_t*>(reinterpret_cast<char*>(g_pickupable_class)+0x12a);
    if(slot!=9||slot>=count){LOG_WARN("fish pickup: click slot mismatch slot=%u count=%u",slot,count);return;}
    auto* entry=reinterpret_cast<char*>(g_pickupable_class)+kClassVtableOffset+slot*kVirtualInvokeSize;
    void* current=*reinterpret_cast<void**>(entry);
    void* direct=*reinterpret_cast<void**>(reinterpret_cast<char*>(click)+kMethodPointerOffset);
    void* virt=*reinterpret_cast<void**>(reinterpret_cast<char*>(click)+kVirtualPointerOffset);
    if(!current||(current!=direct&&current!=virt))return;
    g_fish_click_original=reinterpret_cast<ContentsClickFn>(current);
    g_fish_reticle_original=a.late.call;
    auto* info=reinterpret_cast<char*>(a.late.method);
    void* old_late=*reinterpret_cast<void**>(info+kMethodPointerOffset);
    void* old_virtual=*reinterpret_cast<void**>(info+kVirtualPointerOffset);
    void* hook=reinterpret_cast<void*>(&fish_pickup_reticle);
    bool patched=write_pointer(entry,reinterpret_cast<void*>(&fish_pickup_click));
    patched=patched&&write_pointer(info+kMethodPointerOffset,hook);
    if(old_virtual)patched=patched&&write_pointer(info+kVirtualPointerOffset,hook);
    if(!patched){
        write_pointer(entry,current);write_pointer(info+kMethodPointerOffset,old_late);
        if(old_virtual)write_pointer(info+kVirtualPointerOffset,old_virtual);
        g_fish_pickup.failed=true;return;
    }
    a.hooks=true;LOG_INFO("fish pickup: hooks ready native hand slot=9 reticle=1 protocol=%u", unsigned(kSessionProtocolVersion));
}
