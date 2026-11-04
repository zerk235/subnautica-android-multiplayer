// Called by the native SaveLoadManager.Update observer, never by a peer.
// A guest's accepted JOIN/receive state is deliberately unaffected.
bool net_session_invalidate_host_world() {
    pthread_mutex_lock(&g_session.lock);
    if(!g_session.local_host || !g_session.local_member) {
        pthread_mutex_unlock(&g_session.lock);return false;
    }
    g_session.active=false;g_session.local_member=false;g_session.local_host=false;
    g_session.local_has_world=false;g_session.join_accepted=false;
    g_session.selected=false;g_session.join_request_pending=false;
    g_session.host_id=0;g_session.epoch=0;g_session.slot[0]=0;
    g_session.content_guid_valid=false;g_session.resume_host_slot=false;
    memset(g_session.session_id,0,sizeof(g_session.session_id));
    memset(g_session.content_guid,0,sizeof(g_session.content_guid));
    g_session.host_world_gen_seen=0;g_session.world_refresh_pending=false;
    g_session.election_started=0;g_session.world_request_last=0;
    g_session.world_active=true; // blocks auto-joining an unrelated world during local load
    g_session.leave_cooldown_until=monotonic_seconds()+30.0;
    g_session.presence_dirty=true;
    session_set_state_locked(kSessionWaitingForWorld,kJoinAccepted,"host world changed; awaiting current save");
    pthread_mutex_unlock(&g_session.lock);
    LOG_INFO("session: retired old host world; next saved world creates a fresh session");
    return true;
}
bool session_world_servable(const char* slot) {
    pthread_mutex_lock(&g_session.lock);
    const bool ok=g_session.active && g_session.local_member && g_session.local_host &&
        g_session.local_has_world && (!slot || !strcmp(slot,g_session.slot));
    pthread_mutex_unlock(&g_session.lock);return ok;
}
