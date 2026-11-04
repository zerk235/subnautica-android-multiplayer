// Base arrays may retain capacity after a preview shrinks. Only the explicit
// canonical foundation/room paths may copy active prefixes; all other recipes retain
// the original exact-length requirement. Grid3Shape.GetIndex indexes the active
// x/y/z shape directly; occupied-cell links are validated before replay.
bool build_copy_topology_arrays(void* geometry,base_geometry::Snapshot* result,bool foundation,bool room=false) {
    size_t n=0;
    if (!geometry || !result || !base_geometry::cell_count(result->shape,&n) ||
        (foundation && room) ||
        (foundation && (result->shape!=std::array<int32_t,3>{2,1,2} || n!=4)) ||
        (room && (result->shape!=std::array<int32_t,3>{3,1,3} || n!=9))) return false;
    auto& a=g_ghost_capture;
    void* fields[5]={a.faces,a.cells,a.links,a.masks,a.glass};
    const char* names[5]={"faces","cells","links","masks","glass"};
    void* arrays[5]{};int32_t sizes[5]{};
    for (unsigned i=0;i<5;++i) {
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(geometry),fields[i],&arrays[i]);
        sizes[i]=arrays[i] ? g_array_get_length.call(arrays[i],g_array_get_length.method) : -1;
    }
    int32_t capacity=(foundation || room) ? sizes[1] : int32_t(n);
    if (capacity<int32_t(n) || capacity>int32_t(base_geometry::kMaxCells)) return false;
    for (unsigned i=0;i<5;++i) {
        if (i==3 && !arrays[i]) continue;
        const int expected=capacity*(i==0 ? 6 : 1);
        if (sizes[i]!=expected) {
            LOG_WARN("build: geometry capture reason=array field=%s actual=%d expected=%d",names[i],sizes[i],expected);
            return false;
        }
    }
    if (foundation && capacity!=int32_t(n)) LOG_INFO("build: foundation active grid cells=%zu capacity=%d",n,capacity);
    if (room) LOG_INFO("build: room active grid cells=%zu capacity=%d",n,capacity);
    std::vector<uint8_t>* outputs[5]={&result->faces,&result->cells,&result->links,&result->masks,&result->glass};
    for (unsigned i=0;i<5;++i) {
        if (!arrays[i]) { outputs[i]->clear();continue; }
        const auto* bytes=reinterpret_cast<const uint8_t*>(arrays[i])+0x20;
        outputs[i]->assign(bytes,bytes+n*(i==0 ? 6 : 1));
    }
    if (foundation) LOG_INFO("build: foundation snapshot cells=%u,%u,%u,%u links=%u,%u,%u,%u valid=%u",
        result->cells[0],result->cells[1],result->cells[2],result->cells[3],
        result->links[0],result->links[1],result->links[2],result->links[3],
        unsigned(base_geometry::foundation_topology_valid(*result)));
    return room ? base_geometry::room_topology_valid(*result) :
        foundation ? base_geometry::foundation_topology_valid(*result) : base_geometry::valid(*result);
}
