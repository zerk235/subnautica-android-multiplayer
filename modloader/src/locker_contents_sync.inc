// Included after build_sync.inc, on the Unity game thread only.
namespace lc=locker_contents;
using ContentsAddFn=uint8_t (*)(void*,void*,void*);
using ContentsRemoveFn=uint8_t (*)(void*,void*,uint8_t,uint8_t,void*);
using ContentsAllowFn=uint8_t (*)(void*,void*,uint8_t,void*);
struct ContentsApi {
    bool ready=false,hooks=false,failed=false;
    Il2CppClass *items=nullptr,*serializer=nullptr,*stream=nullptr,*byte=nullptr;
    void *item_pickup=nullptr,*item_owner=nullptr,*pickup_inventory_item=nullptr,*obstacle_storage=nullptr;
    Entry<BuildObjectFn> inventory,enumerate,stream_bytes;
    Entry<ManagedFn> serializer_ctor,stream_ctor;
    Entry<void (*)(void*,void*,void*)> stream_input;
    Entry<void (*)(void*,void*,void*,void*)> serialize;
    Entry<void* (*)(void*,void*,void*,uint8_t,uint8_t,void*,uint8_t,int32_t,void*)> deserialize;
    Entry<void* (*)(void*,void*)> coroutine;
    Entry<ContentsAddFn> add;
    Entry<ContentsRemoveFn> remove;
    Entry<ContentsAllowFn> allow_remove;
    Entry<void* (*)(void*,void*,void*)> add_pickup;
    Entry<uint8_t (*)(void*,int32_t,void*)> room_tech;
    Entry<uint8_t (*)(void*,void*,void*)> room;
    Entry<void (*)(void*,void*,int32_t,void*)> array_set;
    Entry<void (*)(void*,void*,void*)> click;
    Entry<void (*)(void*,void*)> notify;
    Entry<uint8_t (*)(void*,void**,void*)> deconstruct;
    void* roots=nullptr;
} g_contents_api;
ContentsAddFn g_contents_add_original=nullptr;
ContentsRemoveFn g_contents_remove_original=nullptr;
ContentsAllowFn g_contents_allowed_remove_original=nullptr;
using ContentsClickFn=void (*)(void*,void*,void*);
ContentsClickFn g_contents_click_original=nullptr;
using ContentsDeconstructFn=uint8_t (*)(void*,void**,void*);
ContentsDeconstructFn g_contents_deconstruct_original=nullptr;
bool g_contents_internal=false;
struct ContentsLocal {
    lc::Authority authority;
    bool initialized=false,ready=false,repairing=false,want_open=false;
    uint64_t revision=0;
    void* hand=nullptr;
    std::vector<lc::Item> manifest;
    double next_query=0;
};
std::map<std::string,ContentsLocal> g_contents_lockers;
struct ContentsPending {bool used=false,applied=false;lc::Message request;double next_send=0;} g_contents_pending;
struct ContentsOutgoing {uint64_t id=0;size_t part=0;std::vector<uint8_t> blob;};
std::deque<ContentsOutgoing> g_contents_outgoing;
std::deque<std::pair<uint32_t,lc::Message>> g_contents_incoming;
std::map<uint32_t,lc::Assembly> g_contents_assembly;
uint64_t g_contents_message=0,g_contents_request=0;
uint8_t g_contents_session[16]{};uint32_t g_contents_epoch=0;
bool g_contents_context=false;
bool g_contents_journal_checked=false;
struct ContentsLoad {
    bool active=false;lc::Message message;uint32_t origin=0;
    void* task=nullptr;void* result_field=nullptr;void* value_field=nullptr;
    double started=0;
} g_contents_load;
bool contents_locker_busy(const char* id) {
    if(!id||!g_contents_context)return false;
    const auto local=g_contents_lockers.find(id);
    return g_contents_api.failed||
        (g_contents_pending.used&&g_contents_pending.request.locker==id)||
        (g_contents_load.active&&g_contents_load.message.locker==id)||
        (local!=g_contents_lockers.end()&&(local->second.authority.reserved||local->second.repairing));
}

