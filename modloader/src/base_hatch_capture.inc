struct HatchPoint { int32_t x,y,z; };
struct HatchFace { HatchPoint cell; int32_t direction; };
static_assert(sizeof(HatchFace)==16, "Base.Face ABI");
using HatchFindFaceFn=uint8_t (*)(void*,HatchFace*,void*);
using HatchGridWorldFn=Vector3 (*)(void*,HatchPoint,void*);
using HatchWorldGridFn=HatchPoint (*)(void*,Vector3,void*);
using HatchGetFaceFn=uint8_t (*)(void*,HatchFace,void*);
struct HatchCaptureApi {
    bool ready=false;
    Entry<HatchFindFaceFn> find_face;
    Entry<HatchGridWorldFn> grid_world;
    Entry<HatchWorldGridFn> world_grid;
    Entry<HatchGetFaceFn> get_face;
};
HatchCaptureApi g_hatch_capture;
bool build_resolve_hatch_capture() {
    auto& a=g_hatch_capture;
    if (a.ready) return true;
    if (!build_resolve_ghost_capture()) return false;
    auto* cls=build_class("BaseAddFaceGhost");
    auto* base=build_class("Base");
    a.find_face.bind(find_method(cls,"FindFirstMaskedFace",2));
    a.grid_world.bind(build_typed_method(base,"GridToWorld",1,"Int3"));
    a.world_grid.bind(find_method(base,"WorldToGrid",1));
    a.get_face.bind(find_method(base,"GetFaceRaw",1));
    a.ready=a.find_face && a.grid_world && a.world_grid && a.get_face;
    return a.ready;
}
bool build_capture_hatch(void* component,base_geometry::GhostEnvelope* envelope,int32_t tech=5501) {
    const int expected=base_geometry::face_type_for_tech(tech);
    if (expected<0 || !envelope || !build_resolve_hatch_capture()) return false;
    void* model=nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(component),g_ghost_capture.model,&model);
    if (!build_live(model)) return false;
    void* ghost=g_get_component.call(model,g_ghost_capture.ghost_type,g_get_component.method);
    void *base=nullptr,*target=nullptr;
    if (!build_live(ghost)) return false;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(ghost),g_ghost_capture.ghost_base,&base);
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(ghost),g_ghost_capture.target,&target);
    HatchFace face{};
    if (!build_live(base) || !build_live(target) ||
        !g_hatch_capture.find_face.call(base,&face,g_hatch_capture.find_face.method) ||
        face.direction<0 || face.direction>=6 ||
        g_hatch_capture.get_face.call(base,face,g_hatch_capture.get_face.method)!=expected) return false;
    const Vector3 world=g_hatch_capture.grid_world.call(base,face.cell,g_hatch_capture.grid_world.method);
    const HatchPoint cell=g_hatch_capture.world_grid.call(target,world,g_hatch_capture.world_grid.method);
    void* uid=build_uid(target);
    char target_id[kWorldEventObjectIdSize]{};
    if (!uid || !string_to_ascii(g_unique_get_id.call(uid,g_unique_get_id.method),target_id,sizeof(target_id))) return false;
    envelope->target_base_id=target_id;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(target),g_ghost_capture.anchor,envelope->target_base_anchor.data());
    *envelope=base_geometry::face_descriptor(std::move(*envelope),{cell.x,cell.y,cell.z},face.direction,tech);
    return base_geometry::face_direction(*envelope,tech)>=0;
}
