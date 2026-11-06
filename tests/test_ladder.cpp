#define SNMOD_BASE_GEOMETRY_CAPTURE 1
#include "modloader/src/net.h"
#include "modloader/src/base_geometry_corridor.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <tuple>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
struct Il2CppObject {};
struct Vector3 { float x,y,z; };
struct HatchPoint {int32_t x,y,z;};
struct HatchFace {HatchPoint cell;int32_t direction;};
template<class Fn>struct Entry {
    Fn call=nullptr;void* method=nullptr;
    explicit operator bool()const{return call!=nullptr;}
    void bind(void*){}
};
int component_token,model_token,ghost_token,base_token,target_token;
int model_field,base_field,target_field,anchor_field;
struct {void* model=&model_field,*ghost_base=&base_field,*target=&target_field,*anchor=&anchor_field,*ghost_type=nullptr;}g_ghost_capture;
bool wrong_class=false,no_target=false,permission=true,bad_exit=false,deny_set=false,bad_readback=false,throw_write=false;
bool capturing=true,missing_link=false;
bool generic_ghost=false;
int height=3,writes=0,clears=0,rebuilds=0;
using Key=std::tuple<int,int,int,int>;
std::map<Key,uint8_t> faces;
Key key(HatchFace f){return {f.cell.x,f.cell.y,f.cell.z,f.direction};}
bool build_live(void* p){return p!=nullptr;}
void il2cpp_field_get_value(Il2CppObject*,void* f,void* out){
    if(f==&model_field)*static_cast<void**>(out)=&model_token;
    else if(f==&base_field)*static_cast<void**>(out)=&base_token;
    else if(f==&target_field)*static_cast<void**>(out)=no_target?nullptr:&target_token;
    else{assert(f==&anchor_field);const int32_t a[3]={0,0,0};memcpy(out,a,12);}
}
void* component(void*,void*,void*){return &ghost_token;}
Entry<decltype(&component)>g_get_component{&component};
void* il2cpp_object_get_class(Il2CppObject*){return &ghost_token;}
const char* il2cpp_class_get_name(void*){return wrong_class?"BaseAddModuleGhost":generic_ghost?"BaseGhost":"BaseAddLadderGhost";}
void* build_field_on(void*,const char*,int){return nullptr;}
void static_get(void*,void*){}
auto il2cpp_field_static_get_value=&static_get;
void* build_class(const char*){return nullptr;}
void* find_method(void*,const char*,int){return nullptr;}
void* build_typed_method(void*,const char*,int,const char*){return nullptr;}
bool build_resolve_hatch_capture(){return true;}
void* build_uid(void*){return &target_token;}
void* uid(void*,void*){return &target_token;}
Entry<decltype(&uid)>g_unique_get_id{&uid};
bool string_to_ascii(void*,char* out,size_t n){assert(n>12);strcpy(out,"shared-base");return true;}
int first_direction=4;
uint8_t first(void*,HatchFace* out,void*){*out={{0,first_direction==4?0:height-1,0},first_direction};return 1;}
Vector3 world(void*,HatchPoint p,void*){return {float(p.x),float(p.y),float(p.z)};}
HatchPoint grid(void*,Vector3 p,void*){return {int(p.x)+2,int(p.y)+3,int(p.z)+4};}
uint8_t get_face(void* target,HatchFace f,void*){
    if(capturing){
        assert(target==&base_token);
        return missing_link&&f.cell.y==1?1:5;
    }
    assert(target==&target_token);return faces[key(f)];
}
struct {
    Entry<decltype(&first)>find_face{&first};
    Entry<decltype(&world)>grid_world{&world};
    Entry<decltype(&grid)>world_grid{&grid};
    Entry<decltype(&get_face)>get_face{&::get_face};
}g_hatch_capture;
#include "modloader/src/base_ladder_capture.inc"
uint8_t cell(void*,HatchPoint p,void*){
    const int y=p.y-(capturing?0:3);
    return missing_link&&y==1?0:(y==0||y==height-1?4:6);
}
uint8_t exit_cell(void*,HatchPoint p,int d,HatchPoint* out,void*){
    *out=p;out->y+=(d==4?height-1:1-height);if(bad_exit)++out->x;return 1;
}
uint8_t can_set(void*,HatchFace f,HatchFace* out,void*){
    *out={{f.cell.x,f.cell.y+height-1,f.cell.z},5};return !deny_set;
}
#include "modloader/src/base_corridor_registry.inc"
struct Record {bool used=false,verified=false;uint32_t origin=0;char object_id[128]{};};
std::map<std::string,Record>records;
Record* build_corridor_record(const WorldEvent& e,bool allocate){
    auto i=records.find(e.object_id);return i==records.end()?(allocate?&records[e.object_id]:nullptr):&i->second;
}
void set_face(void*,HatchFace f,uint8_t type,void*){
    ++writes;if(throw_write)throw std::runtime_error("native exception");
    if(!bad_readback)faces[key(f)]=type;
}
void clear_face(void*,HatchFace f,uint8_t type,void*){
    assert(type==5&&f.direction==4);++clears;
    // Native deconstructFaceTypes[Ladder] is Solid(1), proven by metadata hash.
    for(int y=0;y<height;++y)
        faces[{f.cell.x,f.cell.y+y,f.cell.z,y==0?4:y==height-1?5:0}]=1;
    if(bad_readback)faces[{f.cell.x,f.cell.y+1,f.cell.z,0}]=5;
}
void rebuild(void*,void*){++rebuilds;}
void fix_links(void*,void*){
    ++rebuilds;
    // Native FixCorridorLinksForCell2880a8c..2880b34 normalizes a Solid
    // endpoint shared with a corridor/connector to Hole(9). Ladder(5) stays.
    for(auto f:{HatchFace{{2,3,4},4},HatchFace{{2,3+height-1,4},5}})
        if(faces[key(f)]==1)faces[key(f)]=9;
}
bool build_resolve_hatch_apply(){return true;}
bool build_hatch_removal_allowed(void*,HatchFace f,int tech){assert(tech==5504);return permission&&f.direction==5;}
struct {Entry<decltype(&set_face)>set_face{&::set_face};Entry<decltype(&clear_face)>clear_face{&::clear_face};}g_hatch_apply;
struct {Entry<decltype(&rebuild)>fix_floors{&::rebuild},fix_links{&::fix_links},rebuild{&::rebuild};}g_corridor_removal_api;
int g_applying_remote=0;
bool build_map_base_geometry(void*,const base_geometry::GhostEnvelope& source,base_geometry::GhostEnvelope* out){return base_geometry::remap_target_anchor(source,{},out);}
#include "modloader/src/base_ladder_apply.inc"
void reset(int h=3){
    height=h;capturing=false;permission=true;bad_exit=deny_set=bad_readback=throw_write=false;
    missing_link=false;g_ladder_apply_failed=false;writes=clears=rebuilds=0;g_applying_remote=0;
    for(auto& e:g_synced_corridors)e=SyncedCorridor{};records.clear();faces.clear();
    faces[{2,3,4,4}]=1;faces[{2,3+height-1,4,5}]=1;
}
WorldEvent event(const char* id="ladder"){
    WorldEvent e{};e.tech_type=5504;e.kind=kWorldEventBuild;e.origin_id=7;
    e.build_flags=kWorldBuildComplete|kWorldBuildHasGeometry|kWorldBuildAnchored;
    e.geometry_hash_low=11;e.geometry_hash_high=22;snprintf(e.object_id,sizeof(e.object_id),"%s",id);return e;
}
base_geometry::GhostEnvelope geometry(const WorldEvent& e){
    base_geometry::GhostEnvelope g;g.object_id=e.object_id;g.target_base_id="shared-base";
    return base_geometry::ladder_descriptor(g,{2,3,4},height);
}
int main(){
    g_ladder_capture.ready=true;g_ladder_capture.get_cell.call=&cell;
    g_ladder_capture.exit_cell.call=&exit_cell;g_ladder_capture.can_set.call=&can_set;
    reset();auto shifted=event("shifted-ladder");auto old_frame=geometry(shifted);
    old_frame.target_offset[0]-=4;old_frame.target_base_anchor[0]-=4;
    assert(build_apply_ladder(&target_token,shifted,old_frame)&&writes==height);
    shifted.kind=kWorldEventRemove;
    assert(build_apply_ladder(&target_token,shifted,old_frame)&&clears==1);
    assert(build_synced_corridor(shifted.object_id)->geometry.target_offset==old_frame.target_offset);
    // Actual capture: both picked ends normalize to one canonical identity.
    for(height=2;height<=16;++height)for(int d:{4,5}){
        first_direction=d;
        capturing=true;base_geometry::GhostEnvelope g;g.object_id="original";
        assert(build_capture_ladder(&component_token,&g));assert(base_geometry::ladder_height(g)==height);
        assert((g.target_offset==std::array<int32_t,3>{2,3,4}));
        const auto original=g;
        generic_ghost=true;
        assert(!build_capture_ladder(&component_token,&g));
        assert(build_capture_ladder(&component_token,&g,true));
        assert(base_geometry::same_ladder(original,g));
        wrong_class=true;assert(!build_capture_ladder(&component_token,&g,true));wrong_class=false;
        missing_link=true;assert(!build_capture_ladder(&component_token,&g,true));missing_link=false;
        generic_ghost=false;
        missing_link=true;assert(!build_capture_ladder(&component_token,&g));missing_link=false;
        wrong_class=true;assert(!build_capture_ladder(&component_token,&g));wrong_class=false;
        no_target=true;assert(!build_capture_ladder(&component_token,&g));no_target=false;
        bad_exit=true;assert(!build_capture_ladder(&component_token,&g));bad_exit=false;
    }
    for(int h=2;h<=16;++h){
        reset(h);auto e=event();auto g=geometry(e);
        std::vector<uint8_t>data;assert(base_geometry::encode_envelope(g,&data));
        base_geometry::GhostEnvelope decoded;assert(base_geometry::decode_envelope(data.data(),data.size(),&decoded));
        assert(base_geometry::same_ladder(g,decoded));
        assert(build_apply_ladder(&target_token,e,g));assert(writes==h&&rebuilds==3&&records.at("ladder").verified);
        assert(!build_synced_cell(g)); // face topology cannot masquerade as a cell
        assert(build_apply_ladder(&target_token,e,g)&&writes==h); // repair idempotence
        e.kind=kWorldEventRemove;build_synced_corridor(e.object_id)->removed=true;
        permission=false;assert(!build_apply_ladder(&target_token,e,g)&&clears==0);
        permission=true;assert(build_apply_ladder(&target_token,e,g)&&clears==1&&rebuilds==6);
        assert(build_ladder_readback(&target_token,g,h,true));
        assert(!g_ladder_apply_failed);
        e.kind=kWorldEventBuild;assert(build_apply_ladder(&target_token,e,g)&&writes==h);
    }
    reset();auto e=event();auto g=geometry(e);
    // Native ladder from disk; discard temporary session records as on reload.
    assert(build_apply_ladder(&target_token,e,g));
    for(auto& entry:g_synced_corridors)entry=SyncedCorridor{};
    records.clear();e.kind=kWorldEventRemove;
    build_synced_corridor(e.object_id,true)->removed=true;
    permission=false;
    assert(!build_apply_ladder(&target_token,e,g)&&clears==0);
    assert(build_synced_corridor(e.object_id)->state.tech_type==0);
    permission=true;
    assert(build_apply_ladder(&target_token,e,g)&&clears==1);
    assert(build_corridor_removed(e.object_id));
    auto replacement=event("saved-replacement");
    assert(build_apply_ladder(&target_token,replacement,geometry(replacement)));
    assert(build_apply_ladder(&target_token,e,g)&&clears==1);
    reset();e=event();g=geometry(e);
    auto malformed=g;malformed.geometry.faces[6]=1;assert(!base_geometry::ladder_height(malformed));
    malformed=g;malformed.geometry.masks[1]=16;assert(!base_geometry::ladder_height(malformed));
    malformed=g;malformed.geometry.cells[0]=4;assert(!base_geometry::ladder_height(malformed));
    malformed=g;malformed.target_offset[1]=8192;assert(!base_geometry::ladder_height(malformed));
    deny_set=true;assert(!build_apply_ladder(&target_token,e,g)&&writes==0);deny_set=false;
    bad_exit=true;assert(!build_apply_ladder(&target_token,e,g)&&writes==0);bad_exit=false;
    missing_link=true;assert(!build_apply_ladder(&target_token,e,g)&&writes==0);missing_link=false;
    faces[{2,4,4,0}]=3;assert(build_apply_ladder(&target_token,e,g)&&writes==0);faces[{2,4,4,0}]=0;
    assert(build_apply_ladder(&target_token,e,g));e.kind=kWorldEventRemove;
    ++e.geometry_hash_low;assert(build_apply_ladder(&target_token,e,g)&&clears==0);--e.geometry_hash_low;
    auto a=event("replacement");build_remember_corridor(a,geometry(a));
    assert(build_apply_ladder(&target_token,e,g)&&clears==0);
    reset();e=event();g=geometry(e);bad_readback=true;
    assert(build_apply_ladder(&target_token,e,g)&&g_ladder_apply_failed&&!records.at("ladder").verified);
    reset();e=event();g=geometry(e);throw_write=true;g_applying_remote=2;
    assert(build_apply_ladder(&target_token,e,g)&&g_ladder_apply_failed&&g_applying_remote==2);
    reset();e=event();g=geometry(e);assert(build_apply_ladder(&target_token,e,g));
    e.kind=kWorldEventRemove;bad_readback=true;
    assert(build_apply_ladder(&target_token,e,g)&&clears==1&&g_ladder_apply_failed);
    auto hatch=base_geometry::face_descriptor(g,{2,3,4},4,5501);
    assert(base_geometry::ladder_faces_overlap(g,hatch));
    auto next=base_geometry::ladder_descriptor(g,{2,5,4},3);
    assert(!base_geometry::ladder_faces_overlap(g,next)); // adjacent floors share a cell, different faces
    // Demolish, rebuild the SAME shaft with a NEW ID, then deliver delayed
    // build/removal packets for the previous ladder while its replacement exists.
    reset();auto old=event("old");auto old_geometry=geometry(old);
    for (int cycle=0;cycle<3;++cycle) {
        char id[32];snprintf(id,sizeof(id),"cycle-%d",cycle);
        auto fresh=event(id);auto fresh_geometry=geometry(fresh);
        assert(build_apply_ladder(&target_token,fresh,fresh_geometry));
        assert(records.at(id).verified && build_synced_ladder(fresh_geometry));
        if(cycle>0) {
            const int prior_writes=writes,prior_clears=clears;
            old.kind=kWorldEventBuild;assert(build_apply_ladder(&target_token,old,old_geometry));
            old.kind=kWorldEventRemove;assert(build_apply_ladder(&target_token,old,old_geometry));
            assert(writes==prior_writes && clears==prior_clears);
            assert(build_ladder_readback(&target_token,fresh_geometry,height,false));
        }
        fresh.kind=kWorldEventRemove;build_synced_corridor(id)->removed=true;
        assert(build_apply_ladder(&target_token,fresh,fresh_geometry));
        assert(build_ladder_readback(&target_token,fresh_geometry,height,true));
        assert(!g_ladder_apply_failed);
        old=fresh;old_geometry=fresh_geometry;
    }
    // Hole(9) is valid only at normalized endpoints. Intermediate markers
    // and unrelated faces still keep the stricter type checks.
    reset();e=event();g=geometry(e);assert(build_apply_ladder(&target_token,e,g));
    faces[{99,3,4,4}]=5;
    e.kind=kWorldEventRemove;assert(build_apply_ladder(&target_token,e,g));
    assert(!g_ladder_apply_failed && faces.at(Key{99,3,4,4})==5);
    faces[{2,4,4,0}]=9;assert(!build_ladder_readback(&target_token,g,height,true));
    faces[{2,4,4,0}]=3;
    assert(!build_ladder_readback(&target_token,g,height,true));
    puts("actual ladder capture/apply: native ClearFace Solid(1) plus FixCorridorLinks Hole(9) endpoint regression; generic BaseGhost demolition; three cycles with delayed packets; both ends, heights2..16, unrelated faces preserved, codec, permissions, readback and exceptions passed");
}
