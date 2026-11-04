#include "modloader/src/base_geometry_corridor.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#define LOG_WARN(...) ((void)0)
#define LOG_INFO(...) ((void)0)
struct Il2CppObject {};
struct BuildGridPoint {int32_t x,y,z;};
template<class Fn>struct Entry{Fn call=nullptr;void* method=nullptr;explicit operator bool()const{return call!=nullptr;}};
struct Array{uintptr_t header[3]{};size_t length=16;uint8_t data[192]{};};
static_assert(offsetof(Array,data)==32,"array ABI");
struct Geometry{int32_t shape[4]{3,1,3,3};Array faces,cells,links,masks,glass;bool no_masks=false;} geometry;
int shape_field,faces_field,cells_field,links_field,masks_field,glass_field;
struct{void* shape=&shape_field,*faces=&faces_field,*cells=&cells_field,*links=&links_field,*masks=&masks_field,*glass=&glass_field;}g_ghost_capture;
void il2cpp_field_get_value(Il2CppObject* obj,void* field,void* out){
    auto* g=reinterpret_cast<Geometry*>(obj);
    if(field==&shape_field){memcpy(out,g->shape,16);return;}
    Array* a=nullptr;
    if(field==&faces_field)a=&g->faces;else if(field==&cells_field)a=&g->cells;
    else if(field==&links_field)a=&g->links;else if(field==&glass_field)a=&g->glass;
    else {assert(field==&masks_field);a=g->no_masks?nullptr:&g->masks;}
    *static_cast<void**>(out)=a;
}
int32_t length(void* p,void*){return int32_t(static_cast<Array*>(p)->length);}
Entry<decltype(&length)>g_array_get_length{&length};
uint8_t target_raw[9]{1,3,3,3,3,3,3,3,3};
uint8_t raw(void*,BuildGridPoint p,void*){
    assert(p.y==2 && p.x>=10 && p.x<=12 && p.z>=20 && p.z<=22);
    return target_raw[(p.x-10)+3*(p.z-20)];
}
struct{Entry<decltype(&raw)>get_raw_cell{&raw};}g_corridor_api;
#include "modloader/src/base_topology_capture.inc"
#include "modloader/src/base_foundation_readback.inc"
void reset(){
    geometry=Geometry{};geometry.faces.length=96;
    memset(geometry.faces.data,1,54);memset(geometry.faces.data+54,255,42);
    for(int z=0;z<3;++z)for(int x=0;x<3;++x){const int i=x+3*z;geometry.cells.data[i]=i?3:1;geometry.links.data[i]=uint8_t((x<<5)|z);}
    for(Array* a:{&geometry.cells,&geometry.links,&geometry.masks,&geometry.glass})memset(a->data+9,255,7);
}
int main(){
    reset();base_geometry::Snapshot s;s.shape={3,1,3};
    assert(build_copy_topology_arrays(&geometry,&s,false,true));
    assert(base_geometry::room_topology_valid(s)&&s.faces.size()==54&&s.links[8]==66);
    assert(!build_copy_topology_arrays(&geometry,&s,false));
    assert(!build_copy_topology_arrays(&geometry,&s,true,true));
    geometry.no_masks=true;assert(build_copy_topology_arrays(&geometry,&s,false,true)&&s.masks.empty());
    geometry.faces.length=95;assert(!build_copy_topology_arrays(&geometry,&s,false,true));geometry.faces.length=96;
    geometry.cells.length=8;assert(!build_copy_topology_arrays(&geometry,&s,false,true));geometry.cells.length=4097;
    assert(!build_copy_topology_arrays(&geometry,&s,false,true));geometry.cells.length=16;
    for(int i=0;i<9;++i){auto old=geometry.links.data[i];geometry.links.data[i]^=1;assert(!build_copy_topology_arrays(&geometry,&s,false,true));geometry.links.data[i]=old;}
    geometry.glass.data[8]=1;assert(!build_copy_topology_arrays(&geometry,&s,false,true));geometry.glass.data[8]=0;
    s.shape={2,1,2};assert(!build_copy_topology_arrays(&geometry,&s,false,true));s.shape={3,1,3};
    assert(build_copy_topology_arrays(&geometry,&s,false,true));
    base_geometry::GhostEnvelope e;e.object_id="room";e.target_base_id="base";e.ghost_class="BaseAddCellGhost";e.target_offset={10,2,20};e.geometry=s;
    assert(base_geometry::supported_existing_piece(e,5500));assert(!base_geometry::supported_existing_piece(e,5509));
    auto bad=e;bad.target_base_id.clear();assert(!base_geometry::supported_existing_piece(bad,5500));
    bad=e;bad.geometry.cells[0]=17;assert(!base_geometry::supported_existing_piece(bad,5500));
    bad=e;bad.target_offset[0]=8191;assert(!base_geometry::supported_existing_piece(bad,5500));
    bad=e;bad.ghost_class="OtherGhost";assert(!base_geometry::supported_existing_piece(bad,5500));
    int32_t bounds[6]{10,2,20,12,2,22};assert(base_geometry::piece_deconstruction_bounds(e,5500,bounds));
    bounds[3]--;assert(!base_geometry::piece_deconstruction_bounds(e,5500,bounds));
    bad=e;bad.target_offset={12,2,22};bad.geometry.shape={1,1,1};assert(base_geometry::piece_footprints_overlap(e,bad));
    bad.target_offset[0]=13;assert(!base_geometry::piece_footprints_overlap(e,bad));
    assert(build_room_footprint_matches(&geometry,e,false));
    for(int i=0;i<9;++i){const auto old=target_raw[i];target_raw[i]=4;assert(!build_room_footprint_matches(&geometry,e,false));target_raw[i]=old;}
    assert(!build_piece_footprint_free(&geometry,e,5500));memset(target_raw,0,9);
    assert(build_room_footprint_matches(&geometry,e,true)&&build_piece_footprint_free(&geometry,e,5500));
    target_raw[8]=3;assert(!build_room_footprint_matches(&geometry,e,true));
    assert(!build_foundation_has_other_root(&geometry));geometry.cells.data[8]=4;assert(build_foundation_has_other_root(&geometry));
    puts("actual room helpers: native topology/links, bounded storage, strict other recipes, all9 raw cells, bounds, overlap and last-root guards passed");
}
