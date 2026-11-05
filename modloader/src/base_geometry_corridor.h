#pragma once

#include "base_geometry_envelope.h"
#include "base_hatch.h"
#include "base_ladder.h"
#include "base_room.h"

namespace base_geometry {
inline bool same_corridor_cell(const GhostEnvelope& a, const GhostEnvelope& b) {
    return !a.target_base_id.empty() && a.target_base_id == b.target_base_id &&
           a.target_base_anchor == b.target_base_anchor && a.target_offset == b.target_offset;
}
// Android 1.22.83315 TechType and BaseAddCorridorGhost.Corridor values.
// All six prefabs use the same single-cell placement path. Rooms and face
// modules require different placement metadata and remain excluded.
inline int corridor_shape(int32_t tech_type) {
    switch (tech_type) {
        case 5510: case 5516: return 0; // I
        case 5511: case 5517: return 1; // L
        case 5512: return 2; // T
        case 5513: return 3; // X
        default: return -1;
    }
}
inline bool corridor_glass(int32_t tech_type) {
    return tech_type == 5516 || tech_type == 5517;
}
inline bool corridor_prefab_matches(int32_t tech_type, int32_t shape, bool glass) {
    return corridor_shape(tech_type) >= 0 && corridor_shape(tech_type) == shape &&
           corridor_glass(tech_type) == glass;
}
inline bool supported_existing_corridor(const GhostEnvelope& e, int32_t tech_type) {
    if (corridor_shape(tech_type) < 0 || (e.ghost_class != "BaseAddCorridorGhost" && e.ghost_class != "BaseGhost") ||
        e.target_base_id.empty() || !valid(e.geometry) ||
        e.geometry.shape != std::array<int32_t, 3>{1, 1, 1} ||
        e.geometry.cells[0] != 4 || e.geometry.glass[0] != uint8_t(corridor_glass(tech_type))) return false;
    for (int32_t value : e.target_offset) if (value < -8192 || value > 8192) return false;
    for (int32_t value : e.target_base_anchor) if (value < -8192 || value > 8192) return false;
    for (int32_t value : e.geometry.cell_offset) if (value < -8192 || value > 8192) return false;
    for (int32_t value : e.geometry.anchor) if (value < -8192 || value > 8192) return false;
    return true;
}

// The vertical connector has a different native ghost but the same single-cell
// placement/storage lifecycle. Foundations and rooms are deliberately excluded.
inline int single_cell_type(int32_t tech) {
    return tech==5519 ? 6 : corridor_shape(tech)>=0 ? 4 : -1;
}
inline const char* single_cell_ghost(int32_t tech) {
    return tech==5519 ? "BaseAddConnectorGhost" : corridor_shape(tech)>=0 ? "BaseAddCorridorGhost" : "";
}
inline bool supported_existing_cell(const GhostEnvelope& e,int32_t tech) {
    if (tech!=5519) return supported_existing_corridor(e,tech);
    if ((e.ghost_class!="BaseAddConnectorGhost" && e.ghost_class!="BaseGhost") ||
        e.target_base_id.empty() || !valid(e.geometry) ||
        e.geometry.shape!=std::array<int32_t,3>{1,1,1} ||
        e.geometry.cells[0]!=6 || e.geometry.glass[0]!=0) return false;
    for (const auto* point:{&e.target_offset,&e.target_base_anchor,&e.geometry.cell_offset,&e.geometry.anchor})
        for (int32_t v:*point) if (v < -8192 || v > 8192) return false;
    return true;
}

inline bool foundation_topology_valid(const Snapshot& s) {
    return valid(s) && s.shape==std::array<int32_t,3>{2,1,2} &&
        s.cells==std::vector<uint8_t>{2,3,3,3} &&
        s.links==std::vector<uint8_t>{0,32,1,33} &&
        s.glass==std::vector<uint8_t>{0,0,0,0};
}
inline bool multi_cell_piece(int32_t tech) { return tech==5509 || tech==5500; }
inline bool piece_topology_valid(const Snapshot& s,int32_t tech) {
    return tech==5500 ? room_topology_valid(s) : tech==5509 ? foundation_topology_valid(s) : false;
}
inline int piece_cell_type(int32_t tech) { return tech==5500 ? 1 : tech==5509 ? 2 : single_cell_type(tech); }
inline std::array<int32_t,3> piece_shape(int32_t tech) {
    return tech==5500 ? std::array<int32_t,3>{3,1,3} : tech==5509 ? std::array<int32_t,3>{2,1,2} : std::array<int32_t,3>{1,1,1};
}
inline const char* piece_ghost(int32_t tech) { return multi_cell_piece(tech) ? "BaseAddCellGhost" : single_cell_ghost(tech); }
inline bool supported_existing_piece(const GhostEnvelope& e,int32_t tech) {
    if (!multi_cell_piece(tech)) return supported_existing_cell(e,tech);
    if ((e.ghost_class!="BaseAddCellGhost" && e.ghost_class!="BaseGhost") ||
        e.target_base_id.empty() || !piece_topology_valid(e.geometry,tech)) return false;
    const auto shape=piece_shape(tech);
    for (size_t i=0;i<3;++i) if (e.target_offset[i] < -8192 ||
        int64_t(e.target_offset[i])+shape[i]-1 > 8192) return false;
    for (const auto* p:{&e.target_base_anchor,&e.geometry.cell_offset,&e.geometry.anchor})
        for (int32_t v:*p) if (v < -8192 || v > 8192) return false;
    return true;
}

// Only recipes that the game itself can place without a target Base may create
// one. Face modules, ladders and connectors require an existing base.
inline bool standalone_piece_topology(const GhostEnvelope& e,int32_t tech) {
    if (tech!=5500 && tech!=5509 && corridor_shape(tech)<0) return false;
    if (e.ghost_class!=piece_ghost(tech)) return false;
    auto candidate=e;
    if (candidate.target_base_id.empty()) candidate.target_base_id="capture-only";
    return supported_existing_piece(candidate,tech);
}
inline bool supported_piece_placement(const GhostEnvelope& e,int32_t tech) {
    return e.creates_base ? (!e.target_base_id.empty() && e.target_base_id!=e.object_id && standalone_piece_topology(e,tech)) :
                           supported_existing_piece(e,tech);
}
inline bool piece_footprints_overlap(const GhostEnvelope& a,const GhostEnvelope& b) {
    if (a.target_base_id.empty() || a.target_base_id!=b.target_base_id ||
        a.target_base_anchor!=b.target_base_anchor) return false;
    for (size_t i=0;i<3;++i) {
        if (a.geometry.shape[i]<=0 || b.geometry.shape[i]<=0 ||
            int64_t(a.target_offset[i])+a.geometry.shape[i]<=b.target_offset[i] ||
            int64_t(b.target_offset[i])+b.geometry.shape[i]<=a.target_offset[i]) return false;
    }
    return true;
}

inline bool piece_deconstruction_bounds(const GhostEnvelope& e,int32_t tech,const int32_t bounds[6]) {
    const auto shape=piece_shape(tech);
    for (size_t i=0;i<3;++i) if (bounds[i]!=e.target_offset[i] ||
        int64_t(bounds[i+3])!=int64_t(e.target_offset[i])+shape[i]-1) return false;
    return true;
}
} // namespace base_geometry
