#define SNMOD_BASE_GEOMETRY_CAPTURE 1
#include "modloader/src/net.h"
#include "modloader/src/base_geometry_corridor.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
template<class Fn> struct Entry {
    Fn call=nullptr; void* method=nullptr;
    explicit operator bool() const { return call!=nullptr; }
    void bind(void*) {}
};
struct Il2CppObject {};
struct HatchPoint { int32_t x,y,z; };
struct BuildGridPoint { int32_t x,y,z; };
struct HatchFace { HatchPoint cell; int32_t direction; };
int target_token,foreign_token,recipe_field,face_field;
struct Candidate { int32_t recipe=5501; HatchFace face{{2,0,3},0}; void* base=&target_token; bool allowed=true; } candidate;
std::vector<Candidate*> candidates{&candidate};
bool build_live(void* p) { return p!=nullptr; }
void il2cpp_field_get_value(Il2CppObject* obj,void* field,void* out) {
    auto* c=reinterpret_cast<Candidate*>(obj);
    if (field==&recipe_field) memcpy(out,&c->recipe,4);
    else { assert(field==&face_field); memset(out,0,20); *static_cast<uint8_t*>(out)=1; memcpy(static_cast<char*>(out)+4,&c->face,16); }
}
void* all(void*,bool inactive,void*) { assert(inactive);return &candidates; }
Entry<decltype(&all)> g_find_objects_all{&all};
int32_t count(void* p,void*) { return static_cast<std::vector<Candidate*>*>(p)->size(); }
Entry<decltype(&count)> g_array_get_length{&count};
void* item(void* p,int32_t i,void*) { return static_cast<std::vector<Candidate*>*>(p)->at(i); }
Entry<decltype(&item)> g_array_get_value{&item};
void* identity(void* p,void*) { return p; }
Entry<decltype(&identity)> g_get_game_object{&identity};
void* component(void* p,void*,void*) { return static_cast<Candidate*>(p)->base; }
Entry<decltype(&component)> g_get_component{&component};
void* g_build_base_geometry_type=&target_token;
template<class Fn> void walk_parents(void* go,Fn fn) { fn(go); }
uint8_t allowed(void* p,void**,void*) { return static_cast<Candidate*>(p)->allowed; }
unsigned writes=0,rebuilds=0;
uint8_t current=1;
bool may_set=true,bad_readback=false,throw_native=false,bad_roof_clear=false;
uint8_t face(void* target,HatchFace,void*) { assert(target==&target_token);return current; }
void rebuild(void* target,void*) { assert(target==&target_token);++rebuilds; }
struct {
    void *type=&candidate,*recipe=&recipe_field,*face=&face_field;
    Entry<decltype(&allowed)> allowed{&::allowed};
    Entry<decltype(&rebuild)> fix_floors{&::rebuild},fix_links{&::rebuild},rebuild{&::rebuild};
} g_corridor_removal_api;
uint8_t room_cells[9]{1,3,3,3,3,3,3,3,3};
uint8_t raw_room(void* target,BuildGridPoint p,void*) {
    assert(target==&target_token&&p.y==0&&p.x>=2&&p.x<=4&&p.z>=3&&p.z<=5);
    return room_cells[(p.x-2)+3*(p.z-3)];
}
struct { void* base=&target_token; Entry<decltype(&raw_room)> get_raw_cell{&raw_room}; } g_corridor_api;
struct { Entry<decltype(&face)> get_face{&face}; } g_hatch_capture;
bool build_resolve_hatch_capture() { return true; }
bool build_resolve_corridor_removal() { return true; }
void* find_method(void*,const char*,int) { return nullptr; }
#include "modloader/src/base_corridor_registry.inc"
struct Record { bool used=false,verified=false; uint32_t origin=0; char object_id[128]{}; };
std::map<std::string,Record> records;
Record* build_corridor_record(const WorldEvent& e,bool allocate) {
    auto i=records.find(e.object_id);
    return i!=records.end() ? &i->second : allocate ? &records[e.object_id] : nullptr;
}
int g_applying_remote=0;
#include "modloader/src/base_hatch_apply.inc"
void set_face(void*,HatchFace,uint8_t type,void*) {
    ++writes;
    if (throw_native) throw std::runtime_error("mock native failure");
    if (!bad_readback) current=type;
}
void clear_face(void*,HatchFace,uint8_t type,void*) { assert(current==type);++writes;current=bad_roof_clear ? 0 : 1; }
uint8_t can_set(void*,HatchFace,uint8_t,void*) { return may_set; }
void reset() {
    for (auto& entry:g_synced_corridors) entry=SyncedCorridor{};
    records.clear();candidate=Candidate{};candidates={&candidate};
    writes=rebuilds=0;current=1;may_set=true;bad_readback=throw_native=false;g_applying_remote=0;
    bad_roof_clear=false;
    g_hatch_apply=HatchApplyApi{};g_hatch_apply.ready=true;
    g_hatch_apply.set_face.call=&set_face;g_hatch_apply.clear_face.call=&clear_face;g_hatch_apply.can_set.call=&can_set;
}
WorldEvent event(int tech,const char* id="piece") {
    WorldEvent e{};e.tech_type=tech;e.kind=kWorldEventBuild;e.origin_id=7;
    e.build_flags=kWorldBuildComplete|kWorldBuildHasGeometry|kWorldBuildAnchored;
    e.geometry_hash_low=11;e.geometry_hash_high=22;snprintf(e.object_id,sizeof(e.object_id),"%s",id);return e;
}
base_geometry::GhostEnvelope geometry(const WorldEvent& e,int d) {
    base_geometry::GhostEnvelope g;g.object_id=e.object_id;g.target_base_id="shared-base";
    return base_geometry::face_descriptor(g,{2,0,3},d,e.tech_type);
}
int main() {
    for (int tech:{5501,5505,5514,5540}) for (int direction=0;direction<6;++direction) {
        if (tech==5540 && direction!=4) continue;
        reset();auto e=event(tech);auto g=geometry(e,direction);
        candidate.recipe=tech;candidate.face.direction=direction;
        assert(build_apply_hatch(&target_token,e,g));
        assert(current==base_geometry::face_type_for_tech(tech) && writes==1 && rebuilds==3);
        assert(records.at(e.object_id).verified && g_applying_remote==0);
        assert(build_apply_hatch(&target_token,e,g) && writes==1); // repair idempotence
        e.kind=kWorldEventRemove;build_synced_corridor(e.object_id)->removed=true; // real queue tombstone
        candidate.recipe=5515;
        assert(!build_apply_hatch(&target_token,e,g) && writes==1); // wrong native recipe
        candidate.recipe=tech;candidate.base=&foreign_token;
        assert(!build_apply_hatch(&target_token,e,g) && writes==1);
        candidate.base=&target_token;candidate.allowed=false;
        assert(!build_apply_hatch(&target_token,e,g) && writes==1);
        candidate.allowed=true;
        assert(build_apply_hatch(&target_token,e,g) && current==1 && writes==2 && rebuilds==6);
        e.kind=kWorldEventBuild;assert(build_apply_hatch(&target_token,e,g) && current==1 && writes==2);
    }
    reset();auto e=event(5505);auto g=geometry(e,0);
    may_set=false;assert(!build_apply_hatch(&target_token,e,g) && writes==0);
    may_set=true;current=6;assert(build_apply_hatch(&target_token,e,g) && writes==0 && current==6);
    current=1;auto malformed=g;malformed.geometry.faces[0]=3;
    assert(build_apply_hatch(&target_token,e,malformed) && writes==0);
    assert(build_apply_hatch(&target_token,e,g));
    e.kind=kWorldEventRemove;build_synced_corridor(e.object_id)->removed=true;
    auto a=event(5505,"new-a"),b=event(5505,"new-b");
    build_remember_corridor(a,geometry(a,0));build_remember_corridor(b,geometry(b,0));
    assert(!build_synced_face(g)); // two competing replacements
    assert(build_apply_hatch(&target_token,e,g) && writes==1 && current==2);
    reset();e=event(5514);g=geometry(e,0);bad_readback=true;
    assert(build_apply_hatch(&target_token,e,g) && g_hatch_apply.failed && !records.at(e.object_id).verified);
    auto next=event(5501,"next");assert(build_apply_hatch(&target_token,next,geometry(next,0)) && writes==1);
    reset();e=event(5501);g=geometry(e,0);throw_native=true;g_applying_remote=2;
    assert(build_apply_hatch(&target_token,e,g) && g_hatch_apply.failed && g_applying_remote==2);
    reset();e=event(5540);g=geometry(e,4);candidate.recipe=5540;candidate.face.direction=4;
    for(int i=0;i<9;++i){const auto old=room_cells[i];room_cells[i]=0;assert(!build_apply_hatch(&target_token,e,g)&&writes==0);room_cells[i]=old;}
    assert(build_apply_hatch(&target_token,e,geometry(e,0))&&writes==0);
    current=0;assert(build_apply_hatch(&target_token,e,g)&&writes==0);current=1;
    may_set=false;assert(!build_apply_hatch(&target_token,e,g)&&writes==0);may_set=true;
    WorldEvent previous{};base_geometry::GhostEnvelope previous_geometry;
    for(int cycle=0;cycle<3;++cycle){
        char id[32];snprintf(id,sizeof(id),"roof-%d",cycle);e=event(5540,id);g=geometry(e,4);
        assert(build_apply_hatch(&target_token,e,g)&&current==18);
        const auto before=writes;
        if(cycle)assert(build_apply_hatch(&target_token,previous,previous_geometry)&&writes==before&&current==18);
        assert(build_apply_hatch(&target_token,e,g)&&writes==before);
        e.kind=kWorldEventRemove;build_synced_corridor(e.object_id)->removed=true;
        assert(build_apply_hatch(&target_token,e,g)&&current==1&&!g_hatch_apply.failed);
        previous=e;previous_geometry=g;
    }
    assert(writes==6);for(int i=0;i<9;++i)assert(room_cells[i]==(i?3:1));
    reset();e=event(5540);g=geometry(e,4);candidate.recipe=5540;candidate.face.direction=4;
    auto bad_bounds=g;bad_bounds.target_offset[0]=8191;assert(build_apply_hatch(&target_token,e,bad_bounds)&&writes==0);
    bad_bounds=g;bad_bounds.target_offset[1]=8192;assert(build_apply_hatch(&target_token,e,bad_bounds)&&writes==0);
    assert(build_apply_hatch(&target_token,e,g)&&current==18);
    e.kind=kWorldEventRemove;build_synced_corridor(e.object_id)->removed=true;bad_roof_clear=true;
    assert(build_apply_hatch(&target_token,e,g)&&current==0&&g_hatch_apply.failed); // must restore Solid1, not Empty0
    puts("actual structural face apply: old3 recipes regression, dome Above-only/full9-cell room guard, native permission, three roof cycles, Solid roof restoration, delayed removals, repair and exception containment passed");
}
