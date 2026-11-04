// Raw cell reads matter: GetCell normalizes OccupiedByOtherCell to its root,
// hiding a damaged or foreign quarter of a multi-cell foundation.
bool build_foundation_footprint_matches(void* target,const base_geometry::GhostEnvelope& e,bool removed) {
    if (!g_corridor_api.get_raw_cell || !base_geometry::foundation_topology_valid(e.geometry)) return false;
    unsigned i=0;
    for (int z=0;z<2;++z) for (int x=0;x<2;++x,++i) {
        BuildGridPoint point{e.target_offset[0]+x,e.target_offset[1],e.target_offset[2]+z};
        const auto actual=g_corridor_api.get_raw_cell.call(target,point,g_corridor_api.get_raw_cell.method);
        if (actual!=(removed ? 0 : e.geometry.cells[i])) {
            LOG_WARN("build: foundation footprint mismatch cell=(%d,%d,%d) actual=%u expected=%u removed=%u",
                point.x,point.y,point.z,unsigned(actual),unsigned(removed ? 0 : e.geometry.cells[i]),unsigned(removed));
            return false;
        }
    }
    return true;
}
bool build_room_footprint_matches(void* target,const base_geometry::GhostEnvelope& e,bool removed) {
    if (!g_corridor_api.get_raw_cell || !base_geometry::room_topology_valid(e.geometry)) return false;
    for (int z=0;z<3;++z) for (int x=0;x<3;++x) {
        BuildGridPoint p{e.target_offset[0]+x,e.target_offset[1],e.target_offset[2]+z};
        const auto actual=g_corridor_api.get_raw_cell.call(target,p,g_corridor_api.get_raw_cell.method);
        const auto expected=removed ? 0 : e.geometry.cells[size_t(x+3*z)];
        if (actual!=expected) {
            LOG_WARN("build: room footprint mismatch cell=(%d,%d,%d) actual=%u expected=%u removed=%u",
                p.x,p.y,p.z,unsigned(actual),unsigned(expected),unsigned(removed));
            return false;
        }
    }
    return true;
}
bool build_piece_footprint_free(void* target,const base_geometry::GhostEnvelope& e,int32_t tech) {
    if (!g_corridor_api.get_raw_cell) return false;
    const auto size=base_geometry::piece_shape(tech);
    for (int z=0;z<size[2];++z) for (int y=0;y<size[1];++y) for (int x=0;x<size[0];++x) {
        BuildGridPoint p{e.target_offset[0]+x,e.target_offset[1]+y,e.target_offset[2]+z};
        const auto actual=g_corridor_api.get_raw_cell.call(target,p,g_corridor_api.get_raw_cell.method);
        if (actual!=0) {
            LOG_WARN("build: piece footprint busy id=%s tech=%d cell=(%d,%d,%d) raw=%u; waiting before mutation",
                e.object_id.c_str(),tech,p.x,p.y,p.z,unsigned(actual));
            return false;
        }
    }
    return true;
}
// Count active root cells, not the three occupied aliases or unused capacity.
bool build_foundation_has_other_root(void* target) {
    int32_t shape[4]{};void* cells=nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(target),g_ghost_capture.shape,shape);
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(target),g_ghost_capture.cells,&cells);
    size_t count=0;
    if (!base_geometry::cell_count({shape[0],shape[1],shape[2]},&count) ||
        int64_t(shape[0])*shape[1]!=shape[3] || !cells ||
        g_array_get_length.call(cells,g_array_get_length.method)<int32_t(count)) return false;
    const auto* bytes=reinterpret_cast<const uint8_t*>(cells)+0x20;
    unsigned roots=0;
    for (size_t i=0;i<count;++i) if (bytes[i] && bytes[i]!=3) {
        if (bytes[i]>19) return false;
        if (++roots>=2) return true;
    }
    return false;
}
