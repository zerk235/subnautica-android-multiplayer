#define SNMOD_BASE_GEOMETRY_CAPTURE 1
#include "modloader/src/net.h"
#include "modloader/src/base_geometry_corridor.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
template<class Fn> struct Entry { Fn call=nullptr;void* method=nullptr;explicit operator bool() const{return call!=nullptr;} };
struct Il2CppObject {};
struct BuildGridPoint {int32_t x,y,z;};
struct ByteArray {uint8_t header[32]{};uint8_t data[2]{6,4};} cell_array;
static_assert(offsetof(ByteArray,data)==32,"managed array data ABI");
int target_token,foreign_token,cells_field,bounds_field,face_field,recipe_field;
struct Candidate {int32_t bounds[6]{4,1,3,4,1,3};int32_t recipe=5519;bool has_face=false,allowed=true;void* base=&target_token;} candidate;
std::vector<Candidate*> candidates{&candidate};
uint8_t current=6;bool glass_ok=true,throw_clear=false,corrupt_clear=false;
unsigned writes=0,rebuilds=0;
bool build_live(void* p){return p!=nullptr;}
void il2cpp_field_get_value(Il2CppObject* obj,void* f,void* out){
    if(f==&cells_field){assert(obj==reinterpret_cast<Il2CppObject*>(&target_token));*static_cast<void**>(out)=&cell_array;return;}
    auto* c=reinterpret_cast<Candidate*>(obj);
    if(f==&bounds_field)memcpy(out,c->bounds,24);
    else if(f==&recipe_field)memcpy(out,&c->recipe,4);
    else {assert(f==&face_field);memset(out,0,20);*static_cast<uint8_t*>(out)=c->has_face;}
}
struct {void* cells=&cells_field;} g_ghost_capture;
int32_t length(void* p,void*){return p==&cell_array?2:int32_t(candidates.size());}
Entry<decltype(&length)> g_array_get_length{&length};
void* all(void*,bool inactive,void*){assert(inactive);return &candidates;}
Entry<decltype(&all)> g_find_objects_all{&all};
void* value(void*,int32_t i,void*){return candidates.at(i);}
Entry<decltype(&value)> g_array_get_value{&value};
void* object(void* p,void*){return p;}
Entry<decltype(&object)> g_get_game_object{&object};
void* component(void* p,void*,void*){return static_cast<Candidate*>(p)->base;}
Entry<decltype(&component)> g_get_component{&component};
void* g_build_base_geometry_type=&target_token;
template<class Fn>void walk_parents(void* go,Fn fn){fn(go);}
uint8_t get_cell(void* t,BuildGridPoint p,void*){assert(t==&target_token && p.x==4 && p.y==1 && p.z==3);return current;}
struct {Entry<decltype(&get_cell)> get_cell{&::get_cell};} g_corridor_api;
bool build_corridor_glass_matches(void*,BuildGridPoint,int32_t){return glass_ok;}
void clear(void*,BuildGridPoint,void*){++writes;if(throw_clear)throw std::runtime_error("native");if(!corrupt_clear)current=0;}
void rebuild(void*,void*){++rebuilds;}
uint8_t allowed(void* p,void**,void*){return static_cast<Candidate*>(p)->allowed;}
struct {
    void* type=&candidate,*bounds=&bounds_field,*face=&face_field,*recipe=&recipe_field;
    Entry<decltype(&clear)> clear_cell{&clear};
    Entry<decltype(&rebuild)> fix_floors{&::rebuild},fix_links{&::rebuild},rebuild{&::rebuild};
    Entry<decltype(&allowed)> allowed{&::allowed};
} g_corridor_removal_api;
bool build_resolve_corridor_removal(){return true;}
bool g_corridor_removal_failed=false;int g_applying_remote=0;
#include "modloader/src/base_corridor_registry.inc"

