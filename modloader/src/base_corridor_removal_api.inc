using BuildClearCellFn = void (*)(void*, BuildGridPoint, void*);
using BuildDeconstructionAllowedFn = uint8_t (*)(void*, void**, void*);
using BuildIsEmptyFn = uint8_t (*)(void*,void*,void*);
struct CorridorRemovalApi {
    bool attempted = false, ready = false;
    void *type = nullptr, *bounds = nullptr, *face = nullptr, *recipe = nullptr;
    Entry<BuildClearCellFn> clear_cell;
    Entry<ManagedFn> fix_floors, fix_links, rebuild, pre_destroy;
    Entry<BuildIsEmptyFn> is_empty;
    Entry<BuildDeconstructionAllowedFn> allowed;
};
CorridorRemovalApi g_corridor_removal_api;
bool g_corridor_removal_failed = false;

bool build_resolve_corridor_removal() {
    auto& a = g_corridor_removal_api;
    if (a.attempted) return a.ready;
    a.attempted = true;
    if (!build_resolve_corridor_api() || !g_object_destroy) return false;
    auto* cls = build_class("BaseDeconstructable");
    const void* type = cls ? il2cpp_class_get_type(cls) : nullptr;
    a.type = type ? il2cpp_type_get_object(type) : nullptr;
    a.bounds = build_field_on(cls, "bounds", 0x24);
    a.face = build_field_on(cls, "face", 0x3C);
    a.recipe = build_field_on(cls, "recipe", 0x60);
    a.allowed.bind(find_method(cls, "DeconstructionAllowed", 1));
    a.clear_cell.bind(build_typed_method(g_corridor_api.base, "ClearCell", 1, "Int3"));
    a.fix_floors.bind(find_method(g_corridor_api.base, "FixRoomFloors", 0));
    a.fix_links.bind(find_method(g_corridor_api.base, "FixCorridorLinks", 0));
    a.rebuild.bind(find_method(g_corridor_api.base, "RebuildGeometry", 0));
    a.is_empty.bind(build_typed_method(g_corridor_api.base,"IsEmpty",1,"BaseGhost"));
    a.pre_destroy.bind(find_method(g_corridor_api.base,"OnPreDestroy",0));
    a.ready = a.type && a.bounds && a.face && a.recipe && a.allowed && a.clear_cell && a.fix_floors && a.fix_links && a.rebuild && a.is_empty && a.pre_destroy;
    LOG_INFO("build: corridor removal API %s type=%u fields=%u allowed=%u clear=%u repair=%u empty=%u pre_destroy=%u",
        a.ready ? "ready" : "unavailable",unsigned(a.type!=nullptr),unsigned(a.bounds && a.face && a.recipe),
        unsigned(bool(a.allowed)),unsigned(bool(a.clear_cell)),unsigned(bool(a.fix_floors) && bool(a.fix_links) && bool(a.rebuild)),
        unsigned(bool(a.is_empty)),unsigned(bool(a.pre_destroy)));
    return a.ready;
}

