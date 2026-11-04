// Only change the copied visual. Updating its local rotation before Animator/IK
// is safe: the wrist hierarchy still supplies the final world pose later.
bool apply_held_rotation_pose(void* model,void* plug,void* wrist,const Vector3& palm_local,
                              const Vector3& handle,HeldRotation received) {
    HeldRotation q{};
    if (!model || !plug || !wrist || !g_held_set_local_rotation || !g_held_transform_point ||
        !g_get_position || !g_set_position || !g_transform_get_parent ||
        !held_rotation_normalize(received,&q) || !prop_owns_transform(model,plug) ||
        g_transform_get_parent.call(model,g_transform_get_parent.method)!=wrist) return false;
    g_held_set_local_rotation.call(model,Quaternion{q.x,q.y,q.z,q.w},g_held_set_local_rotation.method);
    Vector3 target=g_held_transform_point.call(wrist,palm_local,g_held_transform_point.method);
    Vector3 grip=visual_grip_position(model,plug,handle);
    Vector3 p=g_get_position.call(model,g_get_position.method);
    g_set_position.call(model,Vector3{p.x+target.x-grip.x,p.y+target.y-grip.y,p.z+target.z-grip.z},g_set_position.method);
    return true;
}
