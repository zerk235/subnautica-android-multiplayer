// Unity-thread transaction coordinator. Transport reuses the bounded ordered
// event stream and item serializer already used by shared lockers.
#include "fish_pickup.h"
namespace fp=fish_pickup;
struct FishPickupHost {lc::Authority authority;bool initialized=false;};
struct FishPickupApi {
    bool ready=false,hooks=false;
    void* hand_field=nullptr;
    Entry<uint8_t (*)(void*,void*,bool,void*)> pickup;
    Entry<PickupFn> item_pickup;
    Entry<void (*)(void*,void*)> sound;
    Entry<ManagedFn> late;
    Entry<GetPropIntFn> hand_mode;
    Entry<GetPropBoolFn> hand_pda;
    Entry<BuildObjectFn> hand_camera,hand_target;
    Entry<GetVector3Fn> forward;
    Entry<uint8_t (*)(int32_t,void*)> button_down;
    Entry<void (*)(void*,int32_t,bool,int32_t,void*)> reticle_pickup;
    Entry<void (*)(void*,float,void*)> reticle_distance;
} g_fish_pickup_api;
struct FishPickupLoad {
    ManagedRoot roots;
    void* task=nullptr,*result_field=nullptr,*value_field=nullptr;
    double started=0,next_notice=0;
} g_fish_pickup_load;
struct FishPickupState {
    bool context=false,failed=false,journals_checked=false,got_done=false;
    fp::Id session{};uint32_t epoch=0,host=0,generation=0;
    uint64_t counter=0,message=0;
    double next_retry=0;
    fp::Pending pending;
    lc::Message granted;
    std::map<std::string,FishPickupHost> hosts;
    std::map<fp::Id,uint64_t> removed;
    std::map<fp::Id,uint64_t> revisions,reset_revisions;
    std::map<fp::Id,uint32_t> pose_floors;
    std::deque<ContentsOutgoing> outgoing;
    std::deque<std::pair<uint32_t,lc::Message>> incoming;
    std::map<uint32_t,lc::Assembly> assembly;
} g_fish_pickup;
ManagedFn g_fish_reticle_original=nullptr;
ContentsClickFn g_fish_click_original=nullptr;
void fish_pickup_reticle(void* self,void* method);
void fish_pickup_click(void* pickup,void* hand,void* method);
void* fish_pickup_field(Il2CppClass* klass,const char* name) {
    for(int depth=0;klass&&depth<12;++depth,klass=il2cpp_class_get_parent(klass)) {
        void* it=nullptr;
        for(auto* f=il2cpp_class_get_fields(klass,&it);f;f=il2cpp_class_get_fields(klass,&it))
            if(!strcmp(il2cpp_field_get_name(f),name))return f;
    }
    return nullptr;
}

