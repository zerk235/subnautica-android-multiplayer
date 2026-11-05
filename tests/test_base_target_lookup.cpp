#define SNMOD_BASE_GEOMETRY_CAPTURE 1
#include "modloader/src/net.h"
#include "modloader/src/base_geometry_corridor.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#define LOG_INFO(...) ((void)0)
struct Il2CppObject {};
struct Vector3 {float x,y,z;};
struct Quaternion {float x,y,z,w;};
struct HatchPoint {int32_t x,y,z;};
template<class Fn> struct Entry {Fn call;void* method=nullptr;};
struct Base {
    bool ghost=false,placed=false,alive=true;
    void* placement=nullptr;
    int32_t anchor[3]{},shape[4]{1,1,1,1};
    Vector3 origin{10,20,30}; Quaternion rotation{0,0,0,1};
    std::string id="session-base";
} base;
std::vector<Base*> bases{&base};
int ghost_field,placed_field,placement_field,anchor_field,shape_field;
void il2cpp_field_get_value(Il2CppObject* object,void* field,void* out) {
    auto* b=reinterpret_cast<Base*>(object);
    if(field==&ghost_field) *static_cast<bool*>(out)=b->ghost;
    else if(field==&placed_field) *static_cast<bool*>(out)=b->placed;
    else if(field==&placement_field) *static_cast<void**>(out)=b->placement;
    else if(field==&anchor_field) memcpy(out,b->anchor,sizeof(b->anchor));
    else { assert(field==&shape_field);memcpy(out,b->shape,sizeof(b->shape)); }
}
bool build_live(void* p){return p!=nullptr;}
bool build_resolve_corridor_api(){return true;}
bool build_resolve_hatch_capture(){return true;}
double build_now(){return 100;}
void* build_uid(void* p){return p;}
void* uid(void* p,void*){return const_cast<char*>(static_cast<Base*>(p)->id.c_str());}
bool string_to_ascii(void* p,char* out,size_t capacity){snprintf(out,capacity,"%s",static_cast<char*>(p));return true;}
void* all(void*,bool include_inactive,void*){assert(include_inactive);return &bases;}
int length(void*,void*){return bases.size();}
void* value(void*,int i,void*){return bases.at(i);}
void* same(void* p,void*){return p;}
Quaternion rotation(void* p,void*){return static_cast<Base*>(p)->rotation;}
Vector3 grid_world(void* p,HatchPoint cell,void*){
    auto origin=static_cast<Base*>(p)->origin;
    return {origin.x+cell.x,origin.y+cell.y,origin.z+cell.z};
}
Entry<decltype(&all)> g_find_objects_all{&all};
Entry<decltype(&length)> g_array_get_length{&length};
Entry<decltype(&value)> g_array_get_value{&value};
Entry<decltype(&uid)> g_unique_get_id{&uid};
Entry<decltype(&same)> g_get_game_object{&same},g_gameobject_get_transform{&same};
Entry<decltype(&rotation)> g_get_rotation{&rotation};
void* g_build_base_geometry_type=&base;
struct {void* base_is_ghost=&ghost_field,*base_is_placed=&placed_field,*placement_ghost=&placement_field;}g_corridor_api;
struct {void* anchor=&anchor_field,*shape=&shape_field;}g_ghost_capture;
struct {Entry<decltype(&grid_world)>grid_world{&::grid_world};}g_hatch_capture;
bool storage_valid=true;
bool build_corridor_storage_valid(void*,const base_geometry::Snapshot& s){return storage_valid && s.shape==std::array<int32_t,3>{1,1,1};}
#include "modloader/src/base_corridor_registry.inc"
#include "modloader/src/base_target_lookup.inc"

int main(){
    const std::array<int32_t,3> anchor{};
    base.placed=true; assert(build_corridor_target(base.id,anchor)==&base);
    base.placed=false; assert(!build_corridor_target(base.id,anchor)); // unknown prefab
    auto& birth=g_synced_corridors[0];
    birth.used=true; birth.geometry.creates_base=true; birth.geometry.target_base_id=base.id;
    birth.geometry.target_offset={4,5,6};birth.root_pose_valid=true;
    birth.root_pose={14,25,36,0,0,0,1};
    if(build_corridor_target(base.id,anchor)!=&base){
        puts("FAIL: completed session base with isPlaced=0 is rejected before deletion can run");
        return 1;
    }
    // All these cases must remain blocked even with a known birth record.
    base.ghost=true;assert(!build_corridor_target(base.id,anchor));base.ghost=false;
    base.placement=&base;assert(!build_corridor_target(base.id,anchor));base.placement=nullptr;
    base.origin.x+=1;assert(!build_corridor_target(base.id,anchor));base.origin.x-=1;
    base.rotation={0,1,0,0};assert(!build_corridor_target(base.id,anchor));base.rotation={0,0,0,-1};
    assert(build_corridor_target(base.id,anchor)==&base);base.rotation.w=1;
    base.origin.x=NAN;assert(!build_corridor_target(base.id,anchor));base.origin.x=10;
    storage_valid=false;assert(!build_corridor_target(base.id,anchor));storage_valid=true;
    base.shape[0]=2;assert(!build_corridor_target(base.id,anchor));base.shape[0]=1;
    birth.root_pose_valid=false;assert(!build_corridor_target(base.id,anchor));birth.root_pose_valid=true;
    birth.geometry.creates_base=false;assert(!build_corridor_target(base.id,anchor));birth.geometry.creates_base=true;
    base.anchor[0]=1;assert(!build_corridor_target(base.id,anchor));base.anchor[0]=0;
    Base duplicate=base;bases.push_back(&duplicate);assert(!build_corridor_target(base.id,anchor));bases.pop_back();
    g_synced_corridors[1]=birth;assert(!build_corridor_target(base.id,anchor));g_synced_corridors[1]={};
    assert(!build_corridor_target("foreign-base",anchor));
    birth.removed=true;assert(build_corridor_target(base.id,anchor)==&base); // other pieces may survive birth cell removal
    puts("PASS: actual base lookup accepts verified session birth; preview, busy, identity, frame, storage and anchor guards preserved");
}