void contents_notify(const char* text) {
    LOG_INFO("locker contents: %s",text);
    if(g_contents_api.notify){void* s=il2cpp_string_new(text);g_contents_api.notify.call(s,g_contents_api.notify.method);}
}
constexpr const char* kContentsJournal="/sdcard/Android/data/com.unknownworlds.subnautica/files/mod_log/locker_contents";
std::string contents_journal_path(const lc::Message& m,uint32_t peer) {
    // A journal is keyed by an opaque locker hash and peer, never by a path
    // supplied in a packet. It contains serialized recovery bytes, not just IDs.
    uint8_t guid[16]{};guid_from_string(m.locker.c_str(),guid);char name[96]{};
    snprintf(name,sizeof(name),"/%08x-%08x%08x.journal",peer,lc::get32(guid),lc::get32(guid+4));
    return std::string(kContentsJournal)+name;
}
bool contents_journal(const lc::Message& m,uint32_t peer) {
    if(mkdir(kContentsJournal,0770)!=0&&errno!=EEXIST)return false;
    std::vector<uint8_t> bytes;if(!lc::encode(m,&bytes))return false;
    const auto path=contents_journal_path(m,peer),temp=path+".tmp";
    FILE* file=fopen(temp.c_str(),"wb");if(!file)return false;
    bool ok=fwrite(bytes.data(),1,bytes.size(),file)==bytes.size();
    if(fflush(file)!=0||fsync(fileno(file))!=0)ok=false;
    if(fclose(file)!=0)ok=false;
    if(ok&&rename(temp.c_str(),path.c_str())==0)return true;
    unlink(temp.c_str());LOG_WARN("locker contents: journal write failed item=%s",m.item.id.c_str());return false;
}
void contents_journal_done(lc::Message m,uint32_t peer) {
    m.kind=lc::Done;
    if(contents_journal(m,peer))unlink(contents_journal_path(m,peer).c_str());
}
bool contents_check_journals() {
    DIR* dir=opendir(kContentsJournal);if(!dir)return errno==ENOENT;
    bool clear=true;
    while(auto* entry=readdir(dir)) {
        std::string name=entry->d_name;if(name.size()<8||name.substr(name.size()-8)!=".journal")continue;
        std::string path=std::string(kContentsJournal)+"/"+name;FILE* file=fopen(path.c_str(),"rb");
        if(!file){clear=false;continue;}
        std::vector<uint8_t> bytes(lc::kMaxMessage+1);size_t n=fread(bytes.data(),1,bytes.size(),file);fclose(file);bytes.resize(n);
        lc::Message saved;
        if(!lc::decode(bytes,&saved)||saved.kind!=lc::Done){
            clear=false;LOG_ERROR("locker contents: recovery journal present file=%s; retaining bytes and refusing automatic replay",name.c_str());
        }else unlink(path.c_str());
    }
    closedir(dir);return clear;
}
bool contents_world_ready() {
    if(!g_contents_context||!g_build_baseline||!g_role_known||g_world_live_since<=0||
        build_now()-g_world_live_since<kReidSettleSeconds)return false;
    void* manager=g_slm_get_main?g_slm_get_main.call(nullptr,g_slm_get_main.method):nullptr;
    return manager&&g_slm_is_loading&&!g_slm_is_loading.call(manager,g_slm_is_loading.method);
}
struct ContentsScope {bool prior=g_contents_internal;ContentsScope(){g_contents_internal=true;}~ContentsScope(){g_contents_internal=prior;}};
void* contents_field(void* object,void* field) {
    void* out=nullptr;if(object&&field)il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(object),field,&out);return out;
}
bool contents_id(void* component,std::string* out) {
    void* uid=build_uid(component);char id[128]{};
    if(!uid||!string_to_ascii(g_unique_get_id.call(uid,g_unique_get_id.method),id,sizeof(id))||!lc::id_valid(id))return false;
    *out=id;return true;
}
void* contents_locker(const std::string& id,void** constructable=nullptr) {
    WorldEvent e{};snprintf(e.object_id,sizeof(e.object_id),"%s",id.c_str());
    void* c=build_find(e);WorldEvent check{};
    if(!c||!build_locker_capture(c,&check))return nullptr;
    bool complete=false;il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(c),g_build_complete_field,&complete);
    if(!complete)return nullptr;
    if(constructable)*constructable=c;
    return build_locker_storage(g_get_game_object.call(c,g_get_game_object.method),true);
}
void* contents_container(const std::string& id) {
    return contents_field(contents_locker(id),g_locker_api.container_field);
}
void* contents_inventory() {
    void* inv=read_static_object_field(g_inventory_class,"main");
    return inv?g_contents_api.inventory.call(inv,g_contents_api.inventory.method):nullptr;
}
bool contents_parent(void* container,std::string* id) {
    if(!container||il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(container))!=g_contents_api.items)return false;
    // Resolve the Transform property through its getter rather than treating an
    // ItemsContainer as a Unity Component.
    Entry<BuildObjectFn> tr;tr.bind(find_method(g_contents_api.items,"get_tr",0));
    void* transform=tr?tr.call(container,tr.method):nullptr;
    void* go=transform?g_get_game_object.call(transform,g_get_game_object.method):nullptr;
    bool found=false;
    walk_parents(go,[&](void* node){
        void* c=g_get_component.call(node,g_build_type,g_get_component.method);
        WorldEvent check{};
        if(!c||!build_locker_capture(c,&check))return false;
        void* storage=build_locker_storage(node,true);
        if(contents_field(storage,g_locker_api.container_field)!=container)return false;
        *id=check.object_id;found=true;return true;
    });return found;
}
struct ContentsNativeItem {lc::Item state;void* inventory_item=nullptr;void* pickup=nullptr;};
bool contents_items(void* container,std::vector<ContentsNativeItem>* out) {
    out->clear();if(!container)return false;
    void* it=g_contents_api.enumerate.call(container,g_contents_api.enumerate.method);if(!it)return false;
    auto* klass=il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(it));
    Entry<MoveNextFn> next;Entry<BuildObjectFn> current;
    next.bind(find_method(klass,"MoveNext",0));
    current.bind(find_method(klass,"System.Collections.Generic.IEnumerator<InventoryItem>.get_Current",0));
    if(!current)current.bind(find_method(klass,"System.Collections.IEnumerator.get_Current",0));
    if(!next||!current)return false;
    while(next.call(it,next.method)) {
        if(out->size()>=lc::kMaxItems)return false;
        ContentsNativeItem i;i.inventory_item=current.call(it,current.method);
        i.pickup=contents_field(i.inventory_item,g_contents_api.item_pickup);
        if(!build_live(i.pickup)||!contents_id(i.pickup,&i.state.id))return false;
        i.state.tech=int32_t(g_pickup_get_tech.call(i.pickup,g_pickup_get_tech.method));
        if(i.state.tech<=0)return false;out->push_back(std::move(i));
    }return true;
}
bool contents_item(void* container,const std::string& id,ContentsNativeItem* out) {
    std::vector<ContentsNativeItem> all;if(!contents_items(container,&all))return false;
    for(auto& i:all)if(i.state.id==id){*out=std::move(i);return true;}return false;
}
bool contents_serialize(void* pickup,std::vector<uint8_t>* out) try {
    auto& a=g_contents_api;
    void* go=g_get_game_object.call(pickup,g_get_game_object.method);
    void* serializer=il2cpp_object_new(a.serializer);void* stream=il2cpp_object_new(a.stream);
    if(!go||!serializer||!stream)return false;
    a.serializer_ctor.call(serializer,a.serializer_ctor.method);a.stream_ctor.call(stream,a.stream_ctor.method);
    a.serialize.call(serializer,stream,go,a.serialize.method);
    void* bytes=a.stream_bytes.call(stream,a.stream_bytes.method);
    int n=bytes?g_array_get_length.call(bytes,g_array_get_length.method):0;
    if(n<=0||n>int(lc::kMaxItemBytes))return false;
    out->assign(static_cast<uint8_t*>(bytes)+0x20,static_cast<uint8_t*>(bytes)+0x20+n);return true;
} catch(...){LOG_WARN("locker contents: native item serialization failed");return false;}
bool contents_send(lc::Message m) {
    if(g_contents_outgoing.size()>=64)return false;
    ContentsOutgoing out;if(!lc::encode(m,&out.blob))return false;
    out.id=++g_contents_message;g_contents_outgoing.push_back(std::move(out));return true;
}
void contents_query(const std::string& id) {
    auto& local=g_contents_lockers[id];if(build_now()<local.next_query)return;
    lc::Message m;m.kind=lc::Query;m.locker=id;m.recipient=net_session_host_id();
    if(contents_send(m))local.next_query=build_now()+2;
}
bool contents_baseline(const std::string& id) {
    auto& local=g_contents_lockers[id];if(local.initialized)return true;
    std::vector<ContentsNativeItem> actual;if(!contents_items(contents_container(id),&actual))return false;
    std::vector<lc::Item> states;for(auto& i:actual)states.push_back(i.state);
    if(!local.authority.initialize(states))return false;
    local.initialized=local.ready=true;local.revision=1;
    LOG_INFO("locker contents: baseline id=%s items=%zu revision=1",id.c_str(),states.size());return true;
}
void contents_snapshot(const std::string& id,uint32_t peer) {
    if(!contents_baseline(id))return;
    auto& a=g_contents_lockers[id].authority;
    // A prepared transfer is deliberately invisible until its final native commit.
    if(a.reserved)return;
    lc::Message m;m.kind=lc::Snapshot;m.locker=id;m.recipient=peer;m.revision=a.revision;
    std::vector<ContentsNativeItem> actual;
    if(!contents_items(contents_container(id),&actual))return;
    for(auto& i:actual)m.items.push_back(std::move(i.state));
    contents_send(std::move(m));
}
void contents_suppress(void* pickup) {uint8_t guid[16]{};if(pickup_guid(pickup,guid))suppress_item(guid);}
bool contents_move(void* destination,void* item) {
    ContentsScope scope;
    uint8_t ok=g_contents_add_original(destination,item,g_contents_api.add.method);
    return ok!=0;
}
bool contents_remove_copy(void* container,ContentsNativeItem& item) {
    ContentsScope scope;
    uint8_t ok=g_contents_remove_original(container,item.inventory_item,0,0,g_contents_api.remove.method);
    if(!ok)return false;
    contents_suppress(item.pickup);void* go=g_get_game_object.call(item.pickup,g_get_game_object.method);
    g_set_active.call(go,false,g_set_active.method);g_object_destroy.call(go,g_object_destroy.method);return true;
}
void contents_host_commit(uint32_t peer,const lc::Message& q) {
    auto& local=g_contents_lockers[q.locker];auto& authority=local.authority;
    auto old=authority.replies.find({peer,q.request});
    if(old!=authority.replies.end()){contents_send(old->second);return;}
    if(!authority.reserved||!(authority.pending.key==lc::Key{peer,q.request}))return;
    const auto request=authority.pending.request;
    void* container=contents_container(q.locker);if(!container)return;
    ContentsNativeItem native;
    if(request.deposit) {
        if(!contents_item(container,request.item.id,&native))return; // asynchronous load owns this path
    } else if(contents_item(container,request.item.id,&native)) {
        if(!contents_remove_copy(container,native))return;
    }
    auto done=authority.finish(peer,q,true);local.revision=done.revision;
    contents_journal_done(done,peer);
    contents_send(done);
    LOG_INFO("locker contents: committed %s id=%s item=%s request=%llu revision=%llu",
        request.deposit?"deposit":"take",q.locker.c_str(),request.item.id.c_str(),
        (unsigned long long)q.request,(unsigned long long)done.revision);
}
void contents_apply_grant(const lc::Message& grant) {
    auto& pending=g_contents_pending;if(!pending.used||pending.request.request!=grant.request||pending.request.locker!=grant.locker)return;
    auto& q=pending.request;
    if(pending.applied){lc::Message done=q;done.kind=lc::Applied;contents_send(done);return;}
    void* source=q.deposit?contents_inventory():contents_container(q.locker);
    void* destination=q.deposit?contents_container(q.locker):contents_inventory();ContentsNativeItem item;
    lc::Message intent=q;intent.kind=lc::Grant;
    if(!contents_journal(intent,net_local_id())){contents_notify("SNMP: не удалось записать журнал переноса; предмет остаётся на месте.");return;}
    if(!destination||!contents_item(source,q.item.id,&item)||!contents_move(destination,item.inventory_item)) {
        lc::Message cancel=q;cancel.kind=lc::Cancel;contents_send(cancel);
        contents_notify("SNMP: перенос отменён: нет места или предмет уже перемещён.");contents_journal_done(q,net_local_id());pending={};return;
    }
    contents_suppress(item.pickup);pending.applied=true;
    lc::Message m=q;m.kind=lc::Applied;
    if(!contents_journal(m,net_local_id())){g_contents_api.failed=true;contents_notify("SNMP: ошибка журнала после переноса. Сохрани диагностику.");return;}
    contents_send(m);pending.next_send=build_now()+2;
}
uint8_t contents_add_hook(void* destination,void* item,void* method) {
    if(g_contents_internal||!g_contents_api.ready||!contents_world_ready())return g_contents_add_original(destination,item,method);
    void* source=contents_field(item,g_contents_api.item_owner);std::string from,to;
    const bool source_locker=contents_parent(source,&from),destination_locker=contents_parent(destination,&to);
    if(!source_locker&&!destination_locker)return g_contents_add_original(destination,item,method);
    if(g_contents_api.failed)return 0;
    // Swaps and transfers to other storage are refused before the source is removed.
    void* inventory=contents_inventory();
    if(source_locker==destination_locker||
       (source_locker?destination!=inventory:source!=inventory))return 0;
    std::string id=destination_locker?to:from;
    if(g_contents_pending.used||g_contents_load.active)return 0;
    auto& local=g_contents_lockers[id];
    if(g_local_is_host){if(!contents_baseline(id)||local.authority.reserved)return 0;}
    else if(!local.ready||local.repairing){contents_query(id);contents_notify("SNMP: обновляется содержимое шкафа. Повтори перенос через секунду.");return 0;}
    void* pickup=contents_field(item,g_contents_api.item_pickup);lc::Message q;
    q.kind=lc::Request;q.locker=id;q.recipient=net_session_host_id();q.request=++g_contents_request;
    q.revision=g_local_is_host?local.authority.revision:local.revision;q.deposit=destination_locker;
    if(!build_live(pickup)||!contents_id(pickup,&q.item.id))return 0;
    q.item.tech=int32_t(g_pickup_get_tech.call(pickup,g_pickup_get_tech.method));
    if(!contents_serialize(pickup,&q.item.data)){contents_notify("SNMP: не удалось сохранить состояние предмета; перенос не выполнен.");return 0;}
    if(g_local_is_host) {
        auto grant=local.authority.begin(net_local_id(),q);if(grant.kind!=lc::Grant)return 0;
        bool ok=contents_move(destination,item);contents_suppress(pickup);
        auto done=local.authority.finish(net_local_id(),q,ok);local.revision=done.revision;
        if(ok){contents_send(done);LOG_INFO("locker contents: host %s id=%s item=%s revision=%llu",q.deposit?"deposit":"take",id.c_str(),q.item.id.c_str(),(unsigned long long)done.revision);}
        return ok;
    }
    if(!contents_send(q))return 0;
    g_contents_pending.used=true;g_contents_pending.request=std::move(q);g_contents_pending.next_send=build_now()+2;
    LOG_INFO("locker contents: requested %s id=%s item=%s request=%llu",destination_locker?"deposit":"take",id.c_str(),g_contents_pending.request.item.id.c_str(),(unsigned long long)g_contents_pending.request.request);
    return 0;
}
uint8_t contents_remove_hook(void* self,void* item,uint8_t forced,uint8_t verbose,void* method) {
    if(!g_contents_internal&&g_contents_api.ready&&contents_world_ready()) {
        std::string id;
        if(contents_parent(self,&id))return 0; // all authorized moves use the internal scope
        if(g_contents_pending.used) {
            void* pickup=contents_field(item,g_contents_api.item_pickup);std::string item_id;
            if(contents_id(pickup,&item_id)&&item_id==g_contents_pending.request.item.id)return 0;
        }
    }
    return g_contents_remove_original(self,item,forced,verbose,method);
}
uint8_t contents_allow_remove_hook(void* self,void* pickup,uint8_t verbose,void* method) {
    if(!g_contents_internal&&g_contents_api.ready&&contents_world_ready()) {
        std::string id;
        if(contents_parent(self,&id)) {
            auto& local=g_contents_lockers[id];
            if(g_contents_api.failed||g_contents_pending.used||local.repairing||
               (g_local_is_host?local.authority.reserved:!local.ready))return 0;
        }
    }
    return g_contents_allowed_remove_original(self,pickup,verbose,method);
}
void contents_click_hook(void* storage,void* hand,void* method) {
    if(!g_contents_api.ready||!contents_world_ready()){g_contents_click_original(storage,hand,method);return;}
    std::string id;void* container=contents_field(storage,g_locker_api.container_field);
    if(!contents_parent(container,&id)){g_contents_click_original(storage,hand,method);return;}
    if(g_contents_api.failed){contents_notify("SNMP: перенос вещей остановлен; сохрани отчёт диагностики.");return;}
    if(g_local_is_host){
        if(contents_baseline(id)&&!g_contents_lockers[id].authority.reserved)g_contents_click_original(storage,hand,method);
        else contents_notify("SNMP: шкаф занят переносом другого игрока.");
        return;
    }
    auto& local=g_contents_lockers[id];local.want_open=true;local.hand=hand;local.ready=false;contents_query(id);
}
uint8_t contents_deconstruct_hook(void* self,void** reason,void* method) {
    if(contents_world_ready()) {
        void* storage=contents_field(self,g_contents_api.obstacle_storage);
        std::string id;
        if(contents_parent(contents_field(storage,g_locker_api.container_field),&id)&&contents_locker_busy(id.c_str())) {
            if(reason)*reason=il2cpp_string_new("SNMP: дождись завершения переноса вещей в шкафу.");
            return 0;
        }
    }
    return g_contents_deconstruct_original(self,reason,method);
}
bool contents_bind() {
    auto& a=g_contents_api;if(a.ready)return true;if(a.failed||!build_locker_resolve())return false;
    a.items=build_class("ItemsContainer");auto* ii=build_class("InventoryItem");
    a.serializer=build_class("ProtobufSerializer");a.stream=find_class_in_domain("System.IO","MemoryStream");
    a.byte=find_class_in_domain("System","Byte");auto* ext=build_class("ProtobufSerializerExtensions");
    auto* host=build_class("CoroutineHost");auto* storage=build_class("StorageContainer");
    if(!a.items||!ii||!a.serializer||!a.stream||!a.byte||!ext||!host)return false;
    a.item_pickup=build_field_on(ii,"<item>k__BackingField",0x18);a.item_owner=build_field_on(ii,"container",0x20);
    a.pickup_inventory_item=build_field_on(build_class("Pickupable"),"inventoryItem",0x60);
    a.obstacle_storage=build_field_on(build_class("StorageObstacle"),"storageContainer",0x20);
    a.deconstruct.bind(find_method(build_class("StorageObstacle"),"IObstacle.CanDeconstruct",1));
    a.inventory.bind(find_method(g_inventory_class,"get_container",0));
    a.enumerate.bind(find_method(a.items,"IItemsContainer.GetEnumerator",0));
    a.serializer_ctor.bind(find_method(a.serializer,".ctor",0));a.stream_ctor.bind(find_method(a.stream,".ctor",0));
    a.stream_input.bind(build_typed_method(a.stream,".ctor",1,"System.Byte[]"));
    a.stream_bytes.bind(find_method(a.stream,"ToArray",0));a.serialize.bind(find_method(ext,"SerializeObjectTree",3));
    a.deserialize.bind(find_method(a.serializer,"DeserializeObjectsAsync",7));
    a.coroutine.bind(find_method(host,"StartCoroutine",1));
    a.add.bind(find_method(a.items,"IItemsContainer.AddItem",1));
    a.add_pickup.bind(build_typed_method(a.items,"AddItem",1,"Pickupable"));
    a.remove.bind(find_method(a.items,"IItemsContainer.RemoveItem",3));
    a.allow_remove.bind(find_method(a.items,"IItemsContainer.AllowedToRemove",2));
    a.room.bind(build_typed_method(a.items,"HasRoomFor",1,"Pickupable"));
    a.room_tech.bind(build_typed_method(a.items,"HasRoomFor",1,"TechType"));
    a.click.bind(find_method(storage,"OnHandClick",1));
    a.notify.bind(find_method(build_class("ErrorMessage"),"AddMessage",1));
    auto* array=find_class_in_domain("System","Array");
    // SetValue(object, int) is the only two-argument overload with an Int32
    // index; the other overload accepts an Int64 or an index array.
    void* iter=nullptr;
    for(auto* m=il2cpp_class_get_methods(array,&iter);m;m=il2cpp_class_get_methods(array,&iter)) {
        if(strcmp(il2cpp_method_get_name(m),"SetValue")||il2cpp_method_get_param_count(m)!=2)continue;
        void* t=il2cpp_type_get_object(il2cpp_method_get_param(m,1));
        Entry<BuildObjectFn> name;name.bind(find_method(il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(t)),"get_FullName",0));char text[64]{};
        if(name&&string_to_ascii(name.call(t,name.method),text,sizeof(text))&&!strcmp(text,"System.Int32")){a.array_set.bind(m);break;}
    }
    bool ready=a.item_pickup&&a.item_owner&&a.pickup_inventory_item&&a.obstacle_storage&&a.deconstruct&&a.inventory&&a.enumerate&&a.serializer_ctor&&a.stream_ctor&&a.stream_input&&
        a.stream_bytes&&a.serialize&&a.deserialize&&a.coroutine&&a.add&&a.add_pickup&&a.remove&&a.allow_remove&&a.room&&a.room_tech&&a.click&&a.array_set;
    if(!ready){
        static double next=0;if(build_now()>=next){next=build_now()+10;
            LOG_WARN("locker contents: API deferred fields=%u inventory=%u enumerate=%u serialize=%u deserialize=%u coroutine=%u array_set=%u add=%u room=%u",
                unsigned(a.item_pickup&&a.item_owner&&a.pickup_inventory_item),unsigned(bool(a.inventory)),unsigned(bool(a.enumerate)),
                unsigned(bool(a.serialize)),unsigned(bool(a.deserialize)),unsigned(bool(a.coroutine)),unsigned(bool(a.array_set)),unsigned(bool(a.add_pickup)),unsigned(bool(a.room_tech)));}
        return false;
    }
    if(!a.roots){a.roots=il2cpp_array_new(find_class_in_domain("System","Object"),4);if(!a.roots||!il2cpp_gchandle_new(reinterpret_cast<Il2CppObject*>(a.roots),true))return false;}
    a.ready=true;LOG_INFO("locker contents: API ready protocol=28 native item serialization; host authority");return true;
}
void contents_install() {
    auto& a=g_contents_api;if(a.hooks||!contents_bind())return;
    struct Hook {void* klass;void* method;uint16_t slot;void* replacement;void** original;};
    Hook hooks[]={
        {a.items,a.add.method,5,reinterpret_cast<void*>(&contents_add_hook),reinterpret_cast<void**>(&g_contents_add_original)},
        {a.items,a.remove.method,6,reinterpret_cast<void*>(&contents_remove_hook),reinterpret_cast<void**>(&g_contents_remove_original)},
        {a.items,a.allow_remove.method,8,reinterpret_cast<void*>(&contents_allow_remove_hook),reinterpret_cast<void**>(&g_contents_allowed_remove_original)},
        {build_class("StorageContainer"),a.click.method,6,reinterpret_cast<void*>(&contents_click_hook),reinterpret_cast<void**>(&g_contents_click_original)},
        {build_class("StorageObstacle"),a.deconstruct.method,5,reinterpret_cast<void*>(&contents_deconstruct_hook),reinterpret_cast<void**>(&g_contents_deconstruct_original)}};
    // Validate the whole set before the first mutation.
    void* entries[5]{};uint16_t slot=0,count=0;
    for(int i=0;i<5;++i){entries[i]=base_geometry::completion_vtable_entry(hooks[i].klass,hooks[i].method,&slot,&count,hooks[i].slot);if(!entries[i])return;}
    for(int i=0;i<5;++i){if(!*hooks[i].original)*hooks[i].original=*static_cast<void**>(entries[i]);if(!write_pointer(entries[i],hooks[i].replacement)){a.failed=true;return;}}
    a.hooks=true;LOG_INFO("locker contents: hooks ready ItemsContainer slots=5,6,8 StorageContainer slot=6 StorageObstacle slot=5");
}

