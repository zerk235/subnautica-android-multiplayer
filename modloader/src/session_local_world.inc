void net_session_set_local_world(bool has_world, const char* slot,
                                 const uint8_t content_guid[16]) {
    if (!has_world) {
        pthread_mutex_lock(&g_session.lock);
        g_session.world_active = false;
        if (g_session.local_host) {
            g_session.local_host = false;
            g_session.host_id = 0;
        }
        g_session.local_has_world = false;
        if (g_session.local_member) {
            g_session.election_started = monotonic_seconds();
            session_set_state_locked(kSessionWaitingForWorld, kJoinAccepted,
                                     "local world unloaded");
        }
        g_session.presence_dirty = true;
        pthread_mutex_unlock(&g_session.lock);
        return;
    }

    if (!slot || !*slot || strlen(slot) >= sizeof(g_session.slot) ||
        !content_guid || !bytes_nonzero(content_guid, 16)) {
        LOG_ERROR("session: refusing world context with invalid slot/content GUID");
        return;
    }
    bool received_from_host = world_slot_is_host_received(slot);
    pthread_mutex_lock(&g_session.lock);
    g_session.world_active = true;
    if (g_session.active && g_session.local_member) {
        if (g_session.local_host) {
            // Host is the world authority: an autosave that cycles the world guid is a
            // legitimate host-side update (the host_offer_refresh_tick republish path),
            // never a conflict - adopting it quietly keeps the host demote-free.
            memcpy(g_session.content_guid, content_guid, sizeof(g_session.content_guid));
            g_session.content_guid_valid = true;
        } else if (g_session.content_guid_valid &&
                   !same_content(g_session.content_guid, content_guid)) {
            session_set_state_locked(kSessionConflict, kJoinStaleSession,
                                     "loaded world content GUID conflicts with selected session");
            g_session.local_has_world = false;
            pthread_mutex_unlock(&g_session.lock);
            return;
        }
        if (g_session.slot[0] && strcmp(g_session.slot, slot) != 0) {
            session_set_state_locked(kSessionConflict, kJoinStaleSession,
                                     "loaded slot name differs from session slot");
            g_session.local_has_world = false;
            pthread_mutex_unlock(&g_session.lock);
            return;
        }
        g_session.local_has_world = true;
        g_session.resume_host_slot = false;
        memcpy(g_session.content_guid, content_guid, sizeof(g_session.content_guid));
        g_session.content_guid_valid = true;
        snprintf(g_session.slot, sizeof(g_session.slot), "%s", slot);
        if (g_session.local_host) {
            session_set_state_locked(kSessionHost, kJoinAccepted, "host");
        } else if (g_session.host_id) {
            session_set_state_locked(kSessionGuest, kJoinAccepted, "guest");
        } else {
            session_set_state_locked(kSessionWaitingForWorld, kJoinAccepted,
                                     "world loaded; awaiting host election");
            if (g_session.election_started == 0.0)
                g_session.election_started = monotonic_seconds() -
                                             kElectionConvergenceSeconds;
        }
        g_session.presence_dirty = true;
        pthread_mutex_unlock(&g_session.lock);
        return;
    }

    if (!g_session.active && !g_session.local_member && g_session.launch_role==kLaunchGuest) {
        g_session.local_has_world=false;g_session.local_host=false;
        session_set_state_locked(kSessionConflict,kJoinStaleSession,"guest role: return to menu to join a host");
        pthread_mutex_unlock(&g_session.lock);
        LOG_WARN("session: guest launch cannot create a host from a manually loaded world");
        return;
    }
    if (!g_session.active && !g_session.local_member && received_from_host && g_session.launch_role!=kLaunchHost) {
        // The save can outlive the process that accepted its JOIN. Do not turn that guest's
        // downloaded world into a new host session on restart. Keep it loaded locally, but
        // advertise no session/world membership until it has returned to menu and rejoined
        // the matching host. This preserves the host as the save authority.
        g_session.local_has_world = false;
        g_session.local_host = false;
        g_session.resume_host_slot = true;
        memcpy(g_session.content_guid, content_guid, sizeof(g_session.content_guid));
        g_session.content_guid_valid = true;
        snprintf(g_session.slot, sizeof(g_session.slot), "%s", slot);
        if (!g_session.selected && !g_session.join_request_pending) {
            g_session.join_accepted = false;
            g_session.host_id = 0;
            g_session.epoch = 0;
            memset(g_session.session_id, 0, sizeof(g_session.session_id));
            session_set_state_locked(kSessionDiscovering, kJoinAccepted,
                                     "host-received world; waiting to rejoin its session");
        }
        g_session.presence_dirty = true;
        pthread_mutex_unlock(&g_session.lock);
        LOG_WARN("session: loaded host-received slot %s without JOIN state; "
                 "waiting for its host instead of creating a new session", slot);
        return;
    }

    g_session.active = true;
    g_session.local_member = true;
    g_session.local_has_world = true;
    g_session.local_host = true;
    g_session.resume_host_slot = false;
    g_session.epoch = 1;
    g_session.host_id = g_local_id;
    memset(g_session.session_id, 0, sizeof(g_session.session_id));
    random_bytes(g_session.session_id, sizeof(g_session.session_id));
    if (!bytes_nonzero(g_session.session_id, sizeof(g_session.session_id)))
        g_session.session_id[0] = 1;
    memcpy(g_session.content_guid, content_guid, sizeof(g_session.content_guid));
    g_session.content_guid_valid = true;
    snprintf(g_session.slot, sizeof(g_session.slot), "%s", slot);
    g_session.selected = false;
    g_session.join_accepted = false;
    g_session.join_request_pending = false;
    g_session.last_host_presence = monotonic_seconds();
    g_session.election_started = 0.0;
    g_session.presence_dirty = true;
    session_set_state_locked(kSessionHost, kJoinAccepted, "new host session");
    pthread_mutex_unlock(&g_session.lock);
    LOG_INFO("session: created host session epoch=1 slot=%s content-guid=%02x%02x%02x%02x",
             slot, content_guid[0], content_guid[1], content_guid[2], content_guid[3]);
}

