// Exercise the actual hatch capture implementation without any topology arrays.
#include "modloader/src/base_hatch.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <utility>
#define LOG_INFO(...) ((void)0)
struct Vector3 { float x,y,z; };
struct Il2CppObject {};
template<class Fn> struct Entry {
    Fn call=nullptr;
    void* method=nullptr;
    void bind(void*) {}
    explicit operator bool() const { return call!=nullptr; }
};
constexpr size_t kWorldEventObjectIdSize=128;
int component_token,model_token,ghost_token,base_token,target_token;
int model_field,base_field,target_field,anchor_field;
struct {
    void* model=&model_field;
    void* ghost_base=&base_field;
    void* target=&target_field;
    void* anchor=&anchor_field;
    void* ghost_type=nullptr;
} g_ghost_capture;
bool target_live=true,identity_valid=true;
unsigned field_reads=0;
bool build_live(void* p) { return p && (p!=&target_token || target_live); }
void il2cpp_field_get_value(Il2CppObject* object,void* field,void* result) {
    ++field_reads;
    if (field==&model_field) {
        assert(object==reinterpret_cast<Il2CppObject*>(&component_token));
        *static_cast<void**>(result)=&model_token;
    } else if (field==&base_field) {
        assert(object==reinterpret_cast<Il2CppObject*>(&ghost_token));
        *static_cast<void**>(result)=&base_token;
    } else if (field==&target_field) {
        assert(object==reinterpret_cast<Il2CppObject*>(&ghost_token));
        *static_cast<void**>(result)=&target_token;
    } else {
        assert(field==&anchor_field); // Any shape/face/array read is a regression.
        assert(object==reinterpret_cast<Il2CppObject*>(&target_token));
        const int32_t anchor[3]={-1,2,3};
        std::memcpy(result,anchor,sizeof(anchor));
    }
}
bool build_resolve_ghost_capture() { return true; }
void* build_class(const char*) { return nullptr; }
void* find_method(void*,const char*,int) { return nullptr; }
void* build_typed_method(void*,const char*,int,const char*) { return nullptr; }
void* component(void* model,void*,void*) { assert(model==&model_token); return &ghost_token; }
Entry<decltype(&component)> g_get_component{&component,nullptr};
void* build_uid(void* target) { assert(target==&target_token); return identity_valid ? target : nullptr; }
void* unique_id(void* target,void*) { return target; }
Entry<decltype(&unique_id)> g_unique_get_id{&unique_id,nullptr};
bool string_to_ascii(void* value,char* out,size_t size) {
    assert(value==&target_token && size>14);
    std::strcpy(out,"existing-base"); return true;
}
#include "modloader/src/base_hatch_capture.inc"
int direction=0;
uint8_t face_type=3;
int32_t target_x=7;
uint8_t find_face(void* base,HatchFace* out,void*) {
    assert(base==&base_token);
    *out={{1,0,2},direction}; return 1;
}
Vector3 grid_world(void* base,HatchPoint cell,void*) {
    assert(base==&base_token && cell.x==1 && cell.z==2);
    return {12,3,24};
}
HatchPoint world_grid(void* target,Vector3 world,void*) {
    assert(target==&target_token && world.x==12 && world.z==24);
    return {target_x,0,-4};
}
uint8_t get_face(void* base,HatchFace face,void*) {
    assert(base==&base_token && face.direction==direction); return face_type;
}
int main() {
    g_hatch_capture.ready=true;
    g_hatch_capture.find_face.call=&find_face;
    g_hatch_capture.grid_world.call=&grid_world;
    g_hatch_capture.world_grid.call=&world_grid;
    g_hatch_capture.get_face.call=&get_face;
    for (direction=0;direction<6;++direction) {
        base_geometry::GhostEnvelope e;
        e.object_id="original-id";
        e.target_base_id="stale"; e.target_base_anchor={8,8,8};
        // The room's oversized backing arrays are deliberately unreadable to the API.
        e.geometry.shape={3,1,3}; e.geometry.faces.assign(108,1);
        assert(!base_geometry::valid(e.geometry));
        field_reads=0;
        assert(build_capture_hatch(&component_token,&e));
        assert(field_reads==4 && e.object_id=="original-id" && e.target_base_id=="existing-base");
        assert((e.target_base_anchor==std::array<int32_t,3>{-1,2,3}));
        assert((e.target_offset==std::array<int32_t,3>{7,0,-4}));
        assert(base_geometry::hatch_direction(e,5501)==direction);
    }
    base_geometry::GhostEnvelope e;
    direction=0;
    target_live=false; assert(!build_capture_hatch(&component_token,&e)); target_live=true;
    identity_valid=false; assert(!build_capture_hatch(&component_token,&e)); identity_valid=true;
    face_type=1; assert(!build_capture_hatch(&component_token,&e)); face_type=3;
    direction=6; assert(!build_capture_hatch(&component_token,&e)); direction=0;
    target_x=8193; assert(!build_capture_hatch(&component_token,&e));
    target_x=7;
    for (int tech:{5505,5514}) {
        face_type=uint8_t(base_geometry::face_type_for_tech(tech));
        for (direction=0;direction<6;++direction) {
            base_geometry::GhostEnvelope face;
            assert(build_capture_hatch(&component_token,&face,tech));
            assert(base_geometry::face_direction(face,tech)==direction);
            assert(base_geometry::hatch_direction(face,5501)<0);
            assert(face.target_base_id=="existing-base");
        }
        direction=0; face_type=3;
        assert(!build_capture_hatch(&component_token,&e,tech));
    }
    assert(!build_capture_hatch(&component_token,&e,5515));
    face_type=18;
    for(direction=0;direction<6;++direction){
        base_geometry::GhostEnvelope dome;
        assert(build_capture_hatch(&component_token,&dome,5540)==(direction==4));
        if(direction==4)assert(base_geometry::face_direction(dome,5540)==4);
    }
    puts("actual structural face capture: hatch/window/reinforcement checks; actual hatch capture: room-array independence, six faces and exact target identity passed");
}
