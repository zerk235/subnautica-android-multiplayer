#define SNMOD_BASE_GEOMETRY_CAPTURE 1
#include "modloader/src/net.h"
#include "modloader/src/base_geometry_corridor.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include "modloader/src/base_corridor_registry.inc"

int main() {
    using namespace base_geometry;
    GhostEnvelope first;first.object_id="first";first.target_base_id="new-base";
    first.ghost_class="BaseAddCellGhost";
    first.geometry.shape={2,1,2};first.geometry.cells={2,3,3,3};
    first.geometry.links={0,32,1,33};first.geometry.faces.assign(24,1);
    first.geometry.glass.assign(4,0);
    auto left=first;left.object_id="left";left.target_offset={-2,0,0};
    GhostEnvelope shifted_first,shifted_left;
    assert(remap_target_anchor(first,{2,0,0},&shifted_first));
    assert(remap_target_anchor(left,{2,0,0},&shifted_left));
    assert((shifted_first.target_offset==std::array<int32_t,3>{2,0,0}));
    assert((shifted_left.target_offset==std::array<int32_t,3>{0,0,0}));
    assert(same_corridor_cell(first,shifted_first)&&same_corridor_cell(left,shifted_left));
    assert(!piece_footprints_overlap(first,left));
    assert(!piece_footprints_overlap(first,shifted_left));
    assert(piece_footprints_overlap(first,shifted_first));
    // Remapping is a local view; source bytes/fingerprint remain immutable.
    std::vector<uint8_t> before,after;
    assert(encode_envelope(left,&before));
    GhostEnvelope twice;
    assert(remap_target_anchor(shifted_left,{5,1,3},&twice));
    assert(same_corridor_cell(twice,left));
    assert(encode_envelope(left,&after)&&before==after);
    WorldEvent state{};snprintf(state.object_id,sizeof(state.object_id),"%s",left.object_id.c_str());
    state.tech_type=5509;state.kind=kWorldEventBuild;
    build_remember_corridor(state,left);
    assert(build_synced_cell(shifted_left)==build_synced_corridor("left"));
    auto replacement=state;snprintf(replacement.object_id,sizeof(replacement.object_id),"replacement");
    auto other=shifted_left;other.object_id=replacement.object_id;
    build_remember_corridor(replacement,other);
    assert(build_cell_has_replacement(left,"left"));
    other.target_base_id="foreign";assert(!same_corridor_cell(other,left));
    other=shifted_left;other.target_offset[0]++;assert(!same_corridor_cell(other,left));
    const auto face=face_descriptor(first,{-2,0,0},2,5505);
    GhostEnvelope moved_face;
    assert(remap_target_anchor(face,{2,0,0},&moved_face));
    assert(same_structural_face(face,moved_face));
    auto ladder=ladder_descriptor(first,{-2,0,0},3);
    GhostEnvelope moved_ladder;
    assert(remap_target_anchor(ladder,{2,0,0},&moved_ladder));
    assert(same_ladder(ladder,moved_ladder)&&ladder_faces_overlap(ladder,moved_ladder));
    auto endpoint=face_descriptor(first,{-2,0,0},4,5505);
    assert(ladder_faces_overlap(moved_ladder,endpoint));
    auto invalid=left;invalid.target_offset[0]=INT32_MIN;
    assert(!remap_target_anchor(invalid,{2,0,0},&twice));
    invalid=left;invalid.target_base_anchor[0]=-8192;invalid.target_offset[0]=8192;
    assert(!remap_target_anchor(invalid,{1,0,0},&twice));
    assert(!remap_target_anchor(left,{8193,0,0},&twice));
    assert(!remap_target_anchor(left,{0,0,0},nullptr));
    puts("PASS: negative expansion remaps cells/footprints, demolition identity, replacements, faces and ladders without changing wire snapshots; overflow rejected");
}
