#define SNMOD_BASE_GEOMETRY_CAPTURE
#include "modloader/src/base_hatch.h"
#include "modloader/src/bioreactor_state.h"
#include <cassert>

int main() {
    using namespace base_geometry;
    GhostEnvelope owner;
    owner.object_id="reactor-one"; owner.target_base_id="exact-base";owner.target_base_anchor={2,0,1};
    auto e=face_descriptor(owner,{3,1,4},2,kBioreactorTech);
    assert(face_descriptor_tech(e)==kBioreactorTech&&face_direction(e,kBioreactorTech)==2);
    assert(e.geometry.faces[2]==14&&face_direction(e,5505)<0);
    auto bad=e;bad.geometry.faces[3]=14;assert(face_direction(bad,kBioreactorTech)<0);
    bad=e;bad.target_base_id.clear();assert(face_direction(bad,kBioreactorTech)<0);
    bad=e;bad.creates_base=true;assert(face_direction(bad,kBioreactorTech)<0);
    GhostEnvelope moved;
    assert(remap_target_anchor(e,{3,0,1},&moved)&&same_structural_face(e,moved));
    assert((moved.target_offset==std::array<int32_t,3>{4,1,4}));
    moved.target_base_id="other-base";assert(!same_structural_face(e,moved));
    // Loaded fuel and an unreadable container both defer remote demolition; a newly created
    // reactor (no container yet) is empty by construction and may be removed.
    for(int fuel:{-5,-1,0,1,2,64,2147483647})for(bool present:{false,true}) {
        bioreactor_sync::State s{present,fuel};
        const bool expected=present&&fuel==0;
        assert(bioreactor_sync::removal_allowed(s)==expected);
        assert((bioreactor_sync::removal_block(s)==nullptr)==expected);
    }
    assert(std::strcmp(bioreactor_sync::removal_block({true,-1}),"fuel-unreadable")==0);
    assert(std::strcmp(bioreactor_sync::removal_block({true,3}),"fuel-loaded")==0);
    assert(std::strcmp(bioreactor_sync::removal_block({false,0}),"no-bi-reactor")==0);
    puts("PASS: bioreactor exact base and face, anchor remapping and fuel demolition guard");
}
