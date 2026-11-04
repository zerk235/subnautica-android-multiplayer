// Use the proximal finger bones that actually skin this clone to estimate the
// palm center. Each bone counts once across clothing renderers; the old socket
// (49 cm away) and foreign skeletons cannot contribute.
bool visual_palm_center(void* root_go,void* wrist,Vector3* result,unsigned* samples) {
    if (samples) *samples=0;
    if (!root_go || !wrist || !result || g_get_component_is_string || !g_smr_type_obj ||
        !g_smr_get_bones || !g_get_position || !g_array_get_length || !g_array_get_value) return false;
    void* stack[512]; size_t top=0,visited=0; stack[top++]=root_go;
    void* seen[8]{}; unsigned count=0;
    Vector3 sum{},origin=g_get_position.call(wrist,g_get_position.method);
    while (top && visited++<512) {
        void* go=stack[--top];
        void* smr=g_get_component.call(go,g_smr_type_obj,g_get_component.method);
        void* bones=smr ? g_smr_get_bones.call(smr,g_smr_get_bones.method) : nullptr;
        int32_t n=bones ? g_array_get_length.call(bones,g_array_get_length.method) : 0;
        if (n>=0 && n<=512) for (int32_t i=0;i<n;++i) {
            void* bone=g_array_get_value.call(bones,i,g_array_get_value.method);
            if (!bone || g_transform_get_parent.call(bone,g_transform_get_parent.method)!=wrist) continue;
            bool duplicate=false; for (unsigned j=0;j<count;++j) if (seen[j]==bone) duplicate=true;
            if (duplicate) continue;
            Vector3 p=g_get_position.call(bone,g_get_position.method);
            Vector3 d{p.x-origin.x,p.y-origin.y,p.z-origin.z};
            float distance2=d.x*d.x+d.y*d.y+d.z*d.z;
            if (!(distance2>=0.0001f && distance2<=0.0225f)) continue;
            if (count>=8) return false;
            seen[count++]=bone; sum.x+=p.x; sum.y+=p.y; sum.z+=p.z;
        }
        void* tf=g_get_go_transform.call(go,g_get_go_transform.method);
        int32_t children=tf ? g_get_child_count.call(tf,g_get_child_count.method) : 0;
        for (int32_t i=0;i<children && top<512;++i) {
            void* child=g_get_child.call(tf,i,g_get_child.method);
            void* child_go=child ? g_get_game_object.call(child,g_get_game_object.method) : nullptr;
            if (child_go) stack[top++]=child_go;
        }
    }
    if (top || count<3 || count>6) return false;
    // Palm center lies halfway from the wrist to the finger-base centroid.
    float k=0.5f/float(count);
    *result=Vector3{0.5f*origin.x+k*sum.x,0.5f*origin.y+k*sum.y,0.5f*origin.z+k*sum.z};
    if (samples) *samples=count;
    return true;
}

// Keep the authored socket's orientation, but place the authored tool grip at
// the palm and parent it to the real wrist. All of this runs only on attachment.
bool grip_has_offset(const Vector3& offset) {
    return offset.x!=0.0f || offset.y!=0.0f || offset.z!=0.0f;
}
Vector3 visual_grip_position(void* model,void* plug,const Vector3& offset) {
    return grip_has_offset(offset) && g_held_transform_point ?
        g_held_transform_point.call(model,offset,g_held_transform_point.method) :
        g_get_position.call(plug,g_get_position.method);
}

bool plug_visual_to_palm(void* model,void* plug,void* wrist,void* socket,const Vector3& palm,
                         const Vector3& offset=Vector3{}) {
    if (!wrist || !g_set_parent || !g_get_position || !g_set_position ||
        (grip_has_offset(offset) && !g_held_transform_point)) return false;
    if (!prop_owns_transform(wrist,socket)) socket=wrist;
    if (!plug_visual_to_hand(model,plug,socket)) {
        g_set_parent.call(model,wrist,true,g_set_parent.method);
        return false;
    }
    g_set_parent.call(model,wrist,true,g_set_parent.method);
    Vector3 p=g_get_position.call(model,g_get_position.method);
    Vector3 grip=visual_grip_position(model,plug,offset);
    g_set_position.call(model,Vector3{p.x+palm.x-grip.x,p.y+palm.y-grip.y,p.z+palm.z-grip.z},g_set_position.method);
    return g_transform_get_parent.call(model,g_transform_get_parent.method)==wrist;
}
