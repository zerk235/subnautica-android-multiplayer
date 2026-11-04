// Resolve the hand from bones that actually deform this clone's visible mesh.
// ArmsController's attach/hand fields can be independent animation targets.
bool visual_right_hand_name(const char* name) {
    char normalized[128]{};
    size_t used=0;
    for (;name && *name && used+1<sizeof(normalized);++name) {
        char c=*name;
        if (c>='A' && c<='Z') c=char(c-'A'+'a');
        if ((c>='a' && c<='z') || (c>='0' && c<='9')) normalized[used++]=c;
    }
    const char* names[]={"righthand","handright","handr","rhand",
                         "bip01rhand","bip001rhand","mixamorigrighthand","defhandr"};
    for (const char* expected:names)
        if (strcmp(normalized,expected)==0) return true;
    return false;
}

void* find_visual_right_hand(void* root_go,void* root_tf,void* preferred) {
    if (g_get_component_is_string || !root_go || !root_tf || !g_smr_type_obj || !g_smr_get_bones ||
        !g_array_get_length || !g_array_get_value || !g_get_component ||
        !g_get_go_transform || !g_get_child || !g_get_child_count ||
        !g_get_game_object || !g_get_name) return nullptr;
    void* stack[512]; size_t top=0,visited=0;
    stack[top++]=root_go;
    void* match=nullptr;
    bool ambiguous=false,preferred_seen=false;
    while (top && visited++<512) {
        void* go=stack[--top];
        void* smr=g_get_component.call(go,g_smr_type_obj,g_get_component.method);
        void* bones=smr ? g_smr_get_bones.call(smr,g_smr_get_bones.method) : nullptr;
        int32_t count=bones ? g_array_get_length.call(bones,g_array_get_length.method) : 0;
        if (count>=0 && count<=512) for (int32_t i=0;i<count;++i) {
            void* bone=g_array_get_value.call(bones,i,g_array_get_value.method);
            if (!prop_owns_transform(root_tf,bone)) continue;
            if (bone==preferred) preferred_seen=true;
            void* bone_go=g_get_game_object.call(bone,g_get_game_object.method);
            char name[128]{};
            if (!bone_go || !read_managed_name(g_get_name.call(bone_go,g_get_name.method),name,sizeof(name)) ||
                !visual_right_hand_name(name)) continue;
            if (match && match!=bone) ambiguous=true;
            else match=bone;
        }
        void* tf=g_get_go_transform.call(go,g_get_go_transform.method);
        int32_t children=tf ? g_get_child_count.call(tf,g_get_child_count.method) : 0;
        for (int32_t i=0;i<children && top<512;++i) {
            void* child=g_get_child.call(tf,i,g_get_child.method);
            void* child_go=child ? g_get_game_object.call(child,g_get_game_object.method) : nullptr;
            if (child_go) stack[top++]=child_go;
        }
    }
    // Incomplete traversal cannot establish a unique skeleton.
    if (top) return nullptr;
    return preferred_seen ? preferred : ambiguous ? nullptr : match;
}
