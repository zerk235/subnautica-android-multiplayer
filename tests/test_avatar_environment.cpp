#include "modloader/src/net.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
template<class Fn> struct Entry {
    Fn call=nullptr; void* method=nullptr;
    explicit operator bool() const { return call!=nullptr; }
};
struct Obj { bool live=true; const char* id=""; };
struct RemoteAvatar {
    uint32_t id=1;
    void *game_object=nullptr,*transform=nullptr,*sky_applier=nullptr;
    uint8_t sky_environment_id[16]{};
    bool sky_environment_bound=false;
    unsigned long sky_environment_next=0;
};
Obj player,visual,sky,base_a{true,"base-a"},base_b{true,"base-b"};
unsigned long g_avatar_frame=1;
void* g_remote_arms[2]{};
void* g_unique_identifier_type=&player;
void* g_avatar_subroot_type=&player;
void *local_environment=&base_a,*parent=nullptr,*applied=nullptr;
unsigned parent_calls=0,apply_calls=0;
std::vector<void*> interiors{&base_a,&base_b};
bool live(void* obj,void*) { return obj && static_cast<Obj*>(obj)->live; }
Entry<decltype(&live)> g_build_alive{&live};
bool build_live(void* obj) { return live(obj,nullptr); }
void* component(void* go,void*,void*) { return static_cast<Obj*>(go)->id[0] ? go : nullptr; }
Entry<decltype(&component)> g_get_component{&component};
template<class Fn> void walk_parents(void* go,Fn fn) { fn(go); }
void* get_id(void* uid,void*) { return const_cast<char*>(static_cast<Obj*>(uid)->id); }
Entry<decltype(&get_id)> g_unique_get_id{&get_id};
bool string_to_ascii(void* value,char* out,size_t n) {
    if (!value || strlen(static_cast<char*>(value))>=n) return false;
    strcpy(out,static_cast<char*>(value)); return true;
}
void guid_from_string(const char* id,uint8_t out[16]) {
    memset(out,0,16); out[0]=uint8_t(strlen(id)); out[1]=uint8_t(id[strlen(id)-1]);
}
void* get_environment(void*,void*) { return local_environment; }
Entry<decltype(&get_environment)> g_avatar_get_environment{&get_environment};
void set_parent(void*,void* target,bool keep_world,void*) {
    assert(keep_world); parent=target; ++parent_calls;
}
Entry<decltype(&set_parent)> g_set_parent{&set_parent};
void apply_environment(void* target,void* environment,void*) {
    assert(target==&sky); applied=environment; ++apply_calls;
}
Entry<decltype(&apply_environment)> g_avatar_apply_environment{&apply_environment};
void* find_objects(void*,bool inactive,void*) { assert(!inactive); return &interiors; }
Entry<decltype(&find_objects)> g_find_objects_all{&find_objects};
int length(void*,void*) { return int(interiors.size()); }
Entry<decltype(&length)> g_array_get_length{&length};
void* item(void*,int i,void*) { return interiors.at(i); }
Entry<decltype(&item)> g_array_get_value{&item};
void* object(void* obj,void*) { return obj; }
Entry<decltype(&object)> g_get_game_object{&object},g_get_go_transform{&object};
void log_sky_applier_fields(const char*,void*,int) {}
#include "modloader/src/avatar_environment.inc"
int main() {
    LocalState sample{};
    memset(sample.environment_id,0x77,16);
    read_avatar_environment(&player,&sample);
    const uint8_t zero[16]{};
    assert(!memcmp(sample.environment_id,zero,16)); // stale context clears outdoors
    sample.flags=kFlagInside;
    read_avatar_environment(&player,&sample);
    uint8_t a[16]{},b[16]{};
    guid_from_string("base-a",a); guid_from_string("base-b",b);
    assert(!memcmp(sample.environment_id,a,16));
    base_a.live=false;
    read_avatar_environment(&player,&sample);
    assert(!memcmp(sample.environment_id,zero,16));
    base_a.live=true;
    RemoteAvatar avatar;
    avatar.game_object=&visual; avatar.transform=&visual; avatar.sky_applier=&sky;
    PeerState peer{}; peer.id=7; peer.flags=kFlagInside;
    memcpy(peer.environment_id,a,16);
    update_avatar_environment(&avatar,peer);
    assert(parent==&base_a && applied==&base_a && avatar.sky_environment_bound);
    const auto calls=apply_calls;
    // Night/day and viewer context do not enter the selection: peer stays in base A.
    local_environment=&base_b;
    ++g_avatar_frame;
    update_avatar_environment(&avatar,peer);
    assert(parent==&base_a && apply_calls==calls);
    memcpy(peer.environment_id,b,16);
    update_avatar_environment(&avatar,peer);
    assert(parent==&base_b && applied==&base_b);
    peer.flags=0; // exit, even with stale nonzero identity in a packet
    update_avatar_environment(&avatar,peer);
    assert(!parent && !applied && !avatar.sky_environment_bound);
    peer.flags=kFlagInside;
    memcpy(peer.environment_id,a,16);
    interiors={&base_b}; // missing A must not borrow nearest/viewer's base B
    update_avatar_environment(&avatar,peer);
    assert(!parent && !avatar.sky_environment_bound);
    interiors={&base_a,&base_a};
    g_avatar_frame+=31;
    update_avatar_environment(&avatar,peer);
    assert(!parent && !avatar.sky_environment_bound); // duplicate identity refused
    interiors={&base_a};
    g_avatar_frame+=31;
    update_avatar_environment(&avatar,peer);
    assert(parent==&base_a && avatar.sky_environment_bound);
    visual.live=false;
    g_remote_arms[0]=&visual;
    assert(!avatar_object_live(&avatar) && !g_remote_arms[0]);
    puts("actual avatar environment: exact interior, transitions, missing/duplicate IDs and destroyed parent passed");
}
