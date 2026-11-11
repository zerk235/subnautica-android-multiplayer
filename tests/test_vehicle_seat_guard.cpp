#include "modloader/src/vehicle_occupancy.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)

constexpr size_t kMethodPointerOffset=0,kVirtualPointerOffset=8,kMethodSlotOffset=0x50;
constexpr size_t kClassVtableCountOffset=0x126,kClassVtableOffset=0x138,kVirtualInvokeSize=16;
struct Il2CppMethodInfo {alignas(void*) char bytes[0x58]{};};
struct Il2CppClass {alignas(void*) char bytes[0x138+64*16]{};};
Il2CppClass moth,base,errors;
Il2CppMethodInfo click_method,enter_method,message_method;
template<class Fn> struct Entry {
    Fn call=nullptr;void* method=nullptr;
    void bind(Il2CppMethodInfo* value) {
        method=value;call=value?reinterpret_cast<Fn>(*reinterpret_cast<void**>(value)):nullptr;
    }
    explicit operator bool() const {return call!=nullptr;}
};
double clock_now=100;
double monotonic_seconds(){return clock_now;}
SessionState session=kSessionHost;
SessionState net_session_state(){return session;}
uint32_t net_local_id(){return 10;}
VehicleState remote{};PeerState peer{};
size_t net_copy_vehicles(VehicleState* out,size_t){out[0]=remote;return remote.active?1:0;}
size_t net_copy_peers(PeerState* out,size_t){out[0]=peer;return peer.active?1:0;}
bool lookup_throws=false,message_throws=false;
uint8_t native_guid=1;
bool component_guid(void*,uint8_t out[16]) {
    if(lookup_throws)throw 1;
    memset(out,0,16);out[0]=native_guid;return true;
}
void* get_game_object(void* component,void*){return component;}
Entry<void* (*)(void*,void*)> g_get_game_object{get_game_object,nullptr};
struct DrivenVehicle {bool used=false;char classId[16]{};void* game_object=nullptr;uint8_t guid[16]{};};
DrivenVehicle g_driven[4];
void* cached_managed_string(const char* text){return const_cast<char*>(text);}
int clicks=0,enters=0,messages=0,writes=0;
void original_click(void*,void*,void*){++clicks;}
void original_enter(void*,void*,bool teleport,bool animation,void*) {
    assert(teleport&&!animation);++enters;
}
void add_message(void*,void*){if(message_throws)throw 2;++messages;}
Il2CppClass* find_class_in_domain(const char*,const char* name) {
    if(!strcmp(name,"SeaMoth"))return &moth;
    if(!strcmp(name,"Vehicle"))return &base;
    if(!strcmp(name,"ErrorMessage"))return &errors;
    return nullptr;
}
Il2CppMethodInfo* find_method_typed(Il2CppClass* klass,const char* name,int,const char*) {
    if(klass==&base&&!strcmp(name,"OnHandClick"))return &click_method;
    if(klass==&moth&&!strcmp(name,"EnterVehicle"))return &enter_method;
    if(klass==&errors&&!strcmp(name,"AddMessage"))return &message_method;
    return nullptr;
}
bool write_pointer(void* address,void* value){++writes;*static_cast<void**>(address)=value;return true;}
#include "modloader/src/vehicle_seat_guard.inc"