void fish_pickup_notice(const char* text) {
    LOG_INFO("fish pickup: %s",text);
    if(g_contents_api.notify)g_contents_api.notify.call(il2cpp_string_new(text),g_contents_api.notify.method);
}
constexpr const char* kFishJournal="/sdcard/Android/data/com.unknownworlds.subnautica/files/mod_log/fish_pickup";
std::string fish_pickup_journal_path(const lc::Message& m,uint32_t peer) {
    fp::Identity id;if(!fp::identity(m.locker,&id))return {};
    char suffix[48];snprintf(suffix,sizeof(suffix),"-%08x-%016llx.pending",peer,(unsigned long long)m.request);
    return std::string(kFishJournal)+"/"+fp::hex(id.session)+"-"+fp::hex(id.fish)+suffix;
}
bool fish_pickup_journal(const lc::Message& m,uint32_t peer,bool finished=false) {
    const auto path=fish_pickup_journal_path(m,peer);if(path.empty())return false;
    if(mkdir(kFishJournal,0770)&&errno!=EEXIST)return false;
    std::vector<uint8_t> bytes;if(!lc::encode(m,&bytes))return false;
    const auto temporary=path+".tmp";FILE* f=fopen(temporary.c_str(),"wb");if(!f)return false;
    bool ok=fwrite(bytes.data(),1,bytes.size(),f)==bytes.size();
    if(fflush(f)||fsync(fileno(f)))ok=false;if(fclose(f))ok=false;
    if(!ok||rename(temporary.c_str(),path.c_str())){unlink(temporary.c_str());return false;}
    if(finished&&rename(path.c_str(),(path+".done").c_str()))return false;
    return true;
}
bool fish_pickup_check_journals() {
    DIR* d=opendir(kFishJournal);if(!d)return errno==ENOENT;
    bool clear=true;size_t count=0;
    while(auto* e=readdir(d)) {
        if(++count>20000){clear=false;break;}
        std::string name=e->d_name;
        if(name.size()>=8&&name.substr(name.size()-8)==".pending"){
            clear=false;LOG_ERROR("fish pickup: unresolved recovery journal=%s",name.c_str());
        }
    }
    closedir(d);return clear;
}
bool fish_pickup_suppressed(const creature_motion::Message& identity) {
    const auto& s=g_fish_pickup;
    if(!s.context||identity.session!=s.session||identity.epoch!=s.epoch)return false;
    if(s.removed.count(identity.fish))return true;
    fp::Identity pending;
    return s.pending.used&&(s.pending.loading||s.pending.applied)&&
        fp::identity(s.pending.request.locker,&pending)&&pending.fish==identity.fish;
}
bool fish_pickup_pose_allowed(const creature_motion::Message& pose) {
    if(fish_pickup_suppressed(pose))return false;
    auto it=g_fish_pickup.pose_floors.find(pose.fish);
    return it==g_fish_pickup.pose_floors.end()||fp::pose_after_floor(pose.sequence,it->second);
}
uint64_t fish_pickup_revision(const fp::Id& fish) {
    auto it=g_fish_pickup.revisions.find(fish);return it==g_fish_pickup.revisions.end()?1:it->second;
}
fish_motion::Message fish_pickup_pose(const fp::Identity& id, int tech) {
    fish_motion::Message m;m.kind=fish_motion::LivePose;m.host=m.sender=g_fish_pickup.host;
    m.species=fish_motion::species_for_tech(tech);
    m.epoch=id.epoch;m.session=id.session;m.fish=id.fish;m.prefab=id.prefab;m.nonce=1;m.sequence=1;return m;
}
bool fish_pickup_live() {
    auto& s=g_fish_pickup;
    fp::Id session{};uint32_t epoch=0;
    return s.context&&!s.failed&&g_boomerang.runtime_ready&&!g_reid_wanted&&contents_world_ready()&&
        net_geometry_session(session.data(),&epoch)&&s.session==session&&s.epoch==epoch&&s.host==net_session_host_id();
}
bool fish_pickup_send(lc::Message m) {
    auto& s=g_fish_pickup;if(s.outgoing.size()>=32||!fp::valid(m))return false;
    ContentsOutgoing out;if(!lc::encode(m,&out.blob))return false;
    out.id=++s.message;s.outgoing.push_back(std::move(out));return true;
}
void fish_pickup_receive_frame(const WorldEvent& event) {
    auto& s=g_fish_pickup;
    constexpr size_t offset=offsetof(WorldEvent,guid),capacity=sizeof(WorldEvent)-offset;
    if(!event.origin_id||event.reserved16<lc::kFrameHeader||event.reserved16>capacity||s.incoming.size()>=32)return;
    if(!s.assembly.count(event.origin_id)&&s.assembly.size()>=kMaxPeers)return;
    std::vector<uint8_t> bytes;
    if(!s.assembly[event.origin_id].append(reinterpret_cast<const uint8_t*>(&event)+offset,event.reserved16,capacity,&bytes)||bytes.empty())return;
    lc::Message m;if(lc::decode(bytes,&m)&&fp::valid(m))s.incoming.emplace_back(event.origin_id,std::move(m));
    else LOG_WARN("fish pickup: refused malformed transaction peer=%08x",event.origin_id);
}
bool fish_pickup_serialize(void* pickup,std::vector<uint8_t>* bytes) {
    auto& a=g_contents_api;
    void* go=g_get_game_object.call(pickup,g_get_game_object.method);
    ManagedRoot go_root,serializer_root,stream_root;
    if(!go||!prefab_retain(go_root,go))return false;
    void* serializer=il2cpp_object_new(a.serializer);
    if(!serializer||!prefab_retain(serializer_root,serializer))return false;
    void* stream=il2cpp_object_new(a.stream);
    if(!stream||!prefab_retain(stream_root,stream))return false;
    a.serializer_ctor.call(serializer,a.serializer_ctor.method);a.stream_ctor.call(stream,a.stream_ctor.method);
    a.serialize.call(serializer,stream,go,a.serialize.method);
    void* data=a.stream_bytes.call(stream,a.stream_bytes.method);
    int n=data?g_array_get_length.call(data,g_array_get_length.method):0;
    if(n<=0||n>int(lc::kMaxItemBytes))return false;
    bytes->assign(static_cast<uint8_t*>(data)+0x20,static_cast<uint8_t*>(data)+0x20+n);return true;
}
// Stop managing an original only while its GameObject is inactive. Restoring
// saved Behaviour flags must not run its AI between deserialization and pickup.
void fish_pickup_park(const fp::Identity& id) {
    for(auto& r:g_boomerang.replicas)if(r.identity.fish==id.fish&&r.identity.prefab==id.prefab) {
        if(r.visual){release_boomerang(r);continue;}
        if(r.handle&&probe_exists(r.fish)) {
            void* go=g_get_game_object.call(r.fish,g_get_game_object.method);
            if(probe_exists(go)) {
                void* pickup=g_get_component.call(go,g_pickupable_type,g_get_component.method);
                if(probe_exists(pickup))contents_suppress(pickup);
                g_set_active.call(go,false,g_set_active.method);
            }
            release_boomerang(r);
        }
    }
}
bool fish_pickup_inventory_item(const lc::Message& m,ContentsNativeItem* item) {
    return contents_item(contents_inventory(),m.item.id,item)&&item->state.tech==m.item.tech;
}
void fish_pickup_cancel() {
    auto& s=g_fish_pickup;auto m=s.pending.request;m.kind=lc::Cancel;m.item.data.clear();
    s.pending.cancelling=true;fish_pickup_send(m);s.next_retry=build_now()+2;
}
bool fish_pickup_begin_load(const lc::Message& m) {
    auto& s=g_fish_pickup;auto& a=g_contents_api;auto& load=g_fish_pickup_load;
    fp::Identity id;if(!fp::identity(m.locker,&id)||!s.pending.may_restore(m))return false;
    void* container=contents_inventory();
    if(!container||!a.room_tech.call(container,m.item.tech,a.room_tech.method)) {
        fish_pickup_notice("SNMP: в инвентаре нет места для бумеранга.");fish_pickup_cancel();return false;
    }
    ContentsNativeItem owned;
    if(fish_pickup_inventory_item(m,&owned)){
        s.failed=true;fish_pickup_notice("SNMP: такой ID уже есть в инвентаре. Сохрани диагностику.");return false;
    }
    if(!fish_pickup_journal(m,net_local_id())){fish_pickup_notice("SNMP: не удалось записать журнал подбора.");fish_pickup_cancel();return false;}
    s.granted=m;s.pending.loading=true;
    fish_pickup_park(id);
    uint8_t hashed[16]{};guid_from_string(m.item.id.c_str(),hashed);
    void* existing=find_pickupable_by_guid(hashed),*existing_uid=nullptr;
    if(existing) {
        void* owner=contents_field(contents_field(existing,a.pickup_inventory_item),a.item_owner);
        if(owner){s.failed=true;fish_pickup_notice("SNMP: конфликт владельца рыбы. Подбор удерживается.");return false;}
        void* go=g_get_game_object.call(existing,g_get_game_object.method);
        g_set_active.call(go,false,g_set_active.method);existing_uid=build_uid(existing);
        if(!existing_uid){s.failed=true;return false;}
    }
    void* roots=il2cpp_array_new(find_class_in_domain("System","Object"),5);
    if(!roots||!prefab_retain(load.roots,roots)){s.failed=true;return false;}
    void* bytes=il2cpp_array_new(a.byte,unsigned(m.item.data.size()));if(!bytes){s.failed=true;return false;}
    a.array_set.call(roots,bytes,2,a.array_set.method);a.array_set.call(roots,existing_uid,4,a.array_set.method);
    void* serializer=il2cpp_object_new(a.serializer);if(!serializer){s.failed=true;return false;}
    a.array_set.call(roots,serializer,0,a.array_set.method);
    void* stream=il2cpp_object_new(a.stream);if(!stream){s.failed=true;return false;}
    a.array_set.call(roots,stream,1,a.array_set.method);
    memcpy(static_cast<uint8_t*>(bytes)+0x20,m.item.data.data(),m.item.data.size());
    a.serializer_ctor.call(serializer,a.serializer_ctor.method);a.stream_input.call(stream,bytes,a.stream_input.method);
    // The existing serializer uses forceInactiveRoot=true. No new fish AI is
    // activated while its native item state is being restored.
    void* task=a.deserialize.call(serializer,stream,existing_uid,1,0,nullptr,0,0,a.deserialize.method);
    if(!task){s.failed=true;return false;}
    a.array_set.call(roots,task,3,a.array_set.method);
    load.task=task;
    load.result_field=fish_pickup_field(il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(task)),"result");
    void* result=contents_field(task,load.result_field);
    if(result)load.value_field=fish_pickup_field(il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(result)),"value");
    if(!load.result_field||!load.value_field){s.failed=true;return false;}
    load.started=build_now();a.coroutine.call(task,a.coroutine.method);
    LOG_INFO("fish pickup: restore started item=%s request=%llu bytes=%zu",m.item.id.c_str(),(unsigned long long)m.request,m.item.data.size());return true;
}
void fish_pickup_poll_load() {
    auto& s=g_fish_pickup;auto& load=g_fish_pickup_load;auto& a=g_contents_api;
    if((!s.pending.loading&&!s.pending.applied)||!load.task||s.pending.committed)return;
    void* go=contents_field(contents_field(load.task,load.result_field),load.value_field);
    if(!probe_exists(go)) {
        if(build_now()-load.started>30){s.failed=true;fish_pickup_notice("SNMP: восстановление рыбы задержалось. Сохрани диагностику.");}
        return;
    }
    const auto& m=s.granted;fp::Identity id;fp::identity(m.locker,&id);
    void* pickup=g_get_component.call(go,g_pickupable_type,g_get_component.method);std::string actual;
    fish_motion::Message exact;
    if(!pickup||!contents_id(pickup,&actual)||actual!=m.item.id||
        int(g_pickup_get_tech.call(pickup,g_pickup_get_tech.method))!=m.item.tech||
        !boomerang_identity(build_uid(pickup),exact)||exact.fish!=id.fish||exact.prefab!=id.prefab||
        g_boomerang.active.call(go,g_boomerang.active.method)) {
        s.failed=true;fish_pickup_notice("SNMP: восстановленная рыба не прошла проверку ID/типа/неактивности.");return;
    }
    void* container=contents_inventory();if(!container)return;
    if(!a.room.call(container,pickup,a.room.method)) {
        if(build_now()>=load.next_notice){load.next_notice=build_now()+5;fish_pickup_notice("SNMP: освободи место в инвентаре — бумеранг зарезервирован.");}return;
    }
    if(!s.pending.applied) {
        s.pending.loading=false;s.pending.applied=true;
        auto applied=s.pending.request;applied.kind=lc::Applied;applied.item.id=m.item.id;applied.item.data.clear();
        auto journal=m;journal.kind=lc::Applied;
        if(!fish_pickup_journal(journal,net_local_id())){s.failed=true;fish_pickup_notice("SNMP: ошибка журнала подготовки рыбы; сохрани диагностику.");return;}
        fish_pickup_send(applied);s.next_retry=build_now()+2;
        LOG_INFO("fish pickup: guest prepared inactive item=%s request=%llu",m.item.id.c_str(),(unsigned long long)m.request);
    }
    if(!s.got_done)return;
    contents_suppress(pickup);void* added=nullptr;
    {ContentsScope scope;++g_applying_remote;
        g_fish_pickup_api.item_pickup.call(pickup,false,g_fish_pickup_api.item_pickup.method);
        added=a.add_pickup.call(container,pickup,a.add_pickup.method);
        --g_applying_remote;
    }
    ContentsNativeItem item;
    if(!added||!fish_pickup_inventory_item(m,&item)||item.pickup!=pickup){s.failed=true;fish_pickup_notice("SNMP: игра не подтвердила помещение рыбы в инвентарь.");return;}
    g_set_active.call(go,false,g_set_active.method);
    s.pending.committed=true;
    auto journal=m;journal.kind=lc::Done;
    if(!fish_pickup_journal(journal,net_local_id(),true)){s.failed=true;fish_pickup_notice("SNMP: ошибка журнала после подбора; сохрани диагностику.");return;}
    if(g_fish_pickup_api.sound)g_fish_pickup_api.sound.call(pickup,g_fish_pickup_api.sound.method);
    LOG_INFO("fish pickup: guest inventory applied item=%s request=%llu exactly_one=1",m.item.id.c_str(),(unsigned long long)m.request);
    s.pending={};s.granted={};s.got_done=false;load.roots.clear();
    load.task=load.result_field=load.value_field=nullptr;load.started=load.next_notice=0;
}
void fish_pickup_done_local(const lc::Message& done) {
    auto& s=g_fish_pickup;fp::Identity id;if(!fp::identity(done.locker,&id))return;
    if(done.revision<=fish_pickup_revision(id.fish))return;
    s.revisions[id.fish]=done.revision;
    s.removed[id.fish]=done.revision;
    fish_pickup_park(id);
}
void fish_pickup_handle(uint32_t origin,const lc::Message& incoming) {
    auto& s=g_fish_pickup;fp::Identity id;
    if(!fp::valid(incoming)||!fp::identity(incoming.locker,&id)||!fp::matches(id,s.session,s.epoch))return;
    if(incoming.recipient&&incoming.recipient!=net_local_id())return;
    if(g_local_is_host) {
        if(origin==s.host)return;
        auto& q=incoming;
        if(q.kind==lc::Request) {
            if(!s.hosts.count(q.locker)&&s.hosts.size()>=fp::kMaxRecords)return;
            auto& record=s.hosts[q.locker];auto& auth=record.authority;
            if(!record.initialized)auth.revision=fish_pickup_revision(id.fish);
            auto cached=auth.replies.find({origin,q.request});
            if(cached!=auth.replies.end()){fish_pickup_send(cached->second);return;}
            if(auth.reserved&&auth.pending.key==lc::Key{origin,q.request}){fish_pickup_send(auth.reply(auth.pending.request,lc::Grant,origin));return;}
            auto reject=q;reject.kind=lc::Reject;reject.recipient=origin;reject.revision=auth.revision;
            if(s.removed.count(id.fish)||auth.reserved||auth.replies.size()>=64){fish_pickup_send(reject);return;}
            void* fish=find_boomerang(fish_pickup_pose(id,q.item.tech));if(!fish){fish_pickup_send(reject);return;}
            void* tr=g_get_transform.call(fish,g_get_transform.method);
            const auto pos=g_get_position.call(tr,g_get_position.method);
            bool reach=false;PeerState peers[kMaxPeers]{};size_t count=net_copy_world_peers(peers,kMaxPeers);
            for(size_t i=0;i<count;++i)if(peers[i].id==origin)
                reach=fp::actor_in_reach(boomerang_distance_sq(pos,peers[i]),build_now()-peers[i].last_seen);
            if(!reach){fish_pickup_send(reject);return;}
            void* go=g_get_game_object.call(fish,g_get_game_object.method);
            void* pickup=g_get_component.call(go,g_pickupable_type,g_get_component.method);
            lc::Item item;item.tech=q.item.tech;
            if(!probe_exists(pickup)||int(g_pickup_get_tech.call(pickup,g_pickup_get_tech.method))!=item.tech||
                !contents_id(pickup,&item.id)||!fp::item_matches(item.id,id.fish)||
                !fish_pickup_serialize(pickup,&item.data)){fish_pickup_send(reject);return;}
            if(!record.initialized){if(!auth.initialize({item}))return;record.initialized=true;}
            auto canonical=q;canonical.item=std::move(item);
            auto reply=auth.begin(origin,canonical);
            if(reply.kind==lc::Grant&&!fish_pickup_journal(reply,origin))reply=auth.finish(origin,canonical,false);
            fish_pickup_send(reply);
            LOG_INFO("fish pickup: host %s item=%s peer=%08x request=%llu",reply.kind==lc::Grant?"reserved":"rejected",canonical.item.id.c_str(),origin,(unsigned long long)q.request);
        } else if(q.kind==lc::Applied||q.kind==lc::Cancel) {
            auto found=s.hosts.find(q.locker);if(found==s.hosts.end())return;
            auto& auth=found->second.authority;auto old=auth.replies.find({origin,q.request});
            if(old!=auth.replies.end()){fish_pickup_send(old->second);return;}
            if(!auth.reserved||!(auth.pending.key==lc::Key{origin,q.request})||
                auth.pending.request.item.tech!=q.item.tech)return;
            if(q.kind==lc::Cancel){auto rejected=auth.finish(origin,q,false);fish_pickup_journal(rejected,origin,true);fish_pickup_send(rejected);return;}
            const auto grant=auth.pending.request;
            uint8_t hashed[16]{};guid_from_string(grant.item.id.c_str(),hashed);
            void* pickup=find_pickupable_by_guid(hashed);
            if(pickup) {
                void* go=g_get_game_object.call(pickup,g_get_game_object.method);
                if(object_in_local_inventory(go)){s.failed=true;fish_pickup_notice("SNMP: конфликт подбора рыбы на хосте. Сохрани диагностику.");return;}
                contents_suppress(pickup);g_set_active.call(go,false,g_set_active.method);
                // Retain the dormant exact object for the existing item-drop
                // channel, with Pickupable's normal disabled-state bookkeeping.
                g_fish_pickup_api.item_pickup.call(pickup,false,g_fish_pickup_api.item_pickup.method);
                g_set_active.call(go,false,g_set_active.method);
            }
            auto done=auth.finish(origin,q,true);
            if(!fish_pickup_journal(done,origin,true)){s.failed=true;fish_pickup_notice("SNMP: не удалось завершить журнал подбора на хосте.");return;}
            fish_pickup_done_local(done);done.recipient=0;done.item.data.clear();fish_pickup_send(done);
            LOG_INFO("fish pickup: host committed item=%s peer=%08x request=%llu",grant.item.id.c_str(),origin,(unsigned long long)q.request);
        }
        return;
    }
    if(origin!=s.host)return;
    if(incoming.kind==lc::Snapshot) {
        if(incoming.revision<fish_pickup_revision(id.fish)||incoming.revision<=s.reset_revisions[id.fish])return;
        if(s.pending.used) {
            fp::Identity pending;
            if(fp::identity(s.pending.request.locker,&pending)&&pending.fish==id.fish&&
                (s.pending.request.revision!=incoming.revision||s.pending.loading||s.pending.applied)){
                s.failed=true;fish_pickup_notice("SNMP: рыба сменила состояние во время подбора; сохрани диагностику.");return;
            }
        }
        s.reset_revisions[id.fish]=incoming.revision;s.revisions[id.fish]=incoming.revision;
        s.pose_floors[id.fish]=uint32_t(incoming.request);s.removed.erase(id.fish);
        for(auto it=g_boomerang.replicas.begin();it!=g_boomerang.replicas.end();) {
            if(it->identity.fish==id.fish){release_boomerang(*it);it=g_boomerang.replicas.erase(it);}else ++it;
        }
        g_boomerang.next_scan=0;
        LOG_INFO("fish pickup: host drop revision=%llu floor=%u item=%s",(unsigned long long)incoming.revision,uint32_t(incoming.request),incoming.item.id.c_str());return;
    }
    if(incoming.kind==lc::Grant&&s.pending.owns(incoming)) {
        if(s.pending.applied){auto m=s.pending.request;m.kind=lc::Applied;m.item.id=incoming.item.id;fish_pickup_send(m);return;}
        if(s.pending.cancelling){fish_pickup_cancel();return;}
        if(s.pending.may_restore(incoming))fish_pickup_begin_load(incoming);
    } else if(incoming.kind==lc::Reject&&s.pending.owns(incoming)) {
        if(s.pending.applied||s.pending.loading){s.failed=true;fish_pickup_notice("SNMP: конфликт подтверждения подбора. Сохрани диагностику.");return;}
        fish_pickup_journal(incoming,net_local_id(),true);s.pending={};s.granted={};
        s.revisions[id.fish]=std::max(fish_pickup_revision(id.fish),incoming.revision);
        fish_pickup_notice("SNMP: рыбу уже забрали или она вне досягаемости.");
    } else if(incoming.kind==lc::Done) {
        if(incoming.revision<=fish_pickup_revision(id.fish))return;
        if(s.pending.owns(incoming)) {
            if(!s.pending.applied){s.failed=true;fish_pickup_notice("SNMP: подтверждение рыбы пришло до помещения в инвентарь.");return;}
            s.got_done=true;
            LOG_INFO("fish pickup: guest host commit received item=%s request=%llu",incoming.item.id.c_str(),(unsigned long long)incoming.request);
        }
        fish_pickup_done_local(incoming);
    }
}

