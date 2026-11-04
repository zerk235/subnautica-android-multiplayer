#include "modloader/src/net.h"
#include "modloader/src/world.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <vector>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
struct Il2CppObject {};
struct Il2CppClass {};
using ManagedFn=void (*)(void*,void*);
template<class Fn>struct Entry {
    Fn call=nullptr;void* method=nullptr;
    void bind(void* p){call=reinterpret_cast<Fn>(p);}
    explicit operator bool()const{return call!=nullptr;}
};
double now=100.0;
double monotonic_seconds(){return now;}
struct {
    pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    SessionState state=kSessionSlotReady;
    bool active=true,local_member=true,join_accepted=true;
    bool local_host=false,local_has_world=false,world_active=false,explicit_leave=false;
    char slot[24]="host-slot";
}g_session;
#include "modloader/src/autoload_request.inc"
SessionState net_session_state(){return g_session.state;}
Il2CppClass menu_class,host_class;
int main_field,starting_field,menu_token,routine_token,coroutine_token;
bool have_menu=true,have_host_class=true,have_start_method=true,have_fields=true;
bool starting=false,host_received=true,metadata_valid=true,no_strings=false;
bool no_routine=false,no_coroutine=false,throw_load=false;
bool replace_on_string=false,reenter_start=false;
int load_calls=0,start_calls=0;
std::string loaded_slot,loaded_session;
int loaded_change=-1,loaded_mode=-1,loaded_strategy=-1;
void menu_autoload_tick();
void* load(void* menu,void* slot,void* session,int32_t changes,int32_t mode,int32_t strategy,void*){
    assert(menu==&menu_token);++load_calls;
    loaded_slot=static_cast<const char*>(slot);loaded_session=static_cast<const char*>(session);
    loaded_change=changes;loaded_mode=mode;loaded_strategy=strategy;
    if(throw_load)throw std::runtime_error("native load failure");
    return no_routine?nullptr:&routine_token;
}
void* start(void* routine,void*){
    assert(routine==&routine_token);++start_calls;
    if(reenter_start)menu_autoload_tick();
    return no_coroutine?nullptr:&coroutine_token;
}
Il2CppClass* find_class_in_domain(const char* ns,const char* name){
    if(!strcmp(name,"uGUI_MainMenu")){assert(!*ns);return &menu_class;}
    assert(!strcmp(ns,"UWE")&&!strcmp(name,"CoroutineHost"));
    return have_host_class?&host_class:nullptr;
}
void* find_method(Il2CppClass* c,const char* name,int n){
    if(c==&menu_class){assert(!strcmp(name,"LoadGameAsync")&&n==5);return reinterpret_cast<void*>(&load);}
    assert(c==&host_class&&!strcmp(name,"StartCoroutine")&&n==1);
    return have_start_method?reinterpret_cast<void*>(&start):nullptr;
}
void* autoload_field_on(Il2CppClass* c,const char* name,int off){
    assert(c==&menu_class);if(!have_fields)return nullptr;
    if(!strcmp(name,"main")){assert(off==0);return &main_field;}
    assert(!strcmp(name,"isStartingNewGame")&&off==0x80);return &starting_field;
}
void il2cpp_field_static_get_value(void* f,void* out){assert(f==&main_field);*static_cast<void**>(out)=have_menu?&menu_token:nullptr;}
void il2cpp_field_get_value(Il2CppObject* o,void* f,void* out){
    assert(o==reinterpret_cast<Il2CppObject*>(&menu_token)&&f==&starting_field);
    *static_cast<uint8_t*>(out)=starting;
}
bool world_slot_is_host_received(const char* slot){assert(!strcmp(slot,"host-slot"));return host_received;}
bool world_read_load_fields(const char* slot,WorldLoadFields* fields){
    assert(!strcmp(slot,"host-slot"));fields->valid=metadata_valid;
    strcpy(fields->session,"11111111-2222-3333-4444-555555555555");fields->change_set=83315;fields->game_mode=3;
    return metadata_valid;
}
// Managed string storage remains stable across calls, like actual managed objects.
std::vector<char*>strings;
void* il2cpp_string_new(const char* s){
    if(replace_on_string){replace_on_string=false;net_world_autoload_notify("new-host-slot",2);}
    if(no_strings)return nullptr;
    auto* v=strdup(s);strings.push_back(v);return v;
}
#include "modloader/src/menu_autoload.inc"
void reset(){
    for(auto s:strings)free(s);strings.clear();
    g_session.state=kSessionSlotReady;g_session.active=g_session.local_member=g_session.join_accepted=true;
    g_session.local_host=g_session.local_has_world=g_session.world_active=g_session.explicit_leave=false;
    strcpy(g_session.slot,"host-slot");g_auto_load_request.pending=false;
    have_menu=have_host_class=have_start_method=have_fields=true;
    starting=false;host_received=metadata_valid=true;no_strings=no_routine=no_coroutine=throw_load=false;
    replace_on_string=reenter_start=false;load_calls=start_calls=0;
    g_menu_api_resolved=false;g_menu_api_retry_at=0.0;now=100.0;
    g_autoload_started_once=false;g_autoload_started_at=0.0;g_autoload_slot[0]=0;
    net_world_autoload_notify("host-slot",1);
}
int main(){
    reset();menu_autoload_tick();
    assert(load_calls==1&&start_calls==1&&loaded_slot=="host-slot");
    assert(loaded_session=="11111111-2222-3333-4444-555555555555");
    assert(loaded_change==83315&&loaded_mode==3&&loaded_strategy==0);
    assert(!g_auto_load_request.pending&&g_autoload_started_at==now);
    menu_autoload_tick();assert(load_calls==1);
    net_world_autoload_notify("host-slot",1);menu_autoload_tick();assert(load_calls==1);
    net_world_autoload_notify("host-slot",2);menu_autoload_tick();assert(load_calls==2&&g_autoload_generation==2);
    reset();have_menu=false;menu_autoload_tick();assert(!load_calls&&g_auto_load_request.pending);
    have_menu=true;menu_autoload_tick();assert(load_calls==1);
    reset();have_host_class=false;menu_autoload_tick();assert(!load_calls&&g_auto_load_request.pending);
    have_host_class=true;now+=2.1;menu_autoload_tick();assert(load_calls==1);
    reset();have_start_method=false;menu_autoload_tick();assert(!load_calls);
    have_start_method=true;now+=2.1;menu_autoload_tick();assert(load_calls==1);
    reset();have_fields=false;menu_autoload_tick();assert(!load_calls);
    have_fields=true;now+=2.1;menu_autoload_tick();assert(load_calls==1);
    for(auto state:{kSessionDiscovering,kSessionJoinPending,kSessionTransferring,kSessionConflict,kSessionFailed,kSessionHost,kSessionGuest}){
        reset();g_session.state=state;menu_autoload_tick();assert(!load_calls);
    }
    for(int guard=0;guard<7;++guard){
        reset();switch(guard){
        case 0:g_session.local_host=true;break;case 1:g_session.world_active=true;break;
        case 2:g_session.local_has_world=true;break;case 3:g_session.join_accepted=false;break;
        case 4:g_session.active=false;break;case 5:g_session.explicit_leave=true;break;
        case 6:strcpy(g_session.slot,"unrelated-world");break;}
        menu_autoload_tick();assert(!load_calls&&g_auto_load_request.pending);
    }
    reset();starting=true;menu_autoload_tick();assert(!load_calls&&!g_auto_load_request.pending);
    reset();host_received=false;menu_autoload_tick();assert(!load_calls&&!g_auto_load_request.pending);
    reset();metadata_valid=false;menu_autoload_tick();assert(!load_calls&&!g_auto_load_request.pending);
    reset();now+=181;menu_autoload_tick();assert(!load_calls&&!g_auto_load_request.pending);
    reset();no_strings=true;menu_autoload_tick();assert(!load_calls&&g_auto_load_request.pending);
    no_strings=false;menu_autoload_tick();assert(load_calls==1);
    reset();replace_on_string=true;menu_autoload_tick();assert(!load_calls&&g_auto_load_request.pending);
    assert(!strcmp(g_auto_load_request.slot,"new-host-slot"));
    assert(!net_world_autoload_pop_if("host-slot",1)&&g_auto_load_request.pending);
    assert(!net_world_autoload_pop_if("new-host-slot",1)&&g_auto_load_request.pending);
    assert(net_world_autoload_pop_if("new-host-slot",2)&&!g_auto_load_request.pending);
    reset();reenter_start=true;menu_autoload_tick();assert(load_calls==1&&start_calls==1);
    reset();no_routine=true;menu_autoload_tick();assert(load_calls==1&&!start_calls&&!g_auto_load_request.pending);
    menu_autoload_tick();assert(load_calls==1);
    reset();no_coroutine=true;menu_autoload_tick();assert(start_calls==1&&g_autoload_started_at==0.0);
    reset();throw_load=true;menu_autoload_tick();assert(load_calls==1&&!start_calls&&g_autoload_started_at==0.0);
    puts("actual autoload request + consumer: exact slot/session metadata, static persistent host ABI, menu/API retry, duplicate/reentrant launch, stale-generation race, live-world/session/marker guards, failures passed");
}
