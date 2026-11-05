// Caller holds g_peers_lock; preserve the gameplay admission rules for all packet kinds.
const char* session_peer_block_reason_locked(size_t index, bool require_world) {
    if (index >= kMaxPeers || !g_peers[index].active) return "peer_inactive";
    const double now = monotonic_seconds();
    if (now - g_peers[index].last_seen > kPeerTimeout) return "peer_timeout";
    const RemotePresence& remote = g_remote[index];
    if (!remote.valid) return "presence_missing";
    if (!remote.protocol_v3) return "protocol_mismatch";
    if (now - remote.last_presence > kPeerTimeout) return "presence_timeout";
    if (!(remote.flags & kSessionFlagMember)) return "remote_not_member";
    if (require_world && !(remote.flags & kSessionFlagHasWorld)) return "remote_world_missing";
    pthread_mutex_lock(&g_session.lock);
    const char* reason = nullptr;
    if (!g_session.local_member) reason = "local_not_member";
    else if (require_world && !g_session.local_has_world) reason = "local_world_missing";
    else if (remote.epoch != g_session.epoch) reason = "epoch_mismatch";
    else if (remote.host_id != g_session.host_id) reason = "host_mismatch";
    else if (!same_session(remote.session_id, g_session.session_id)) reason = "session_mismatch";
    pthread_mutex_unlock(&g_session.lock);
    return reason;
}

bool session_peer_eligible_locked(size_t index, bool require_world) {
    return session_peer_block_reason_locked(index, require_world) == nullptr;
}
