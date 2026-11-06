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
struct Array {int32_t length;};
struct Base {
    bool ghost=false,placed=false,alive=true;
    void* placement=nullptr;
    int32_t anchor[3]{},shape[4]{1,1,1,1};
    int32_t cell_offset[3]{};
    Array faces{6},cells{1},links{1},glass{1},objects{1},masks{1};
    Vector3 origin{10,20,30}; Quaternion rotation{0,0,0,1};
    std::string id="session-base";
} base;
std::vector<Base*> bases{&base};
int ghost_field,placed_field,placement_field,anchor_field,shape_field;
int faces_field,cells_field,links_field,glass_field,objects_field;
void il2cpp_field_get_value(Il2CppObject* object,void* field,void* out) {
    auto* b=reinterpret_cast<Base*>(object);
    if(field==&ghost_field) *static_cast<bool*>(out)=b->ghost;
    else if(field==&placed_field) *static_cast<bool*>(out)=b->placed;
    else if(field==&placement_field) *static_cast<void**>(out)=b->placement;
    else if(field==&anchor_field) memcpy(out,b->anchor,sizeof(b->anchor));
    else if(field==&shape_field) memcpy(out,b->shape,sizeof(b->shape));
    else {
        Array* array=field==&faces_field ? &b->faces : field==&cells_field ? &b->cells :
            field==&links_field ? &b->links : field==&glass_field ? &b->glass :
            field==&objects_field ? &b->objects : nullptr;
        assert(array);*static_cast<void**>(out)=array->length<0 ? nullptr : array;
    }
}
bool build_live(void* p){return p!=nullptr;}
bool build_resolve_corridor_api(){return true;}
bool build_resolve_hatch_capture(){return true;}
double build_now(){return 100;}
void* build_uid(void* p){return p;}
void* uid(void* p,void*){return const_cast<char*>(static_cast<Base*>(p)->id.c_str());}
bool string_to_ascii(void* p,char* out,size_t capacity){snprintf(out,capacity,"%s",static_cast<char*>(p));return true;}
void* all(void*,bool include_inactive,void*){assert(include_inactive);return &bases;}
int length(void* p,void*){return p==&bases ? int(bases.size()) : static_cast<Array*>(p)->length;}
void* value(void*,int i,void*){return bases.at(i);}
void* same(void* p,void*){return p;}
Quaternion rotation(void* p,void*){return static_cast<Base*>(p)->rotation;}
Vector3 position(void* p,void*){return static_cast<Base*>(p)->origin;}
Vector3 grid_world(void* p,HatchPoint cell,void*){
    auto origin=static_cast<Base*>(p)->origin;
    auto* b=static_cast<Base*>(p);
    return {origin.x+cell.x+b->cell_offset[0],origin.y+cell.y+b->cell_offset[1],origin.z+cell.z+b->cell_offset[2]};
}
Entry<decltype(&all)> g_find_objects_all{&all};
Entry<decltype(&length)> g_array_get_length{&length};
Entry<decltype(&value)> g_array_get_value{&value};
Entry<decltype(&uid)> g_unique_get_id{&uid};
Entry<decltype(&same)> g_get_game_object{&same},g_gameobject_get_transform{&same};
Entry<decltype(&rotation)> g_get_rotation{&rotation};
Entry<decltype(&position)> g_get_position{&position};
void* g_build_base_geometry_type=&base;
struct {void* base_is_ghost=&ghost_field,*base_is_placed=&placed_field,*placement_ghost=&placement_field,*cell_objects=&objects_field;}g_corridor_api;
struct {void* anchor=&anchor_field,*shape=&shape_field,*faces=&faces_field,*cells=&cells_field,*links=&links_field,*glass=&glass_field;}g_ghost_capture;
struct {Entry<decltype(&grid_world)>grid_world{&::grid_world};}g_hatch_capture;
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
    birth.geometry.base_pose={10,20,30,0,0,0,1};
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
    // AllocateArrays grows existing arrays to twice the requested size. masks
    // remains a one-cell preview cache. Both are normal on a completed Base.
    base.shape[2]=2;
    base.faces.length=24;base.cells.length=base.links.length=base.glass.length=base.objects.length=4;
    assert(base.masks.length==1 && build_corridor_target(base.id,anchor)==&base);
    // Extending in the negative direction reindexes cells, but leaves the Base
    // transform unchanged. The old birth integer cell no longer has its old pose.
    base.cell_offset[2]=-1;base.anchor[2]=1;
    assert(grid_world(&base,{4,5,6},nullptr).z!=birth.root_pose[2]);
    assert(build_corridor_target(base.id,anchor)==&base);
    for (auto* array:{&base.faces,&base.cells,&base.links,&base.glass,&base.objects}) {
        const auto capacity=array->length;
        array->length=0;assert(!build_corridor_target(base.id,anchor));
        array->length=-1;assert(!build_corridor_target(base.id,anchor));
        array->length=capacity;
    }
    base.shape[3]=2;assert(!build_corridor_target(base.id,anchor));base.shape[3]=1;
    base.shape[0]=0;assert(!build_corridor_target(base.id,anchor));base.shape[0]=1;
    base.shape[2]=4097;assert(!build_corridor_target(base.id,anchor));base.shape[2]=2;
    birth.geometry.base_pose[0]=NAN;assert(!build_corridor_target(base.id,anchor));birth.geometry.base_pose[0]=10;
    birth.root_pose_valid=false;assert(!build_corridor_target(base.id,anchor));birth.root_pose_valid=true;
    birth.geometry.creates_base=false;assert(!build_corridor_target(base.id,anchor));birth.geometry.creates_base=true;
    base.anchor[0]=1;assert(!build_corridor_target(base.id,anchor));base.anchor[0]=0;
    // A saved/placed base may also be reindexed after this session has recorded
    // a verified cell frame. An arbitrary anchor change still fails above.
    base.placed=true;birth.geometry.creates_base=false;
    assert(build_corridor_target(base.id,anchor)==&base);
    birth.geometry.creates_base=true;base.placed=false;
    Base duplicate=base;bases.push_back(&duplicate);assert(!build_corridor_target(base.id,anchor));bases.pop_back();
    g_synced_corridors[1]=birth;assert(!build_corridor_target(base.id,anchor));g_synced_corridors[1]={};
    assert(!build_corridor_target("foreign-base",anchor));
    birth.removed=true;assert(build_corridor_target(base.id,anchor)==&base); // other pieces may survive birth cell removal
    puts("PASS: completed base grows and reindexes without losing target identity; preview, busy, duplicate, transform, short storage and anchor guards preserved");
}
