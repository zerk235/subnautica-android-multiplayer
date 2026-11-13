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
    void* anchored_face=nullptr;
};
// A wall module (bioreactor, water filtration) is a BaseAddModuleGhost, not a
// BaseAddFaceGhost, but both carry the same Nullable<Base.Face> anchoredFace at 0x4C.
// Pinning one exact class made every bioreactor capture fail before it read anything.
void* build_ghost_anchored_face(struct Il2CppClass* klass,int* offset) {
    for(struct Il2CppClass* c=klass;c;c=il2cpp_class_get_parent(c)) {
        void* it=nullptr;
        for(struct Il2CppFieldInfo* f=il2cpp_class_get_fields(c,&it);f;f=il2cpp_class_get_fields(c,&it)) {
            const char* name=il2cpp_field_get_name(f);
            if(name&&strcmp(name,"anchoredFace")==0) {
                if(offset)*offset=il2cpp_field_get_offset(f);
                return f;
            }
        }
    }
    return nullptr;
}
void build_report_ghost_class(struct Il2CppClass* klass) {
    static void* reported=nullptr;
    if(!klass||klass==reported)return;
    reported=klass;
    char names[192]="";
    void* it=nullptr;
    for(struct Il2CppFieldInfo* f=il2cpp_class_get_fields(klass,&it);f;f=il2cpp_class_get_fields(klass,&it)) {
        const char* name=il2cpp_field_get_name(f);
        if(!name)continue;
        if(strlen(names)+strlen(name)+2>=sizeof(names))break;
        if(names[0])strcat(names,",");
        strcat(names,name);
    }
    const char* cls_name=il2cpp_class_get_name(klass);
    LOG_INFO("bioreactor: ghost class=%s fields=%s",cls_name?cls_name:"?",names);
}
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
    a.anchored_face=build_field_on(cls,"anchoredFace",0x4C);
    a.ready=a.find_face && a.grid_world && a.world_grid && a.get_face;
    return a.ready;
}
bool build_capture_hatch(void* component,base_geometry::GhostEnvelope* envelope,int32_t tech=5501) {
    const int expected=base_geometry::face_type_for_tech(tech);
    // Bioreactor capture used to fail silently; every early exit names its stage.
    const char* stage="expected";
    auto fail=[&]() {
        if (tech==base_geometry::kBioreactorTech)
            LOG_WARN("bioreactor: capture failed stage=%s expected=%d",stage,expected);
        return false;
    };
    if (expected<0 || !envelope || !build_resolve_hatch_capture()) return fail();
    void* model=nullptr;
    stage="model-field";
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(component),g_ghost_capture.model,&model);
    if (!build_live(model)) return fail();
    stage="ghost-component";
    void* ghost=g_get_component.call(model,g_ghost_capture.ghost_type,g_get_component.method);
    void *base=nullptr,*target=nullptr;
    if (!build_live(ghost)) return fail();
    stage="ghost-fields";
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(ghost),g_ghost_capture.ghost_base,&base);
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(ghost),g_ghost_capture.target,&target);
    HatchFace face{};
    if (!build_live(target)) return fail();
    bool anchored=false;
    if(tech==base_geometry::kBioreactorTech) {
        stage="ghost-class";
        auto* cls=il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(ghost));
        build_report_ghost_class(cls);
        stage="anchored-field";
        int anchored_offset=0;
        void* anchored_field=build_ghost_anchored_face(cls,&anchored_offset);
        if(anchored_field) {
            stage="anchored-read";
            uint8_t nullable[20]{};
            il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(ghost),anchored_field,nullable);
            anchored=nullable[0]!=0;
            if(anchored) {
                memcpy(&face,nullable+4,sizeof(face));
                int32_t anchor[3]{};
                il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(target),g_ghost_capture.anchor,anchor);
                face.cell={face.cell.x+anchor[0],face.cell.y+anchor[1],face.cell.z+anchor[2]};
            }
        }
        LOG_INFO("bioreactor: capture anchored=%u field=%p off=%d cell=(%d,%d,%d) dir=%d base=%p",
                 unsigned(anchored),anchored_field,anchored_offset,face.cell.x,face.cell.y,face.cell.z,
                 face.direction,base);
    }
    if(!anchored) {
        stage="find-face";
        if(!build_live(base))return fail();
        if(!g_hatch_capture.find_face.call(base,&face,g_hatch_capture.find_face.method))return fail();
        stage="face-type";
        const uint8_t actual=g_hatch_capture.get_face.call(base,face,g_hatch_capture.get_face.method);
        if(actual!=expected) {
            if (tech==base_geometry::kBioreactorTech)
                LOG_WARN("bioreactor: capture scanned cell=(%d,%d,%d) dir=%d type=%u expected=%d",
                         face.cell.x,face.cell.y,face.cell.z,face.direction,unsigned(actual),expected);
            return fail();
        }
        stage="grid-convert";
        const Vector3 world=g_hatch_capture.grid_world.call(base,face.cell,g_hatch_capture.grid_world.method);
        face.cell=g_hatch_capture.world_grid.call(target,world,g_hatch_capture.world_grid.method);
    }
    stage="uid";
    void* uid=build_uid(target);
    char target_id[kWorldEventObjectIdSize]{};
    if (!uid || !string_to_ascii(g_unique_get_id.call(uid,g_unique_get_id.method),target_id,sizeof(target_id))) return fail();
    envelope->target_base_id=target_id;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(target),g_ghost_capture.anchor,envelope->target_base_anchor.data());
    stage="descriptor";
    *envelope=base_geometry::face_descriptor(std::move(*envelope),{face.cell.x,face.cell.y,face.cell.z},face.direction,tech);
    if (base_geometry::face_direction(*envelope,tech)<0) return fail();
    if (tech==base_geometry::kBioreactorTech)
        LOG_INFO("bioreactor: capture ok target=%s cell=(%d,%d,%d) dir=%d",
                 target_id,face.cell.x,face.cell.y,face.cell.z,face.direction);
    return true;
}
