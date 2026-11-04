#pragma once
#include "base_hatch.h"

namespace base_geometry {
// First ladder family: two existing corridor floors and zero or more connector
// cells between them. Rooms have additional exit/offset rules, left to a later batch.
inline int ladder_height(const GhostEnvelope& e,int32_t tech=5504) {
    const auto& s=e.geometry;
    const int h=s.shape[1];
    if (tech!=5504 || e.ghost_class!="BaseCorridorLadder" || e.target_base_id.empty() ||
        h<2 || h>16 || s.shape!=std::array<int32_t,3>{1,h,1} || !valid(s) ||
        s.cell_offset!=std::array<int32_t,3>{} || s.anchor!=std::array<int32_t,3>{} ||
        s.cells!=std::vector<uint8_t>(h,0) || s.links!=std::vector<uint8_t>(h,0) ||
        s.glass!=std::vector<uint8_t>(h,0) || s.masks.size()!=size_t(h)) return 0;
    for (int v:e.target_offset) if (v < -8192 || v > 8192) return 0;
    for (int v:e.target_base_anchor) if (v < -8192 || v > 8192) return 0;
    if (int64_t(e.target_offset[1])+h-1>8192) return 0;
    for (int y=0;y<h;++y) {
        const int d=y==0 ? 4 : y==h-1 ? 5 : 0;
        if (s.masks[y]!=uint8_t(1u<<d)) return 0;
        for (int j=0;j<6;++j) if (s.faces[6*y+j]!=(j==d ? 5 : 0)) return 0;
    }
    return h;
}
inline GhostEnvelope ladder_descriptor(GhostEnvelope e,std::array<int32_t,3> lower,int height) {
    e.ghost_class="BaseCorridorLadder";e.target_offset=lower;e.geometry=Snapshot{};
    if (height<2 || height>16) return e;
    auto& s=e.geometry;s.shape={1,height,1};s.cells.assign(height,0);
    s.links=s.glass=s.cells;s.faces.assign(6*height,0);s.masks.assign(height,0);
    for (int y=0;y<height;++y) {
        const int d=y==0 ? 4 : y==height-1 ? 5 : 0;
        s.faces[6*y+d]=5;s.masks[y]=uint8_t(1u<<d);
    }
    return e;
}
inline bool same_ladder(const GhostEnvelope& a,const GhostEnvelope& b) {
    return ladder_height(a)>0 && ladder_height(a)==ladder_height(b) &&
        a.target_base_id==b.target_base_id && a.target_base_anchor==b.target_base_anchor &&
        a.target_offset==b.target_offset;
}
// Compare physical faces, including a wall recipe replacing an endpoint.
inline bool ladder_faces_overlap(const GhostEnvelope& a,const GhostEnvelope& b) {
    const int h=ladder_height(a), other=ladder_height(b);
    const int face=face_direction(b,face_descriptor_tech(b));
    if (!h || (!other && face<0) || a.target_base_id!=b.target_base_id ||
        a.target_base_anchor!=b.target_base_anchor || a.target_offset[0]!=b.target_offset[0] ||
        a.target_offset[2]!=b.target_offset[2]) return false;
    for (int y=0;y<h;++y) {
        const int d=y==0 ? 4 : y==h-1 ? 5 : 0;
        for (int z=0;z<(other ? other : 1);++z) {
            const int bd=other ? (z==0 ? 4 : z==other-1 ? 5 : 0) : face;
            if (d==bd && a.target_offset[1]+y==b.target_offset[1]+z) return true;
        }
    }
    return false;
}
}
