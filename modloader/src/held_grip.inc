bool prop_owns_transform(void* root,void* candidate) {
    if (!root || !candidate || !g_transform_get_parent) return false;
    for (unsigned depth=0;candidate && depth<64;++depth) {
        if (candidate==root) return true;
        candidate=g_transform_get_parent.call(candidate,g_transform_get_parent.method);
    }
    return false;
}

// A child socket can itself be offset/animated away from the visible wrist.
// Ownership does not make it the same spatial point as its parent hand bone.
void* visual_hand_anchor(void* root,void* bone,void* socket) {
    if (prop_owns_transform(root,bone)) return bone;
    if (prop_owns_transform(root,socket)) return socket;
    return root;
}

// A camera-socket tool's authored right-hand IK target is a wrist pose, not
// the camera origin. Null plugOrigin is legal in ModelPlug and denotes root.
void* select_visual_grip(void* root,void* plug,void* right_ik,int socket,uint32_t tech=0) {
    // Seaglide's socket=Hand but plugOrigin is its center mount. Its authored
    // right-hand IK transform identifies the handle and wrist pose instead.
    if ((socket==1 || tech==751) && prop_owns_transform(root,right_ik)) return right_ik;
    if (prop_owns_transform(root,plug)) return plug;
    return !plug ? root : nullptr;
}

bool plug_visual_to_hand(void* prop_tf,void* plug,void* hand) {
    if (!hand || !g_plug_visual || !prop_owns_transform(prop_tf,plug)) return false;
    try {
        // Native ModelPlug aligns the authored grip, including its rotation.
        // It is used only on attach/reparent; normal following is hierarchical.
        g_plug_visual.call(prop_tf,plug,hand,g_plug_visual.method);
        if (g_transform_get_parent.call(prop_tf,g_transform_get_parent.method)==hand) return true;
    } catch (...) {
        LOG_WARN("held grip: native attachment failed; using root fallback");
    }
    // A failed native call may already have reparented through a temporary object.
    // Keep the fallback visual under the real hand before that object is destroyed.
    if (g_set_parent) g_set_parent.call(prop_tf,hand,true,g_set_parent.method);
    return false;
}
