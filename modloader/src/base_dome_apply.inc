// GlassDome18 occupies the Above face at the minimum Room root, not a wall
// face or the room's central occupied alias. CanSetFace additionally checks
// that the full 3x1x3 region above it is empty (native CanSetRoomFace).
bool build_dome_owner_present(void* target,const base_geometry::GhostEnvelope& e) {
    if (base_geometry::face_direction(e,5540)!=4 || !g_corridor_api.get_raw_cell) return false;
    for (int z=0;z<3;++z) for (int x=0;x<3;++x) {
        BuildGridPoint p{e.target_offset[0]+x,e.target_offset[1],e.target_offset[2]+z};
        const auto actual=g_corridor_api.get_raw_cell.call(target,p,g_corridor_api.get_raw_cell.method);
        if (actual!=(x==0 && z==0 ? 1 : 3)) return false;
    }
    return true;
}
