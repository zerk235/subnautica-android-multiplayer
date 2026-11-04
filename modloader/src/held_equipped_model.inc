// Apply FPModel's native equipped state only to owned visual children.
// Do this while the copied item root is inactive, before stripping scripts.
bool set_equipped_visual(void* root_tf,void* world_go,void* view_go) {
    if (!root_tf || !world_go || !view_go || world_go==view_go ||
        !g_get_go_transform || !g_set_active) return false;
    void* world=g_get_go_transform.call(world_go,g_get_go_transform.method);
    void* view=g_get_go_transform.call(view_go,g_get_go_transform.method);
    if (world==root_tf || view==root_tf || !prop_owns_transform(root_tf,world) ||
        !prop_owns_transform(root_tf,view) || prop_owns_transform(world,view) ||
        prop_owns_transform(view,world)) return false;
    g_set_active.call(world_go,false,g_set_active.method);
    g_set_active.call(view_go,true,g_set_active.method);
    return true;
}