bool foundation_intact=true,other_root=true,foundation_clear_verified=true;
bool build_foundation_footprint_matches(void*,const base_geometry::GhostEnvelope&,bool removed){
    return removed ? current==0 && foundation_clear_verified : current==2 && foundation_intact;
}
bool build_foundation_has_other_root(void*){return other_root;}
bool build_room_footprint_matches(void*,const base_geometry::GhostEnvelope&,bool removed){
    return removed ? current==0 && foundation_clear_verified : current==1 && foundation_intact;
}
bool build_apply_corridor_removal_inner(void* target, const WorldEvent& e,
                                        const base_geometry::GhostEnvelope& geometry) {
    if (g_corridor_removal_failed || e.kind != kWorldEventRemove || e.face_has_value ||
        !base_geometry::supported_existing_piece(geometry, e.tech_type) || geometry.object_id != e.object_id)
        return true;
    auto* identity = build_synced_corridor(e.object_id);
    if (!identity || identity->geometry.object_id != e.object_id || identity->state.tech_type != e.tech_type ||
        !base_geometry::same_corridor_cell(identity->geometry, geometry) ||
        identity->state.geometry_hash_low != e.geometry_hash_low ||
        identity->state.geometry_hash_high != e.geometry_hash_high) {
        LOG_WARN("build: corridor removal refused id=%s: no matching session placement", e.object_id);
        return true;
    }
    if (build_cell_has_replacement(geometry,e.object_id)) return true;
    if (!build_resolve_corridor_removal()) return true;
    auto& a = g_corridor_removal_api;
    const BuildGridPoint cell{geometry.target_offset[0], geometry.target_offset[1], geometry.target_offset[2]};
    const uint8_t current = g_corridor_api.get_cell.call(target, cell, g_corridor_api.get_cell.method);
    if (current == 0) return true;
    if (current != base_geometry::piece_cell_type(e.tech_type)) { LOG_WARN("build: corridor removal refused: cell changed type id=%s", e.object_id); return true; }
    if (!build_corridor_glass_matches(target, cell, e.tech_type)) {
        LOG_WARN("build: corridor removal refused: glass changed id=%s", e.object_id);
        return true;
    }
    if (base_geometry::multi_cell_piece(e.tech_type)) {
        if (!(e.tech_type==5500 ? build_room_footprint_matches(target,geometry,false) :
              build_foundation_footprint_matches(target,geometry,false)) ||
            !build_foundation_has_other_root(target)) return true;
    } else {
    // Never remove the final cell/base through this first implementation.
    void* cells = nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(target), g_ghost_capture.cells, &cells);
    const int32_t length = cells ? g_array_get_length.call(cells, g_array_get_length.method) : 0;
    if (length <= 0 || length > 1048576) return true;
    int occupied = 0;
    auto* bytes = reinterpret_cast<uint8_t*>(cells) + 0x20;
    for (int32_t i=0; i<length && occupied<2; ++i) if (bytes[i]) ++occupied;
    if (occupied < 2) return true;
    }
    void* list = g_find_objects_all.call(a.type, true, g_find_objects_all.method);
    const int32_t count = list ? g_array_get_length.call(list, g_array_get_length.method) : 0;
    void* deconstructable = nullptr;
    for (int32_t i=0; i<count; ++i) {
        void* candidate = g_array_get_value.call(list, i, g_array_get_value.method);
        if (!build_live(candidate)) continue;
        int32_t bounds[6]{}, recipe=0;
        uint8_t face[20]{};
        auto* object = reinterpret_cast<Il2CppObject*>(candidate);
        il2cpp_field_get_value(object, a.bounds, bounds);
        il2cpp_field_get_value(object, a.recipe, &recipe);
        il2cpp_field_get_value(object, a.face, face);
        if (recipe != e.tech_type || face[0] ||
            !base_geometry::piece_deconstruction_bounds(geometry,e.tech_type,bounds)) continue;
        void* go = g_get_game_object.call(candidate, g_get_game_object.method);
        bool belongs = false;
        walk_parents(go, [&](void* node) {
            void* base = g_get_component.call(node, g_build_base_geometry_type, g_get_component.method);
            if (base != target) return false;
            belongs = true;
            return true;
        });
        if (!belongs) continue;
        if (deconstructable) { LOG_WARN("build: corridor removal ambiguous id=%s", e.object_id); return true; }
        deconstructable = candidate;
    }
    if (!deconstructable) { LOG_WARN("build: corridor removal waiting for exact cell handle id=%s", e.object_id); return false; }
    void* reason = nullptr;
    if (!a.allowed.call(deconstructable, &reason, a.allowed.method)) {
        LOG_WARN("build: corridor removal blocked by local game rules id=%s", e.object_id);
        return false;
    }
    const int prior_remote = g_applying_remote;
    const char* stage = "clear-cell";
    ++g_applying_remote;
    try {
        // Same single-cell and repair methods called by BaseDeconstructable.Deconstruct
        // (2A919E4, 2A91AF8, 2A91B04, 2A91B10), with no temporary resource ghost.
        a.clear_cell.call(target, cell, a.clear_cell.method);
        stage = "fix-floors"; a.fix_floors.call(target, a.fix_floors.method);
        stage = "fix-links"; a.fix_links.call(target, a.fix_links.method);
        stage = "rebuild"; a.rebuild.call(target, a.rebuild.method);
        const uint8_t actual = g_corridor_api.get_cell.call(target, cell, g_corridor_api.get_cell.method);
        const bool verified=actual==0 &&
            (e.tech_type!=5509 || build_foundation_footprint_matches(target,geometry,true)) &&
            (e.tech_type!=5500 || build_room_footprint_matches(target,geometry,true));
        LOG_INFO("build: corridor removed id=%s target_cell=%u verified=%u tech=%d", e.object_id, actual, unsigned(verified),e.tech_type);
        if (!verified) g_corridor_removal_failed = true;
    } catch (...) {
        g_corridor_removal_failed = true;
        LOG_WARN("build: corridor removal exception id=%s stage=%s; removal disabled until restart", e.object_id, stage);
    }
    g_applying_remote = prior_remote;
    return true; // once mutated, never repeat a partly applied removal
}

