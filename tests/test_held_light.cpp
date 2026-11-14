#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>
#include "modloader/src/held_light.h"
#define LOG_INFO(...) ((void)0)
template<class Fn> struct Entry { Fn call;void* method=nullptr;explicit operator bool() const{return call!=nullptr;} };
struct Il2CppObject {};
struct Node {Node* parent=nullptr;std::vector<Node*> children;bool active=true,enabled=true,is_lamp=false,is_toggle=false,lights_active=false;Node* lights_parent=nullptr;};
Node prop,group,lamp,decor,toggle,outside_lamp;Node* held=&prop;
int light_type,toggle_type,parent_field,active_field;
void* g_light_type_obj=&light_type;bool g_get_component_is_string=false;
unsigned long g_avatar_frame=1;
constexpr uint32_t kFlagToolLight=1u<<9;
struct LocalState {uint32_t held_tech=751,flags=0;};
struct PeerState {uint32_t id=1,held_tech=751,flags=0;};
struct RemoteAvatar {void* held_prop=&prop;void* held_lights_parent=nullptr;void* held_lights[8]{};bool held_light_enabled[8]{};unsigned held_light_count=0;uint32_t held_tech=751,id=1;};
void* component(void* go,void* type,void*) {auto* n=(Node*)go;return ((type==&light_type && n->is_lamp)||(type==&toggle_type && n->is_toggle)) ? n : nullptr;}
void* identity(void* go,void*){return go;}
int32_t child_count(void* go,void*){return ((Node*)go)->children.size();}
void* child(void* go,int32_t i,void*){return ((Node*)go)->children.at(i);}
bool enabled(void* go,void*){return ((Node*)go)->enabled;}
bool hierarchy(void* go,void*){auto* n=(Node*)go;for(int i=0;n && i<128;++i,n=n->parent)if(!n->active)return false;return true;}
void enable(void* go,bool value,void*){((Node*)go)->enabled=value;}
void activate(void* go,bool value,void*){((Node*)go)->active=value;}
void* inventory_get(void*,void*){return held;}
uint32_t tech(void*,void*){return 507;}
Entry<decltype(&component)> g_get_component{component};
Entry<decltype(&identity)> g_get_go_transform{identity},g_get_game_object{identity};
Entry<decltype(&child_count)> g_get_child_count{child_count};
Entry<decltype(&child)> g_get_child{child};
Entry<decltype(&enabled)> g_behaviour_get_enabled{enabled},g_go_get_active_hierarchy{hierarchy};
Entry<decltype(&enable)> g_behaviour_set_enabled{enable},g_set_active{activate};
Entry<decltype(&inventory_get)> g_inventory_get_held{inventory_get};
Entry<decltype(&tech)> g_pickup_get_tech{tech};
void* g_inventory_class=&prop;
void* read_static_object_field(void*,const char*){return &prop;}
void* find_component_in_hierarchy(void* go,void* type,void*,int){if(void* result=component(go,type,nullptr))return result;for(auto* c:((Node*)go)->children)if(void* result=find_component_in_hierarchy(c,type,nullptr,0))return result;return nullptr;}
bool prop_owns_transform(void* root,void* go){for(auto* n=(Node*)go;n;n=n->parent)if(n==root)return true;return false;}
void il2cpp_field_get_value(Il2CppObject* object,void* field,void* out){auto* n=(Node*)object;if(field==&parent_field)*(Node**)out=n->lights_parent;else if(field==&active_field)*(bool*)out=n->lights_active;else assert(false);}
#include "modloader/src/held_light.inc"
int main(){
    prop.children={&group,&toggle,&outside_lamp};group.parent=&prop;toggle.parent=&prop;
    outside_lamp.parent=&prop;outside_lamp.is_lamp=true;
    group.children={&lamp,&decor};lamp.parent=&group;decor.parent=&group;
    lamp.is_lamp=decor.is_lamp=true;decor.enabled=false;
    toggle.is_toggle=true;toggle.lights_parent=&group;toggle.lights_active=true;
    g_toggle_lights_type=&toggle_type;g_toggle_lights_parent=&parent_field;g_toggle_lights_active=&active_field;
    LocalState local;local.held_tech=507;
    read_local_held_light(&local);assert(local.flags&kFlagToolLight);
    lamp.enabled=false;read_local_held_light(&local);assert(!(local.flags&kFlagToolLight));
    lamp.enabled=true;group.active=false;read_local_held_light(&local);assert(!(local.flags&kFlagToolLight));
    group.active=true;toggle.lights_active=false;read_local_held_light(&local);assert(!(local.flags&kFlagToolLight));
    toggle.lights_active=true;held=nullptr;read_local_held_light(&local);assert(!(local.flags&kFlagToolLight));held=&prop;
    RemoteAvatar avatar;avatar.held_tech=507;assert(prepare_remote_held_light(&avatar,&prop,507));
    assert(!toggle.enabled && !lamp.enabled && !outside_lamp.enabled && !group.active && avatar.held_light_count==2);
    PeerState peer;peer.held_tech=507;peer.flags=kFlagToolLight;apply_peer_held_light(&avatar,peer);
    assert(group.active && lamp.enabled && !decor.enabled && lamp.parent==&group);
    assert(!outside_lamp.enabled);
    peer.flags=0;apply_peer_held_light(&avatar,peer);assert(!group.active && !lamp.enabled);
    peer.flags=kFlagToolLight;peer.held_tech=751;apply_peer_held_light(&avatar,peer);assert(!group.active && !lamp.enabled);
    peer.held_tech=507;apply_peer_held_light(&avatar,peer);assert(group.active && lamp.enabled);
    Node foreign;toggle.lights_parent=&foreign;RemoteAvatar other;
    assert(!prepare_remote_held_light(&other,&prop,507) && foreign.active);
    toggle.lights_parent=&prop;
    assert(!prepare_remote_held_light(&other,&prop,507) && prop.active);
    assert(!prepare_remote_held_light(&other,&prop,509));
    puts("actual held-light path: owner toggle, energy-disabled lamp, inactive parent, holster, copied driver disabled, authored lamp parent, off/on and mismatched tool guards passed");
}
