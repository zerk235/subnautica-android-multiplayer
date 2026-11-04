using LadderExitFn=uint8_t (*)(void*,HatchPoint,int32_t,HatchPoint*,void*);
using LadderCanFn=uint8_t (*)(void*,HatchFace,HatchFace*,void*);
using LadderCellFn=uint8_t (*)(void*,HatchPoint,void*);
struct LadderCaptureApi {
    bool ready=false;
    Entry<LadderExitFn> exit_cell;
    Entry<LadderCanFn> can_set;
    Entry<LadderCellFn> get_cell;
};
LadderCaptureApi g_ladder_capture;
bool build_resolve_ladder_capture() {
    auto& a=g_ladder_capture;
    if (a.ready) return true;
    if (!build_resolve_hatch_capture() || !il2cpp_field_static_get_value) return false;
    auto* base=build_class("Base");
    auto* cls=build_class("BaseAddLadderGhost");
    void* field=cls ? build_field_on(cls,"ladderFaceDir",0) : nullptr;
    int marker=-1;
    if (field) il2cpp_field_static_get_value(field,&marker);
    if (marker!=0) return false; // native connector marker must match the descriptor
    a.exit_cell.bind(find_method(base,"GetLadderExitCell",3));
    a.can_set.bind(find_method(base,"CanSetLadder",2));
    a.get_cell.bind(build_typed_method(base,"GetCell",1,"Int3"));
    a.ready=a.exit_cell && a.can_set && a.get_cell;
    LOG_INFO("build: corridor ladder capture API %s",a.ready ? "ready" : "unavailable");
    return a.ready;
}
bool build_ladder_path_cells(void* base,HatchPoint lower,int height) {
    for (int y=0;y<height;++y) {
        HatchPoint c{lower.x,lower.y+y,lower.z};
        const int expected=(y==0 || y==height-1) ? 4 : 6;
        if (g_ladder_capture.get_cell.call(base,c,g_ladder_capture.get_cell.method)!=expected) return false;
    }
    return true;
}
bool build_capture_ladder(void* component,base_geometry::GhostEnvelope* e,bool demolition=false) {
    if (!e || !build_resolve_ladder_capture()) return false;
    void* model=nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(component),g_ghost_capture.model,&model);
    if (!build_live(model)) return false;
    void* ghost=g_get_component.call(model,g_ghost_capture.ghost_type,g_get_component.method);
    if (!build_live(ghost)) return false;
    auto* cls=il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(ghost));
    const char* name=cls ? il2cpp_class_get_name(cls) : nullptr;
    // Native demolition uses basedeconstructable.prefab's plain BaseGhost and
    // copies the selected ladder into it. Only the final demolition observer
    // may accept that model; construction must use BaseAddLadderGhost.
    if (!name || (strcmp(name,"BaseAddLadderGhost") &&
                  !(demolition && !strcmp(name,"BaseGhost")))) {
        LOG_WARN("build: ladder capture refused class=%s demolition=%u",name ? name : "null",unsigned(demolition));
        return false;
    }
    void *base=nullptr,*target=nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(ghost),g_ghost_capture.ghost_base,&base);
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(ghost),g_ghost_capture.target,&target);
    HatchFace face{};HatchPoint end{};
    if (!build_live(base) || !build_live(target) ||
        !g_hatch_capture.find_face.call(base,&face,g_hatch_capture.find_face.method) ||
        (face.direction!=4 && face.direction!=5) ||
        !g_ladder_capture.exit_cell.call(base,face.cell,face.direction,&end,g_ladder_capture.exit_cell.method)) {
        LOG_WARN("build: ladder capture refused endpoint class=%s demolition=%u direction=%d",name,unsigned(demolition),face.direction);
        return false;
    }
    HatchPoint lower=face.direction==4 ? face.cell : end;
    HatchPoint upper=face.direction==4 ? end : face.cell;
    const int64_t h=int64_t(upper.y)-lower.y+1;
    if (lower.x!=upper.x || lower.z!=upper.z || h<2 || h>16 ||
        !build_ladder_path_cells(base,lower,int(h))) {
        LOG_WARN("build: ladder capture refused path class=%s demolition=%u height=%lld",name,unsigned(demolition),static_cast<long long>(h));
        return false;
    }
    for (int y=0;y<h;++y) {
        HatchFace f{{lower.x,lower.y+y,lower.z},y==0 ? 4 : y==h-1 ? 5 : 0};
        const int type=g_hatch_capture.get_face.call(base,f,g_hatch_capture.get_face.method);
        if (type!=5) {
            LOG_WARN("build: ladder capture refused face class=%s demolition=%u y=%d direction=%d type=%d",name,unsigned(demolition),y,f.direction,type);
            return false;
        }
    }
    auto map=[&](HatchPoint p) {
        Vector3 world=g_hatch_capture.grid_world.call(base,p,g_hatch_capture.grid_world.method);
        return g_hatch_capture.world_grid.call(target,world,g_hatch_capture.world_grid.method);
    };
    const auto a=map(lower),b=map(upper);
    if (a.x!=b.x || a.z!=b.z || int64_t(b.y)-a.y+1!=h) return false;
    void* uid=build_uid(target);char id[kWorldEventObjectIdSize]{};
    if (!uid || !string_to_ascii(g_unique_get_id.call(uid,g_unique_get_id.method),id,sizeof(id))) return false;
    e->target_base_id=id;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(target),g_ghost_capture.anchor,e->target_base_anchor.data());
    *e=base_geometry::ladder_descriptor(std::move(*e),{a.x,a.y,a.z},int(h));
    LOG_INFO("build: ladder snapshot lower=(%d,%d,%d) height=%d class=%s demolition=%u",a.x,a.y,a.z,int(h),name,unsigned(demolition));
    return base_geometry::ladder_height(*e)>0;
}
