// Base.AllocateArrays keeps spare capacity (and doubles an existing array when
// growing it). Unlike replay ghosts, completed bases need only contain the active
// grid. masks is a placement-preview cache: AllocateArrays does not resize it.
// Never use this check for copying a received ghost's topology arrays.
bool build_completed_base_storage_valid(void* base) {
    int32_t shape[4]{};
    auto& a=g_ghost_capture;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(base),a.shape,shape);
    size_t count=0;
    if (!base_geometry::cell_count({shape[0],shape[1],shape[2]},&count) ||
        int64_t(shape[0])*shape[1]!=shape[3]) return false;
    auto contains=[&](void* field,size_t minimum) {
        void* array=nullptr;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(base),field,&array);
        const int32_t capacity=array ? g_array_get_length.call(array,g_array_get_length.method) : -1;
        if (capacity>=int32_t(minimum)) return true;
        static double next_report=0;
        if (build_now()>=next_report) {
            next_report=build_now()+5;
            LOG_INFO("build: completed base storage deferred shape=(%d,%d,%d) field=%p capacity=%d required=%zu",
                shape[0],shape[1],shape[2],field,capacity,minimum);
        }
        return false;
    };
    return contains(a.faces,count*6) && contains(a.cells,count) &&
        contains(a.links,count) && contains(a.glass,count) &&
        contains(g_corridor_api.cell_objects,count);
}