void set_method(Il2CppMethodInfo& method,unsigned short slot,void* fn) {
    *reinterpret_cast<void**>(method.bytes)=fn;
    *reinterpret_cast<void**>(method.bytes+8)=fn;
    *reinterpret_cast<unsigned short*>(method.bytes+0x50)=slot;
}
void** slot(Il2CppClass& klass,unsigned n){return reinterpret_cast<void**>(klass.bytes+0x138+n*16);}
int main() {
    // Actual Unity 6 layout: the old header's +0x126 reads an event count.
    // Poison that location to reproduce the first live build's refusal to arm.
    *reinterpret_cast<unsigned short*>(moth.bytes+0x126)=1;
    *reinterpret_cast<unsigned short*>(base.bytes+0x126)=1;
    *reinterpret_cast<unsigned short*>(moth.bytes+0x12a)=64;
    *reinterpret_cast<unsigned short*>(base.bytes+0x12a)=64;
    set_method(click_method,6,reinterpret_cast<void*>(&original_click));
    set_method(enter_method,53,reinterpret_cast<void*>(&original_enter));
    set_method(message_method,0xffff,reinterpret_cast<void*>(&add_message));
    *slot(moth,6)=*slot(base,6)=reinterpret_cast<void*>(&original_click);
    *slot(moth,53)=reinterpret_cast<void*>(&original_enter);
    *slot(moth,7)=reinterpret_cast<void*>(&original_click);
    install_seamoth_seat_hooks();
    assert(g_seamoth_seat_hooks&&writes==2);
    assert(*slot(base,6)==reinterpret_cast<void*>(&original_click)); // shared base untouched
    assert(*slot(moth,7)==reinterpret_cast<void*>(&original_click)); // unrelated slot untouched
    install_seamoth_seat_hooks();assert(writes==2);

    hooked_seamoth_hand_click(&moth,nullptr,&click_method);
    hooked_seamoth_enter(&moth,nullptr,true,false,&enter_method);
    assert(clicks==1&&enters==1);
    remote.active=remote.seq_valid=true;remote.pilot=20;remote.updated=clock_now;
    remote.guid[0]=1;remote.sequence=100;memcpy(remote.classId,"SeaMoth",8);
    peer.id=20;peer.active=peer.seq_valid=true;peer.last_seen=clock_now;
    peer.sequence=101;peer.flags=kFlagPiloting;
    hooked_seamoth_hand_click(&moth,nullptr,&click_method);
    hooked_seamoth_enter(&moth,nullptr,true,false,&enter_method);
    assert(clicks==1&&enters==1&&messages==1); // neither native entry path ran

    clock_now+=1.1;message_throws=true;
    hooked_seamoth_hand_click(&moth,nullptr,&click_method);
    assert(clicks==1); // a broken notice does not permit entry
    peer.flags=0;peer.sequence=102;
    hooked_seamoth_hand_click(&moth,nullptr,&click_method);
    hooked_seamoth_enter(&moth,nullptr,true,false,&enter_method);
    assert(clicks==2&&enters==2); // newer exit restores both original paths
    peer.flags=kFlagPiloting;lookup_throws=true;
    hooked_seamoth_hand_click(&moth,nullptr,&click_method);
    assert(clicks==3); // identity failure preserves the game's own interaction
    lookup_throws=false;session=kSessionDiscovering;
    hooked_seamoth_hand_click(&moth,nullptr,&click_method);
    assert(clicks==4); // solo/menu does not become locked

    void* original=nullptr;
    Il2CppClass wrong;
    *reinterpret_cast<unsigned short*>(wrong.bytes+0x12a)=64;
    *slot(wrong,6)=reinterpret_cast<void*>(&original_enter);
    assert(!seamoth_patch_slot(&wrong,&click_method,6,reinterpret_cast<void*>(&hooked_seamoth_hand_click),&original));
    assert(!original&&writes==2); // unknown dispatch/layout is never overwritten

    // Live failure: the visible prefab has a different local GUID from the
    // host's vehicle. Resolve only the exact tracked object to its wire GUID.
    session=kSessionHost;native_guid=2;
    remote.updated=peer.last_seen=clock_now;
    peer.flags=kFlagPiloting;
    hooked_seamoth_hand_click(&moth,nullptr,&click_method);
    assert(clicks==5); // genuinely different native vehicle remains available
    g_driven[0].used=true;g_driven[0].game_object=&moth;
    memcpy(g_driven[0].classId,"SeaMoth",8);g_driven[0].guid[0]=1;
    lookup_throws=true; // the replica path must not depend on its native GUID
    hooked_seamoth_hand_click(&moth,nullptr,&click_method);
    hooked_seamoth_enter(&moth,nullptr,true,false,&enter_method);
    assert(clicks==5&&enters==2);
    lookup_throws=false;
    g_driven[0].game_object=&base;
    hooked_seamoth_hand_click(&moth,nullptr,&click_method);
    assert(clicks==6); // nearby / other object is never blocked by association
    g_driven[0].game_object=&moth;g_driven[0].used=false;
    hooked_seamoth_hand_click(&moth,nullptr,&click_method);
    assert(clicks==7); // unused tracking records do not claim a seat
    g_driven[0].used=true;peer.flags=0;peer.sequence=104;
    hooked_seamoth_hand_click(&moth,nullptr,&click_method);
    assert(clicks==8); // explicit exit releases even a still-tracked replica
    puts("actual vehicle seat hooks: SeaMoth-only dispatch, both admission paths, exit, message failure and unknown-slot safety passed");
}
