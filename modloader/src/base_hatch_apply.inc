using HatchSetFaceFn=void (*)(void*,HatchFace,uint8_t,void*);
using HatchCanFaceFn=uint8_t (*)(void*,HatchFace,uint8_t,void*);
struct HatchApplyApi {
    bool ready=false,failed=false;
    Entry<HatchSetFaceFn> set_face,clear_face;
    Entry<HatchCanFaceFn> can_set;
};
HatchApplyApi g_hatch_apply;
bool build_resolve_hatch_apply() {
    auto& a=g_hatch_apply;
    if (a.ready) return true;
    if (!build_resolve_hatch_capture() || !build_resolve_corridor_removal()) return false;
    auto* cls=g_corridor_api.base;
    a.set_face.bind(find_method(cls,"SetFaceType",2));
    a.clear_face.bind(find_method(cls,"ClearFace",2));
    a.can_set.bind(find_method(cls,"CanSetFace",2));
    a.ready=a.set_face && a.clear_face && a.can_set;
    LOG_INFO("build: structural face API %s",a.ready ? "ready" : "unavailable");
    return a.ready;
}
bool build_hatch_removal_allowed(void* target,HatchFace face,int32_t tech) {
    auto& a=g_corridor_removal_api;
    void* list=g_find_objects_all.call(a.type,true,g_find_objects_all.method);
    int count=list ? g_array_get_length.call(list,g_array_get_length.method) : 0;
    void* found=nullptr;
    for (int i=0;i<count;++i) {
        void* candidate=g_array_get_value.call(list,i,g_array_get_value.method);
        if (!build_live(candidate)) continue;
        int recipe=0;
        uint8_t nullable[20]{};
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(candidate),a.recipe,&recipe);
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(candidate),a.face,nullable);
        HatchFace actual{};
        memcpy(&actual,nullable+4,sizeof(actual));
        if (recipe!=tech || !nullable[0] || actual.cell.x!=face.cell.x || actual.cell.y!=face.cell.y ||
            actual.cell.z!=face.cell.z || actual.direction!=face.direction) continue;
        bool belongs=false;
        void* go=g_get_game_object.call(candidate,g_get_game_object.method);
        walk_parents(go,[&](void* node) {
            if (g_get_component.call(node,g_build_base_geometry_type,g_get_component.method)!=target) return false;
            belongs=true; return true;
        });
        if (!belongs) continue;
        if (found) return false;
        found=candidate;
    }
    void* reason=nullptr;
    return found && a.allowed.call(found,&reason,a.allowed.method);
}
// true = finished/refused; false = retry before any mutation.
#include "base_dome_apply.inc"
bool build_apply_hatch_inner(void* target,const WorldEvent& e,const base_geometry::GhostEnvelope& wire_geometry) {
    base_geometry::GhostEnvelope geometry;
    if (!build_map_base_geometry(target,wire_geometry,&geometry)) return true;
    const int direction=base_geometry::face_direction(geometry,e.tech_type);
    const int face_type=base_geometry::face_type_for_tech(e.tech_type);
    if (direction<0 || geometry.object_id!=e.object_id || e.face_has_value ||
        !(e.build_flags & kWorldBuildHasGeometry) || !(e.build_flags & kWorldBuildComplete) ||
        (e.kind!=kWorldEventBuild && e.kind!=kWorldEventRemove)) return true;
    if (g_hatch_apply.failed || !build_resolve_hatch_apply()) return true;
    HatchFace face{{geometry.target_offset[0],geometry.target_offset[1],geometry.target_offset[2]},direction};
    const bool remove=e.kind==kWorldEventRemove;
    // A dome may arrive before its room's async prefab placement. Retry only
    // before mutation; never write roof topology into an absent/foreign room.
    if (e.tech_type==5540 && !build_dome_owner_present(target,geometry)) return remove;
    const uint8_t current=g_hatch_capture.get_face.call(target,face,g_hatch_capture.get_face.method);
    if (remove) {
        if (!build_removal_identity_matches(e,geometry)) return true;
        if (build_face_has_replacement(geometry,e.object_id)) return true;
        if (current!=face_type) return true;
        if (!build_hatch_removal_allowed(target,face,e.tech_type)) return false;
        if (!build_bind_removal_identity(e,wire_geometry)) return true;
    } else {
        if (build_corridor_removed(e.object_id) || build_corridor_record(e,false)) return true;
        if ((current!=0 && current!=1) || (e.tech_type==5540 && current!=1)) {
            LOG_WARN("build: face placement refused id=%s face_type=%u",e.object_id,current);
            return true;
        }
        if (!g_hatch_apply.can_set.call(target,face,uint8_t(face_type),g_hatch_apply.can_set.method)) return false;
        if (!build_synced_corridor(e.object_id,true)) return true;
        auto* record=build_corridor_record(e,true);
        if (!record) return true;
        record->used=true; record->origin=e.origin_id;
        snprintf(record->object_id,sizeof(record->object_id),"%s",e.object_id);
    }
    ++g_applying_remote;
    if (remove) g_hatch_apply.clear_face.call(target,face,uint8_t(face_type),g_hatch_apply.clear_face.method);
    else g_hatch_apply.set_face.call(target,face,uint8_t(face_type),g_hatch_apply.set_face.method);
    auto& a=g_corridor_removal_api;
    a.fix_floors.call(target,a.fix_floors.method);
    a.fix_links.call(target,a.fix_links.method);
    a.rebuild.call(target,a.rebuild.method);
    const uint8_t actual=g_hatch_capture.get_face.call(target,face,g_hatch_capture.get_face.method);
    const bool verified=remove ? (e.tech_type==5540 ? actual==1 : (actual==0 || actual==1)) : actual==face_type;
    if (verified && !remove) {
        build_remember_corridor(e,wire_geometry);
        build_corridor_record(e,false)->verified=true;
    }
    if (!verified) g_hatch_apply.failed=true;
    LOG_INFO("build: face %s id=%s tech=%d cell=(%d,%d,%d) direction=%d face=%u verified=%u",
        remove ? "removed" : "applied",e.object_id,e.tech_type,face.cell.x,face.cell.y,face.cell.z,direction,actual,unsigned(verified));
    return true;
}
bool build_apply_hatch(void* target,const WorldEvent& e,const base_geometry::GhostEnvelope& geometry) {
    const int prior=g_applying_remote;
    bool done=true;
    try { done=build_apply_hatch_inner(target,e,geometry); }
    catch (...) {
        g_hatch_apply.failed=true;
        LOG_WARN("build: face game exception id=%s; structural face apply disabled until restart",e.object_id);
    }
    g_applying_remote=prior;
    return done;
}
