// Only the game's main-menu observer calls this. In-flight JOIN replies become
// ineligible immediately; the pump performs its normal leave/receive cleanup.
bool net_session_choose_role(SessionLaunchRole role) {
    if (!session_launch_role_valid(role)) return false;
    pthread_mutex_lock(&g_session.lock);
    if (g_session.world_active) {pthread_mutex_unlock(&g_session.lock);return false;}
    g_session.launch_role=role;
    g_session.role_reset_pending=true;
    g_session.explicit_leave=true;
    g_session.selected=false;
    g_session.join_request_pending=false;
    g_session.join_accepted=false;
    g_session.presence_dirty=true;
    pthread_mutex_unlock(&g_session.lock);
    LOG_INFO("session: explicit launch role=%s queued in main menu",role==kLaunchHost ? "host" : role==kLaunchGuest ? "guest" : "auto");
    return true;
}
