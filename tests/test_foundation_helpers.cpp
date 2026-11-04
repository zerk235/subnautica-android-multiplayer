#include "modloader/src/base_geometry_corridor.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#define LOG_WARN(...) ((void)0)
#define LOG_INFO(...) ((void)0)
struct Il2CppObject {};
struct BuildGridPoint {int32_t x,y,z;};
template<class Fn>struct Entry{Fn call=nullptr;void* method=nullptr;explicit operator bool()const{return call!=nullptr;}};
struct Array{uintptr_t header[3]{};size_t length=8;uint8_t data[192]{};};
static_assert(offsetof(Array,data)==32,"array ABI");
struct Geometry{int32_t shape[4]{2,1,2,2};Array faces,cells,links,masks,glass;bool no_masks=false;} geometry;
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
uint8_t target_raw[4]{2,3,3,3};
uint8_t raw(void*,BuildGridPoint p,void*){
    assert(p.y==2 && p.x>=10 && p.x<=11 && p.z>=20 && p.z<=21);
    return target_raw[(p.x-10)+2*(p.z-20)];
}
struct{Entry<decltype(&raw)>get_raw_cell{&raw};}g_corridor_api;
#include "modloader/src/base_topology_capture.inc"
#include "modloader/src/base_foundation_readback.inc"
void reset(){
    geometry=Geometry{};geometry.faces.length=48;
    memset(geometry.faces.data,1,24);memset(geometry.faces.data+24,255,24);
    const uint8_t cells[4]{2,3,3,3},links[4]{0,32,1,33};
    memcpy(geometry.cells.data,cells,4);memcpy(geometry.links.data,links,4);
    for(Array* a:{&geometry.cells,&geometry.links,&geometry.masks,&geometry.glass})memset(a->data+4,255,4);
}
int main(){
    reset();base_geometry::Snapshot s;s.shape={2,1,2};
    assert(build_copy_topology_arrays(&geometry,&s,true));
    assert(base_geometry::foundation_topology_valid(s) && s.faces.size()==24 && s.cells.size()==4);
    base_geometry::Snapshot strict;strict.shape=s.shape;
    assert(!build_copy_topology_arrays(&geometry,&strict,false));
    geometry.no_masks=true;assert(build_copy_topology_arrays(&geometry,&s,true) && s.masks.empty());
    geometry.faces.length=47;assert(!build_copy_topology_arrays(&geometry,&s,true));geometry.faces.length=48;
    geometry.cells.length=3;assert(!build_copy_topology_arrays(&geometry,&s,true));geometry.cells.length=4097;
    assert(!build_copy_topology_arrays(&geometry,&s,true));geometry.cells.length=8;
    geometry.links.data[1]=0;assert(!build_copy_topology_arrays(&geometry,&s,true));geometry.links.data[1]=32;
    geometry.glass.data[2]=1;assert(!build_copy_topology_arrays(&geometry,&s,true));geometry.glass.data[2]=0;
    s.shape={1,1,1};assert(!build_copy_topology_arrays(&geometry,&s,true));s.shape={2,1,2};
    assert(build_copy_topology_arrays(&geometry,&s,true));
    base_geometry::GhostEnvelope e;e.object_id="foundation";e.target_base_id="base";
    e.ghost_class="BaseAddCellGhost";e.target_offset={10,2,20};e.geometry=s;
    assert(base_geometry::supported_existing_piece(e,5509));
    assert(!base_geometry::supported_existing_cell(e,5509));
    auto bad=e;bad.target_base_id.clear();assert(!base_geometry::supported_existing_piece(bad,5509));
    bad=e;bad.geometry.cells[1]=2;assert(!base_geometry::supported_existing_piece(bad,5509));
    bad=e;bad.geometry.links[3]=255;assert(!base_geometry::supported_existing_piece(bad,5509));
    bad=e;bad.target_offset[0]=8192;assert(!base_geometry::supported_existing_piece(bad,5509));
    int32_t bounds[6]{10,2,20,11,2,21};assert(base_geometry::piece_deconstruction_bounds(e,5509,bounds));
    bounds[3]=10;assert(!base_geometry::piece_deconstruction_bounds(e,5509,bounds));
    auto overlap=e;overlap.geometry.shape={1,1,1};overlap.target_offset={11,2,21};
    assert(base_geometry::piece_footprints_overlap(e,overlap));overlap.target_offset[0]=12;
    assert(!base_geometry::piece_footprints_overlap(e,overlap));
    assert(build_foundation_footprint_matches(&geometry,e,false));
    target_raw[3]=4;assert(!build_foundation_footprint_matches(&geometry,e,false));
    assert(!build_piece_footprint_free(&geometry,e,5509));
    memset(target_raw,0,4);assert(build_foundation_footprint_matches(&geometry,e,true));
    assert(build_piece_footprint_free(&geometry,e,5509));target_raw[2]=3;
    assert(!build_foundation_footprint_matches(&geometry,e,true));
    assert(!build_foundation_has_other_root(&geometry)); // aliases and trailing capacity ignored
    geometry.cells.data[3]=4;assert(build_foundation_has_other_root(&geometry));
    geometry.shape[3]=99;assert(!build_foundation_has_other_root(&geometry));
    puts("actual foundation helpers: bounded active capacity, strict other recipes, exact 2x2 cells/packed links, bounds/overlap, raw quarter readback/free footprint and active root counting passed");
}