// The item channel still owns normal Drop(). Release a completed pickup's
// tombstone only on that exact item, and publish a new lifecycle revision.
void fish_pickup_on_drop(void* pickup) {
    auto& s=g_fish_pickup;
    if(!s.context||s.failed||s.removed.empty()||!probe_exists(pickup))return;
    const int tech=int(g_pickup_get_tech.call(pickup,g_pickup_get_tech.method));
    if(!fish_motion::supported_tech(tech))return;
    void* go=g_get_game_object.call(pickup,g_get_game_object.method);
    fish_motion::Message exact;
    if(!probe_exists(go)||object_in_local_inventory(go)||!boomerang_identity(build_uid(pickup),exact))return;
    auto removed=s.removed.find(exact.fish);if(removed==s.removed.end())return;
    if(removed->second==UINT64_MAX){s.failed=true;return;}
    const auto revision=removed->second+1;s.removed.erase(removed);s.revisions[exact.fish]=revision;
    exact.species=fish_motion::species_for_tech(tech);
    exact.session=s.session;exact.epoch=s.epoch;exact.host=exact.sender=s.host;exact.kind=fish_motion::LivePose;exact.nonce=exact.sequence=1;
    fp::Identity id{s.session,exact.fish,exact.prefab,s.epoch};const auto key=fp::key(id);
    s.hosts.erase(key);
    for(auto it=g_boomerang.replicas.begin();it!=g_boomerang.replicas.end();) {
        if(it->identity.fish==id.fish){release_boomerang(*it);it=g_boomerang.replicas.erase(it);}else ++it;
    }
    g_boomerang.next_scan=0;
    if(!g_local_is_host) {
        if(auto* replica=boomerang_record(exact,build_now()))bind_boomerang(*replica);
    } else {
        s.reset_revisions[id.fish]=revision;
        lc::Message reset;reset.kind=lc::Snapshot;reset.locker=key;reset.revision=revision;
        reset.request=g_boomerang.sequence==UINT32_MAX?1:g_boomerang.sequence+1;
        reset.item.tech=tech;if(!contents_id(pickup,&reset.item.id))return;
        fish_pickup_send(reset);
    }
    LOG_INFO("fish pickup: exact item dropped revision=%llu item=%s",(unsigned long long)revision,fp::hex(id.fish).c_str());
}