bool build_apply_corridor_removal(void* target, const WorldEvent& e,
                                  const base_geometry::GhostEnvelope& geometry) {
    const int prior_remote = g_applying_remote;
    try {
        const bool done = build_apply_corridor_removal_inner(target, e, geometry);
        g_applying_remote = prior_remote;
        return done;
    } catch (...) {
        g_applying_remote = prior_remote;
        g_corridor_removal_failed = true;
        LOG_WARN("build: corridor removal validation exception id=%s; removal disabled until restart", e.object_id);
        return true;
    }
}

void reset(){
    for(auto& e:g_synced_corridors)e=SyncedCorridor{};
    candidate=Candidate{};candidates={&candidate};cell_array.data[0]=6;cell_array.data[1]=4;
    current=6;glass_ok=true;throw_clear=corrupt_clear=false;writes=rebuilds=0;
    g_corridor_removal_failed=false;g_applying_remote=7;foundation_intact=other_root=foundation_clear_verified=true;
}
WorldEvent make_event(int tech=5519,const char* id="connector"){
    WorldEvent e{};e.kind=kWorldEventRemove;e.tech_type=tech;e.geometry_hash_low=11;e.geometry_hash_high=22;
    snprintf(e.object_id,sizeof(e.object_id),"%s",id);return e;
}
base_geometry::GhostEnvelope make_geometry(const WorldEvent& e){
    base_geometry::GhostEnvelope g;g.object_id=e.object_id;g.target_base_id="base";g.target_offset={4,1,3};
    g.ghost_class=base_geometry::piece_ghost(e.tech_type);
    g.geometry.shape={1,1,1};g.geometry.cells={uint8_t(base_geometry::piece_cell_type(e.tech_type))};
    g.geometry.faces.assign(6,1);g.geometry.links={0};g.geometry.glass={uint8_t(base_geometry::corridor_glass(e.tech_type))};
    if(e.tech_type==5509){g.geometry.shape={2,1,2};g.geometry.cells={2,3,3,3};g.geometry.links={0,32,1,33};g.geometry.glass={0,0,0,0};g.geometry.faces.assign(24,1);}
    if(e.tech_type==5500){g.geometry.shape={3,1,3};g.geometry.cells={1,3,3,3,3,3,3,3,3};g.geometry.links={0,32,64,1,33,65,2,34,66};g.geometry.glass.assign(9,0);g.geometry.faces.assign(54,1);}
    return g;
}
void remember(const WorldEvent& e,const base_geometry::GhostEnvelope& g){
    build_remember_corridor(e,g);build_synced_corridor(e.object_id)->removed=true;
}
int main(){
    for(int tech:{5519,5510,5516,5509,5500}){
        reset();auto e=make_event(tech);auto g=make_geometry(e);remember(e,g);
        candidate.recipe=tech;current=uint8_t(base_geometry::piece_cell_type(tech));
        if(tech==5509){candidate.bounds[3]++;candidate.bounds[5]++;}
        if(tech==5500){candidate.bounds[3]+=2;candidate.bounds[5]+=2;}
        assert(build_apply_corridor_removal(&target_token,e,g));
        assert(current==0 && writes==1 && rebuilds==3 && g_applying_remote==7 && !g_corridor_removal_failed);
    }
    reset();auto e=make_event();auto g=make_geometry(e);remember(e,g);
    cell_array.data[1]=0;
    assert(build_apply_corridor_removal(&target_token,e,g)&&writes==0); // final cell protected
    cell_array.data[1]=4;candidate.allowed=false;
    assert(!build_apply_corridor_removal(&target_token,e,g)&&writes==0);
    candidate.allowed=true;candidate.recipe=5509;
    assert(!build_apply_corridor_removal(&target_token,e,g)&&writes==0);
    candidate.recipe=5519;candidate.base=&foreign_token;
    assert(!build_apply_corridor_removal(&target_token,e,g)&&writes==0);
    candidate.base=&target_token;current=4;
    assert(build_apply_corridor_removal(&target_token,e,g)&&writes==0);
    current=6;glass_ok=false;assert(build_apply_corridor_removal(&target_token,e,g)&&writes==0);glass_ok=true;
    e.geometry_hash_low++;assert(build_apply_corridor_removal(&target_token,e,g)&&writes==0);e.geometry_hash_low--;
    auto a=make_event(5519,"replacement-a"),b=make_event(5519,"replacement-b");
    build_remember_corridor(a,make_geometry(a));build_remember_corridor(b,make_geometry(b));
    assert(!build_synced_cell(g)); // ambiguous replacements must also protect the cell
    assert(build_apply_corridor_removal(&target_token,e,g)&&writes==0);
    reset();remember(e,g);throw_clear=true;
    assert(build_apply_corridor_removal(&target_token,e,g)&&writes==1&&g_corridor_removal_failed&&g_applying_remote==7);
    assert(build_apply_corridor_removal(&target_token,e,g)&&writes==1);
    reset();remember(e,g);corrupt_clear=true;
    assert(build_apply_corridor_removal(&target_token,e,g)&&g_corridor_removal_failed&&g_applying_remote==7);
    reset();e=make_event(5509);g=make_geometry(e);remember(e,g);current=2;candidate.recipe=5509;
    foundation_intact=false;assert(build_apply_corridor_removal(&target_token,e,g)&&writes==0);
    foundation_intact=true;other_root=false;assert(build_apply_corridor_removal(&target_token,e,g)&&writes==0);
    other_root=true;assert(!build_apply_corridor_removal(&target_token,e,g)&&writes==0); // handle has wrong 1x1 bounds
    candidate.bounds[3]++;candidate.bounds[5]++;foundation_clear_verified=false;
    assert(build_apply_corridor_removal(&target_token,e,g)&&writes==1&&g_corridor_removal_failed); // raw quarter remained
    for(int cycle=0;cycle<2;++cycle){
        reset();e=make_event(5500);g=make_geometry(e);remember(e,g);current=1;candidate.recipe=5500;
        candidate.bounds[3]+=2;candidate.bounds[5]+=2;
        foundation_intact=false;assert(build_apply_corridor_removal(&target_token,e,g)&&writes==0);
        foundation_intact=true;other_root=false;assert(build_apply_corridor_removal(&target_token,e,g)&&writes==0);
        other_root=true;candidate.allowed=false;assert(!build_apply_corridor_removal(&target_token,e,g)&&writes==0);
        candidate.allowed=true;assert(build_apply_corridor_removal(&target_token,e,g)&&writes==1&&!g_corridor_removal_failed);
    }
    puts("actual room/foundation/corridor removal: exact bounds, footprint, last root, permission, identity, replacement, exception and readback guards passed");
}
