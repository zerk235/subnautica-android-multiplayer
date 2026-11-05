// Two-handed tools must not be parented to either solved hand: that forms an
// IK feedback loop. Follow the owner's tool frame under the body, then let
// ArmsController's world targets place both wrists on the authored handles.
void clear_held_body_ik(RemoteAvatar* avatar) {
    if (avatar->held_world_ik_bound && avatar->held_arms && g_arms_world_targets)
        g_arms_world_targets.call(avatar->held_arms,nullptr,nullptr,g_arms_world_targets.method);
    avatar->held_world_ik_bound=false;
}

bool prepare_held_body_pose(RemoteAvatar* avatar,void* root,void* left,void* right,uint32_t tech) {
    if (tech!=751 || !avatar->transform || !g_arms_type_obj || !g_arms_world_targets ||
        !g_set_parent || !g_set_position || !g_set_rotation || !g_set_active || !g_get_go_transform ||
        !prop_owns_transform(root,left) || !prop_owns_transform(root,right)) return false;
    void* arms=find_component_in_hierarchy(avatar->game_object,g_arms_type_obj,g_arms_name_str,0);
    if (!arms) return false;
    avatar->held_arms=arms;avatar->held_left_ik=left;avatar->held_right_ik=right;
    g_set_parent.call(root,avatar->transform,false,g_set_parent.method);
    return true;
}

void apply_held_body_pose(RemoteAvatar* avatar,const PeerState& peer) {
    HeldRotation q{};
    bool valid=peer.held_tech==751 && avatar->held_tech==751 &&
        (peer.flags&kFlagHeldWorldPose) && peer.held_rotation_valid &&
        held_offset_valid(peer.held_offset) && held_rotation_normalize(peer.held_rotation,&q);
    g_set_active.call(avatar->held_prop,valid,g_set_active.method);
    if (!valid) {clear_held_body_ik(avatar);return;}
    void* tf=g_get_go_transform.call(avatar->held_prop,g_get_go_transform.method);
    const auto& p=peer.held_offset;
    g_set_position.call(tf,Vector3{peer.x+p.x,peer.y+p.y,peer.z+p.z},g_set_position.method);
    g_set_rotation.call(tf,Quaternion{q.x,q.y,q.z,q.w},g_set_rotation.method);
    if (!avatar->held_world_ik_bound) {
        g_arms_world_targets.call(avatar->held_arms,avatar->held_left_ik,avatar->held_right_ik,g_arms_world_targets.method);
        avatar->held_world_ik_bound=true;
    }
    if ((g_avatar_frame%120)==0)
        LOG_INFO("held body pose: peer=%08x tech=%u offset=(%.3f,%.3f,%.3f) twoHands=1",avatar->id,peer.held_tech,p.x,p.y,p.z);
}