#include "fish_pickup_ui.inc"

void tick_fish_pickup(const LocalState& local) try {
    auto& s=g_fish_pickup;
    if(!local.valid||!g_role_known||!g_boomerang.runtime_ready||!g_contents_api.ready)return;
    fp::Id session{};uint32_t epoch=0;if(!net_geometry_session(session.data(),&epoch))return;
    const auto host=net_session_host_id();
    if(!s.context||s.session!=session||s.epoch!=epoch||s.host!=host||s.generation!=g_world_context_generation) {
        bool reserved=false;for(const auto& entry:s.hosts)reserved|=entry.second.authority.reserved;
        if(s.context&&(s.pending.used||reserved)){s.failed=true;fish_pickup_notice("SNMP: сессия сменилась во время подбора. Сохрани диагностику.");return;}
        s.context=true;s.session=session;s.epoch=epoch;s.host=host;s.generation=g_world_context_generation;
        s.hosts.clear();s.removed.clear();s.revisions.clear();s.reset_revisions.clear();s.pose_floors.clear();s.assembly.clear();s.incoming.clear();s.outgoing.clear();
    }
    fish_pickup_install();
    if(s.failed)return;
    if(!s.journals_checked){s.journals_checked=true;if(!fish_pickup_check_journals()){s.failed=true;fish_pickup_notice("SNMP: есть незавершённый подбор прошлого запуска. Сохрани диагностику.");return;}}
    if(!g_fish_pickup_api.hooks||!fish_pickup_live())return;
    fish_pickup_poll_load();
    for(int i=0;i<4&&!s.failed&&!s.incoming.empty();++i){auto m=std::move(s.incoming.front());s.incoming.pop_front();fish_pickup_handle(m.first,m.second);}
    if(s.failed)return;
    if(s.pending.used&&build_now()>=s.next_retry&&s.outgoing.size()<8) {
        auto m=s.pending.request;m.item.data.clear();
        if(s.pending.applied)m.kind=lc::Applied;else if(s.pending.cancelling)m.kind=lc::Cancel;
        // Loading waits for its native result; it must not request a second restore.
        if(!s.pending.loading)fish_pickup_send(m);
        s.next_retry=build_now()+2;
    }
    constexpr size_t offset=offsetof(WorldEvent,guid),capacity=sizeof(WorldEvent)-offset;
    for(int i=0;i<4&&!s.outgoing.empty()&&net_pending_world_event_count()<24;++i) {
        auto& out=s.outgoing.front();std::vector<uint8_t> frame;
        if(!lc::fragment(out.blob,out.id,out.part,capacity,&frame)){s.outgoing.pop_front();continue;}
        WorldEvent e{};e.kind=kWorldEventFishPickup;e.reserved16=uint16_t(frame.size());
        memcpy(reinterpret_cast<uint8_t*>(&e)+offset,frame.data(),frame.size());
        if(!net_try_post_world_event(e))break;
        if(++out.part*(capacity-lc::kFrameHeader)>=out.blob.size())s.outgoing.pop_front();
    }
} catch(...){g_fish_pickup.failed=true;fish_pickup_notice("SNMP: ошибка подбора рыбы. Повторы остановлены; сохрани диагностику.");}
