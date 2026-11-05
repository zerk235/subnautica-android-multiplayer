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
struct BuildGridPoint {int32_t x,y,z;};
struct HatchPoint {int32_t x,y,z;};
struct BuildGridBounds {BuildGridPoint min,max;};
template<class Fn>struct Entry {Fn call=nullptr;void* method=nullptr;explicit operator bool()const{return call!=nullptr;}};
struct Base {Vector3 p{100,10,100};Quaternion q{0,0,0,1};int32_t anchor[3]{};};
Base ghost_base,target;
int component,model,ghost,model_field,base_field,target_field,anchor_field,geometry_type;
void* g_build_base_geometry_type=&geometry_type;
void* current_target=nullptr;
bool glass_ok=true,footprint_ok=true,wrong_bounds=false;
int raw_type=2;
size_t g_build_id_root_count=0;
constexpr size_t kBuildKnownLimit=512;
bool build_live(void* p){return p!=nullptr;}
bool build_resolve_hatch_capture(){return true;}
bool build_resolve_corridor_api(){return true;}
unsigned il2cpp_gchandle_new(Il2CppObject*,bool pinned){assert(pinned);return 1;}
bool build_keep_id_root(unsigned root){if(!root||g_build_id_root_count>=kBuildKnownLimit)return false;++g_build_id_root_count;return true;}
struct{void *model=&model_field,*ghost_base=&base_field,*target=&target_field,*anchor=&anchor_field,*ghost_type=&ghost;}g_ghost_capture;
void il2cpp_field_get_value(Il2CppObject* obj,void* field,void* out){
    if(field==&model_field){assert(obj==reinterpret_cast<Il2CppObject*>(&component));*static_cast<void**>(out)=&model;}
    else if(field==&base_field)*static_cast<void**>(out)=&ghost_base;
    else if(field==&target_field)*static_cast<void**>(out)=current_target;
    else{assert(field==&anchor_field);memcpy(out,reinterpret_cast<Base*>(obj)->anchor,12);}
}
void* component_call(void*,void* type,void*){return type==g_build_base_geometry_type ? static_cast<void*>(&ghost_base) : &ghost;}
Entry<decltype(&component_call)>g_get_component{&component_call};
void* build_uid(void* p){return p;}
void* id_call(void*,void*){return const_cast<char*>("host-generated-base-id");}
Entry<decltype(&id_call)>g_unique_get_id{&id_call};
bool string_to_ascii(void* s,char* out,size_t n){snprintf(out,n,"%s",static_cast<char*>(s));return true;}
void* object_call(void* p,void*){return p==&model ? &ghost_base : p;}
Entry<decltype(&object_call)>g_get_game_object{&object_call},g_gameobject_get_transform{&object_call};
Vector3 position(void* p,void*){return static_cast<Base*>(p)->p;}
Quaternion rotation(void* p,void*){return static_cast<Base*>(p)->q;}
Entry<decltype(&position)>g_get_position{&position};
Entry<decltype(&rotation)>g_get_rotation{&rotation};
BuildGridBounds bounds(void*,void*){return {{-1,0,-2},{wrong_bounds?1:0,0,-1}};}
uint8_t raw_cell(void*,BuildGridPoint,void*){return raw_type;}
struct{Entry<decltype(&bounds)>bounds{&::bounds};Entry<decltype(&raw_cell)>get_raw_cell{&raw_cell};}g_corridor_api;
Vector3 grid_world(void* b,HatchPoint p,void*){auto* base=static_cast<Base*>(b);return {base->p.x+p.x-base->anchor[0],base->p.y+p.y-base->anchor[1],base->p.z+p.z-base->anchor[2]};}
HatchPoint world_grid(void* b,Vector3 p,void*){auto* base=static_cast<Base*>(b);return {int(p.x-base->p.x)+base->anchor[0],int(p.y-base->p.y)+base->anchor[1],int(p.z-base->p.z)+base->anchor[2]};}
struct{Entry<decltype(&grid_world)>grid_world{&::grid_world};Entry<decltype(&world_grid)>world_grid{&::world_grid};}g_hatch_capture;
bool build_corridor_glass_matches(void*,BuildGridPoint,int){return glass_ok;}
bool build_room_footprint_matches(void*,const base_geometry::GhostEnvelope&,bool){return footprint_ok;}
bool build_foundation_footprint_matches(void*,const base_geometry::GhostEnvelope&,bool){return footprint_ok;}
#include "modloader/src/base_creation.inc"
#include "modloader/src/base_corridor_registry.inc"
bool base_available=true;
bool build_base_id_exists(const std::string&){return base_available;}
void* build_corridor_target(const std::string&,const std::array<int32_t,3>&,bool){return base_available ? &target : nullptr;}
#include "modloader/src/base_detached_removal.inc"

