#pragma once
#include "base_geometry_envelope.h"
#include <utility>

namespace base_geometry {
// Android 1.22.83315 structural face recipes. Other face modules carry their
// own state or multi-face topology and must not enter this face path.
// Dome5540 is one normalized Room-root Above face (native BuildRoomGeometry),
// with an additional complete room footprint check at replay.
inline int face_type_for_tech(int32_t tech) {
    switch (tech) { case 5501:return 3; case 5505:return 2; case 5514:return 6; case 5540:return 18; default:return -1; }
}
inline const char* face_descriptor_class(int32_t tech) {
    switch (tech) { case 5501:return "BaseHatchFace"; case 5505:return "BaseWindowFace";
        case 5514:return "BaseReinforcementFace"; case 5540:return "BaseRoomDomeFace"; default:return "UnsupportedFace"; }
}
inline int face_descriptor_tech(const GhostEnvelope& e) {
    for (int tech:{5501,5505,5514,5540}) if (e.ghost_class==face_descriptor_class(tech)) return tech;
    return 0;
}
// Canonical descriptor: one target cell and exactly one face, never cell topology.
inline GhostEnvelope face_descriptor(GhostEnvelope e,std::array<int32_t,3> cell,int direction,int32_t tech) {
    e.ghost_class=face_descriptor_class(tech);
    e.target_offset=cell;
    e.geometry=Snapshot{};
    e.geometry.shape={1,1,1};
    e.geometry.cells={0}; e.geometry.links={0}; e.geometry.glass={0};
    e.geometry.faces.assign(6,0); e.geometry.masks={0};
    const int type=face_type_for_tech(tech);
    if (type>=0 && direction>=0 && direction<6) {
        e.geometry.faces[direction]=uint8_t(type);
        e.geometry.masks[0]=uint8_t(1u<<direction);
    }
    return e;
}
inline int face_direction(const GhostEnvelope& e,int32_t tech) {
    const int type=face_type_for_tech(tech);
    if (type<0 || e.ghost_class!=face_descriptor_class(tech) || e.target_base_id.empty() ||
        !valid(e.geometry) || e.geometry.shape!=std::array<int32_t,3>{1,1,1} ||
        e.geometry.cells!=std::vector<uint8_t>{0} || e.geometry.links!=std::vector<uint8_t>{0} ||
        e.geometry.glass!=std::vector<uint8_t>{0} || e.geometry.masks.size()!=1 ||
        e.geometry.cell_offset!=std::array<int32_t,3>{} || e.geometry.anchor!=std::array<int32_t,3>{}) return -1;
    for (int32_t v:e.target_offset) if (v < -8192 || v > 8192) return -1;
    for (int32_t v:e.target_base_anchor) if (v < -8192 || v > 8192) return -1;
    int found=-1;
    for (int d=0;d<6;++d) {
        if (e.geometry.faces[d]==type && found<0) found=d;
        else if (e.geometry.faces[d]!=0) return -1;
    }
    if (tech==5540 && (found!=4 || int64_t(e.target_offset[0])+2>8192 ||
        int64_t(e.target_offset[2])+2>8192 || int64_t(e.target_offset[1])+1>8192)) return -1;
    return found>=0 && e.geometry.masks[0]==uint8_t(1u<<found) ? found : -1;
}
// Physical wall identity deliberately spans recipe types: a replacement on the
// same face must prevent an older object's delayed demolition from touching it.
inline bool same_structural_face(const GhostEnvelope& a,const GhostEnvelope& b) {
    int d=face_direction(a,face_descriptor_tech(a));
    return d>=0 && d==face_direction(b,face_descriptor_tech(b)) &&
        same_target_cell(a,b);
}
// Original hatch-only policy retained for regression fixtures and older callers.
inline GhostEnvelope hatch_descriptor(GhostEnvelope e,std::array<int32_t,3> cell,int direction) {
    return face_descriptor(std::move(e),cell,direction,5501);
}
inline int hatch_direction(const GhostEnvelope& e,int32_t tech) { return tech==5501 ? face_direction(e,tech) : -1; }
inline bool same_hatch_face(const GhostEnvelope& a,const GhostEnvelope& b) {
    return hatch_direction(a,5501)>=0 && hatch_direction(b,5501)>=0 && same_structural_face(a,b);
}
}