void contents_open_ready(const std::string& id) {
    auto& local=g_contents_lockers[id];
    if(!local.ready||local.repairing||g_contents_pending.used)return;
    if(local.want_open){
        void* storage=contents_locker(id);if(!storage)return;
        local.want_open=false;g_contents_click_original(storage,local.hand,g_contents_api.click.method);
    }
}
void contents_reconcile(const std::string& id) {
    auto& local=g_contents_lockers[id];if(!local.repairing||g_contents_pending.used||g_contents_load.active)return;
    void* container=contents_container(id);std::vector<ContentsNativeItem> actual;
    if(!contents_items(container,&actual))return;
    for(auto& i:actual) {
        auto expected=std::find_if(local.manifest.begin(),local.manifest.end(),[&](const lc::Item& x){return x.id==i.state.id;});
        if(expected==local.manifest.end()){if(!contents_remove_copy(container,i))return;}
        else if(expected->tech!=i.state.tech){g_contents_api.failed=true;contents_notify("SNMP: конфликт типа предмета в шкафу; синхронизация остановлена.");return;}
    }
    bool missing=false;
    for(auto& expected:local.manifest) {
        ContentsNativeItem item;if(contents_item(container,expected.id,&item))continue;
        missing=true;
        if(build_now()>=local.next_query){lc::Message fetch;fetch.kind=lc::Fetch;fetch.recipient=net_session_host_id();fetch.locker=id;fetch.revision=local.revision;fetch.item=expected;contents_send(fetch);}
    }
    if(missing){if(build_now()>=local.next_query)local.next_query=build_now()+3;return;}
    local.repairing=false;local.ready=true;
    LOG_INFO("locker contents: snapshot matched id=%s items=%zu revision=%llu",id.c_str(),local.manifest.size(),(unsigned long long)local.revision);
    contents_open_ready(id);
}
bool contents_start_load(uint32_t origin,const lc::Message& message) {
    auto& a=g_contents_api;auto& load=g_contents_load;
    if(load.active||message.item.data.empty()||message.item.data.size()>lc::kMaxItemBytes)return false;
    // Never deserialize a peer's bytes over a real item in this player's hands.
    uint8_t guid[16]{};guid_from_string(message.item.id.c_str(),guid);
    void* existing=find_pickupable_by_guid(guid);void* existing_uid=nullptr;
    if(existing){
        void* go=g_get_game_object.call(existing,g_get_game_object.method);
        if(object_in_local_inventory(go)){contents_notify("SNMP: конфликт владельца предмета; перенос остановлен.");return false;}
        void* inventory_item=contents_field(existing,a.pickup_inventory_item);
        if(contents_field(inventory_item,a.item_owner)||g_go_get_active_self.call(go,g_go_get_active_self.method)) {
            LOG_WARN("locker contents: duplicate owned/active identity item=%s; refusing overwrite",message.item.id.c_str());return false;
        }
        // World pickup replication intentionally retains an inactive copy. The
        // game's restore API accepts its exact UID and updates that copy in place.
        existing_uid=build_uid(existing);if(!existing_uid)return false;
    }
    void* bytes=il2cpp_array_new(a.byte,unsigned(message.item.data.size()));
    void* serializer=il2cpp_object_new(a.serializer);void* stream=il2cpp_object_new(a.stream);
    if(!bytes||!serializer||!stream)return false;
    memcpy(static_cast<uint8_t*>(bytes)+0x20,message.item.data.data(),message.item.data.size());
    a.array_set.call(a.roots,serializer,1,a.array_set.method);a.array_set.call(a.roots,stream,2,a.array_set.method);a.array_set.call(a.roots,bytes,3,a.array_set.method);
    a.serializer_ctor.call(serializer,a.serializer_ctor.method);a.stream_input.call(stream,bytes,a.stream_input.method);
    void* task=a.deserialize.call(serializer,stream,existing_uid,1,1,nullptr,0,0,a.deserialize.method);if(!task)return false;
    a.array_set.call(a.roots,task,0,a.array_set.method);
    auto* task_class=il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(task));
    void* it=nullptr;void* result_field=nullptr;
    for(auto* f=il2cpp_class_get_fields(task_class,&it);f;f=il2cpp_class_get_fields(task_class,&it))if(!strcmp(il2cpp_field_get_name(f),"result")){result_field=f;break;}
    void* result=contents_field(task,result_field);if(!result)return false;
    auto* result_class=il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(result));it=nullptr;void* value_field=nullptr;
    for(auto* f=il2cpp_class_get_fields(result_class,&it);f;f=il2cpp_class_get_fields(result_class,&it))if(!strcmp(il2cpp_field_get_name(f),"value")){value_field=f;break;}
    if(!value_field)return false;
    load.active=true;load.message=message;load.origin=origin;load.task=task;load.result_field=result_field;load.value_field=value_field;load.started=build_now();
    a.coroutine.call(task,a.coroutine.method);
    LOG_INFO("locker contents: restore started id=%s item=%s bytes=%zu",message.locker.c_str(),message.item.id.c_str(),message.item.data.size());return true;
}
void contents_poll_load() {
    auto& a=g_contents_api;auto& load=g_contents_load;if(!load.active)return;
    void* result=contents_field(load.task,load.result_field);void* go=contents_field(result,load.value_field);
    if(!build_live(go)) {
        if(build_now()-load.started>30){a.failed=true;contents_notify("SNMP: восстановление предмета не завершилось. Сохрани отчёт диагностики; не повторяй перенос.");}
        return;
    }
    void* pickup=g_get_component.call(go,g_pickupable_type,g_get_component.method);std::string id;
    if(!pickup||!contents_id(pickup,&id)||id!=load.message.item.id||
       int32_t(g_pickup_get_tech.call(pickup,g_pickup_get_tech.method))!=load.message.item.tech) {
        a.failed=true;contents_notify("SNMP: восстановленный предмет не прошёл проверку идентификатора/типа.");return;
    }
    void* container=contents_container(load.message.locker);if(!container)return;
    if(!a.room.call(container,pickup,a.room.method)){a.failed=true;contents_notify("SNMP: в шкафу нет места для подтверждённого переноса; операция удерживается.");return;}
    contents_suppress(pickup);
    void* inventory_item=nullptr;
    {ContentsScope scope;
        if(g_pickup_original_slot)g_pickup_original_slot.call(pickup,false,g_pickup_original_slot.method);
        inventory_item=a.add_pickup.call(container,pickup,a.add_pickup.method);
    }
    if(!inventory_item){a.failed=true;contents_notify("SNMP: игра отклонила добавление восстановленного предмета.");return;}
    g_set_active.call(go,false,g_set_active.method);
    auto message=load.message;uint32_t origin=load.origin;
    load={};for(int i=0;i<4;++i)a.array_set.call(a.roots,nullptr,i,a.array_set.method);
    LOG_INFO("locker contents: restored id=%s item=%s state=serialized native=verified",message.locker.c_str(),id.c_str());
    if(g_local_is_host&&message.kind==lc::Applied)contents_host_commit(origin,message);
    else if(message.kind==lc::Done){g_contents_lockers[message.locker].revision=message.revision;g_contents_lockers[message.locker].ready=true;}
    contents_reconcile(message.locker);
}
void contents_handle(uint32_t origin,const lc::Message& m) {
    const uint32_t local_id=net_local_id(),host=net_session_host_id();
    if(m.kind!=lc::Done&&m.recipient&&m.recipient!=local_id)return;
    if(g_local_is_host) {
        if(m.kind==lc::Query){contents_snapshot(m.locker,origin);return;}
        if(m.kind==lc::Fetch) {
            if(!contents_baseline(m.locker))return;
            auto& authority=g_contents_lockers[m.locker].authority;
            if(authority.reserved||authority.revision!=m.revision){contents_snapshot(m.locker,origin);return;}
            ContentsNativeItem item;if(!contents_item(contents_container(m.locker),m.item.id,&item))return;
            lc::Message response=m;response.kind=lc::ItemState;response.recipient=origin;response.item=item.state;
            if(contents_serialize(item.pickup,&response.item.data))contents_send(response);return;
        }
        if(m.kind==lc::Request) {
            if(!m.request||!m.revision||m.item.tech<=0||!lc::id_valid(m.item.id)||m.item.data.empty())return;
            if(!contents_baseline(m.locker))return;
            auto& authority=g_contents_lockers[m.locker].authority;
            auto old=authority.replies.find({origin,m.request});
            if(old!=authority.replies.end()){contents_send(old->second);return;}
            void* container=contents_container(m.locker);
            if(!container){contents_send(authority.reply(m,lc::Reject,origin));return;}
            if(m.deposit&&!authority.reserved&&!g_contents_api.room_tech.call(container,m.item.tech,g_contents_api.room_tech.method)) {
                auto reject=authority.reply(m,lc::Reject,origin);authority.replies.emplace(lc::Key{origin,m.request},reject);contents_send(reject);return;
            }
            auto reply=authority.begin(origin,m);
            if(reply.kind==lc::Grant&&!contents_journal(m,origin))reply=authority.finish(origin,m,false);
            contents_send(reply);
            LOG_INFO("locker contents: %s peer=%08x id=%s item=%s request=%llu revision=%llu",reply.kind==lc::Grant?"reserved":"rejected",origin,m.locker.c_str(),m.item.id.c_str(),(unsigned long long)m.request,(unsigned long long)authority.revision);
            if(reply.kind==lc::Reject)contents_snapshot(m.locker,origin);return;
        }
        if(m.kind==lc::Cancel){
            auto& a=g_contents_lockers[m.locker].authority;
            const bool owned=a.reserved&&a.pending.key==lc::Key{origin,m.request};
            contents_send(a.finish(origin,m,false));if(owned)contents_journal_done(m,origin);contents_snapshot(m.locker,origin);return;
        }
        if(m.kind==lc::Applied) {
            auto& a=g_contents_lockers[m.locker].authority;auto old=a.replies.find({origin,m.request});
            if(old!=a.replies.end()){contents_send(old->second);return;}
            if(!a.reserved||!(a.pending.key==lc::Key{origin,m.request}))return;
            auto granted=a.pending.request;granted.kind=lc::Applied;ContentsNativeItem existing;
            if(granted.deposit&&!contents_item(contents_container(m.locker),granted.item.id,&existing)) {
                if(!contents_start_load(origin,granted)){g_contents_api.failed=true;contents_notify("SNMP: подтверждённый перенос удерживается: предмет не удалось восстановить.");}
            } else contents_host_commit(origin,granted);
            return;
        }
        return;
    }
    // Guests accept state and permission ONLY from the selected session host.
    if(origin!=host)return;
    auto& local=g_contents_lockers[m.locker];
    if(m.kind==lc::Grant){contents_apply_grant(m);return;}
    if(m.kind==lc::Reject) {
        if(g_contents_pending.used&&g_contents_pending.request.request==m.request&&g_contents_pending.request.locker==m.locker) {
            if(g_contents_pending.applied){g_contents_api.failed=true;contents_notify("SNMP: конфликт подтверждения переноса. Сохрани диагностику.");return;}
            g_contents_pending={};local.ready=false;local.next_query=0;contents_query(m.locker);
            contents_notify("SNMP: шкаф изменился или занят другим игроком. Повтори перенос после обновления.");
        }return;
    }
    if(m.kind==lc::Snapshot) {
        if(m.revision<local.revision||g_contents_pending.used)return;
        lc::Authority check;if(!m.revision||!check.initialize(m.items))return;
        local.revision=m.revision;local.ready=false;local.repairing=true;local.manifest=m.items;local.next_query=0;
        contents_reconcile(m.locker);return;
    }
    if(m.kind==lc::ItemState) {
        if(!local.repairing||m.revision!=local.revision)return;
        auto found=std::find_if(local.manifest.begin(),local.manifest.end(),[&](const lc::Item& i){return i.id==m.item.id&&i.tech==m.item.tech;});
        ContentsNativeItem existing;if(found==local.manifest.end()||contents_item(contents_container(m.locker),m.item.id,&existing))return;
        if(!contents_start_load(origin,m)){local.next_query=0;contents_query(m.locker);}return;
    }
    if(m.kind==lc::Done) {
        const bool own=g_contents_pending.used&&m.recipient==local_id&&g_contents_pending.request.request==m.request&&g_contents_pending.request.locker==m.locker;
        if(own) {
            if(!g_contents_pending.applied){g_contents_api.failed=true;contents_notify("SNMP: подтверждение пришло до локального переноса.");return;}
            contents_journal_done(g_contents_pending.request,local_id);
            g_contents_pending={};local.revision=m.revision;local.ready=true;
            LOG_INFO("locker contents: transfer finished id=%s item=%s request=%llu revision=%llu",m.locker.c_str(),m.item.id.c_str(),(unsigned long long)m.request,(unsigned long long)m.revision);return;
        }
        if(m.revision<=local.revision)return;
        if(!local.ready||m.revision!=local.revision+1||g_contents_pending.used){local.ready=false;local.next_query=0;contents_query(m.locker);return;}
        void* container=contents_container(m.locker);ContentsNativeItem item;
        if(m.deposit) {
            if(!contents_item(container,m.item.id,&item)) {
                local.ready=false;
                if(!contents_start_load(origin,m)){local.next_query=0;contents_query(m.locker);}return;
            }
        } else if(contents_item(container,m.item.id,&item)&&!contents_remove_copy(container,item))return;
        local.revision=m.revision;local.ready=true;
        LOG_INFO("locker contents: peer delta applied id=%s item=%s deposit=%u revision=%llu",m.locker.c_str(),m.item.id.c_str(),unsigned(m.deposit),(unsigned long long)m.revision);
    }
}
void contents_receive_frame(const WorldEvent& e) {
    constexpr size_t offset=offsetof(WorldEvent,guid),capacity=sizeof(WorldEvent)-offset;
    if(!e.origin_id||e.reserved16<lc::kFrameHeader||e.reserved16>capacity||g_contents_incoming.size()>=64)return;
    std::vector<uint8_t> blob;
    if(!g_contents_assembly[e.origin_id].append(reinterpret_cast<const uint8_t*>(&e)+offset,e.reserved16,capacity,&blob))return;
    if(blob.empty())return;
    lc::Message m;if(lc::decode(blob,&m))g_contents_incoming.emplace_back(e.origin_id,std::move(m));
    else LOG_WARN("locker contents: invalid encoded message peer=%08x",e.origin_id);
}
void tick_locker_contents(const LocalState& local) try {
    if(!local.valid||!g_build_baseline||!g_role_known)return;
    contents_install();auto& a=g_contents_api;if(!a.hooks||a.failed)return;
    if(!g_contents_journal_checked){
        g_contents_journal_checked=true;
        if(!contents_check_journals()){a.failed=true;contents_notify("SNMP: найден незавершённый перенос прошлого запуска. Вещи сохранены в журнале; нужен отчёт диагностики.");return;}
    }
    uint8_t session[16]{};uint32_t epoch=0;
    if(!net_geometry_session(session,&epoch))return;
    if(!g_contents_context||g_contents_epoch!=epoch||memcmp(session,g_contents_session,16)) {
        if(g_contents_context&&(g_contents_pending.used||g_contents_load.active)) {
            a.failed=true;contents_notify("SNMP: сессия сменилась во время переноса. Сохрани диагностику перед перезапуском.");return;
        }
        g_contents_lockers.clear();g_contents_assembly.clear();g_contents_outgoing.clear();g_contents_incoming.clear();
        memcpy(g_contents_session,session,16);g_contents_epoch=epoch;g_contents_context=true;
    }
    contents_poll_load();
    for(int i=0;i<4&&!g_contents_load.active&&!g_contents_incoming.empty();++i) {
        auto entry=std::move(g_contents_incoming.front());g_contents_incoming.pop_front();contents_handle(entry.first,entry.second);
    }
    for(auto& entry:g_contents_lockers)if(!g_local_is_host) {
        contents_reconcile(entry.first);
        // Queries made while the host reserves an item receive no stale snapshot.
        // Keep the open/refresh request alive without requiring another tap.
        if(!entry.second.ready&&!entry.second.repairing&&!g_contents_pending.used)contents_query(entry.first);
    }
    if(g_contents_pending.used&&build_now()>=g_contents_pending.next_send&&g_contents_outgoing.size()<8) {
        auto retry=g_contents_pending.request;if(g_contents_pending.applied)retry.kind=lc::Applied;
        contents_send(retry);g_contents_pending.next_send=build_now()+3;
    }
    constexpr size_t offset=offsetof(WorldEvent,guid),capacity=sizeof(WorldEvent)-offset;
    for(int i=0;i<4&&!g_contents_outgoing.empty()&&net_peer_count()&&net_pending_world_event_count()<32;++i) {
        auto& out=g_contents_outgoing.front();std::vector<uint8_t> frame;
        if(!lc::fragment(out.blob,out.id,out.part,capacity,&frame)){g_contents_outgoing.pop_front();continue;}
        WorldEvent event{};event.kind=kWorldEventLockerContents;event.reserved16=uint16_t(frame.size());
        memcpy(reinterpret_cast<uint8_t*>(&event)+offset,frame.data(),frame.size());
        if(!net_try_post_world_event(event))break;++out.part;
        if(out.part*(capacity-lc::kFrameHeader)>=out.blob.size())g_contents_outgoing.pop_front();
    }
} catch(...){
    g_contents_api.failed=true;
    contents_notify("SNMP: ошибка переноса вещей; повторные операции остановлены. Сохрани диагностический ZIP.");
}