int main(){
    using namespace base_geometry;
    GhostEnvelope e;e.object_id="construction-id";e.ghost_class="BaseAddCellGhost";
    e.geometry.shape={2,1,2};e.geometry.cell_offset={-1,0,-2};
    e.geometry.cells={2,3,3,3};e.geometry.links={0,32,1,33};
    e.geometry.faces.assign(24,1);e.geometry.glass.assign(4,0);
    assert(standalone_piece_topology(e,5509));assert(!supported_piece_placement(e,5509));
    assert(!standalone_piece_topology(e,5504)&&!standalone_piece_topology(e,5501)&&!standalone_piece_topology(e,5519));
    FirstBaseCapture capture;
    wrong_bounds=true;assert(!build_capture_first_base(&component,e,&capture));wrong_bounds=false;
    current_target=&target;assert(!build_capture_first_base(&component,e,&capture));current_target=nullptr;
    g_build_id_root_count=kBuildKnownLimit;assert(!build_capture_first_base(&component,e,&capture));g_build_id_root_count=0;
    assert(build_capture_first_base(&component,e,&capture)&&capture.ghost==&ghost&&g_build_id_root_count==1);
    assert(capture.root_world.x==99&&capture.root_world.z==98);
    assert(!build_finish_first_base(capture,&e,5509)); // failed original transition cannot invent an ID
    current_target=&target;target.anchor[0]=4;target.anchor[2]=6;
    assert(build_finish_first_base(capture,&e,5509));
    assert(e.creates_base&&e.target_base_id=="host-generated-base-id");
    assert(e.target_offset==(std::array<int32_t,3>{3,0,4}));
    assert(supported_piece_placement(e,5509)&&valid_base_pose(e));
    std::vector<uint8_t> bytes;GhostEnvelope decoded;
    assert(encode_envelope(e,&bytes)&&decode_envelope(bytes.data(),bytes.size(),&decoded));
    assert(decoded.creates_base&&decoded.base_pose==e.base_pose&&decoded.target_offset==e.target_offset);
    WorldEvent state{};state.tech_type=5509;
    assert(build_created_base_present(&target,state,e));
    target.q.w=-1;assert(build_created_base_present(&target,state,e));target.q.w=1;
    target.p.x+=0.1f;assert(!build_created_base_present(&target,state,e));target.p.x=100;
    ++target.anchor[0];assert(!build_created_base_present(&target,state,e));--target.anchor[0];
    raw_type=1;assert(!build_created_base_present(&target,state,e));raw_type=2;
    glass_ok=false;assert(!build_created_base_present(&target,state,e));glass_ok=true;
    footprint_ok=false;assert(!build_created_base_present(&target,state,e));footprint_ok=true;
    snprintf(state.object_id,sizeof(state.object_id),"%s",e.object_id.c_str());
    build_remember_corridor(state,e);
    base_available=false;build_record_piece_root(state,capture.completed_base);base_available=true;
    auto* record=build_synced_corridor(state.object_id);assert(record && record->root_pose_valid);
    auto detached=e;detached.target_base_id.clear();detached.ghost_class="BaseGhost";
    assert(!build_detached_demolition(&component,state,detached)); // base must actually be gone
    base_available=false;assert(build_detached_demolition(&component,state,detached)==record);
    ghost_base.p.x+=0.01f;assert(!build_detached_demolition(&component,state,detached));ghost_base.p.x=100;
    ghost_base.q={0,1,0,0};assert(!build_detached_demolition(&component,state,detached));ghost_base.q={0,0,0,-1};
    assert(build_detached_demolition(&component,state,detached)==record);ghost_base.q={0,0,0,1};
    record->removed=true;assert(!build_detached_demolition(&component,state,detached));record->removed=false;
    g_synced_corridors[1]=*record;assert(!build_detached_demolition(&component,state,detached));g_synced_corridors[1]={};
    auto wrong=state;wrong.tech_type=5500;assert(!build_detached_demolition(&component,wrong,detached));
    detached.geometry.cells[0]=4;assert(!build_detached_demolition(&component,state,detached));
    e.target_base_id=e.object_id;assert(!supported_piece_placement(e,5509));
    puts("first base: post-transition identity, remapped root, exact pose/anchor/type/footprint, duplicate identity, bounds and capacity checks passed");
}
