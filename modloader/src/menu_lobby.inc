namespace {

bool lobby_peer_visible(int index, double now) {
    if (index < 0) return false;
    const auto& peer = g_peers[index];
    const auto& remote = g_remote[index];
    return peer.active && remote.valid && remote.protocol_v3 &&
        now - remote.last_presence <= kPeerTimeout &&
        !strcmp(remote.game_build, kGameBuild) && !strcmp(remote.mod_version, kModVersion);
}

bool lobby_peer_in_room(int index, double now) {
    return lobby_peer_visible(index, now) &&
        menu_lobby::same_room(g_lobby.presence, g_remote[index].lobby);
}

void lobby_leave_guest_locked() {
    if (!g_session.selected || g_session.local_host) return;
    g_session.explicit_leave = true;
    g_session.selected = false;
    g_session.join_request_pending = false;
    g_session.join_accepted = false;
}

// Caller holds the peer lock, then the session lock.
void lobby_refresh_locked(double now) {
    auto& room = g_lobby.presence;
    if (!room.room) return;
    if (room.host == g_local_id) {
        if (room.flags & menu_lobby::kStarted) return;
        if (room.guest && !lobby_peer_in_room(peer_index_by_id(room.guest), now)) {
            room.guest = 0;
            room.flags = 0;
            g_session.presence_dirty = true;
        }
        if (room.guest) return;
        for (size_t i = 0; i < kMaxPeers; ++i) {
            if (lobby_peer_in_room(i, now) && g_remote[i].lobby.guest == g_peers[i].id) {
                room.guest = g_peers[i].id;
                g_session.presence_dirty = true;
                break;
            }
        }
        return;
    }

    const int index = peer_index_by_id(room.host);
    if (!lobby_peer_in_room(index, now) ||
        (g_remote[index].lobby.guest && g_remote[index].lobby.guest != g_local_id)) {
        room = {};
        g_lobby.room_lost = true;
        lobby_leave_guest_locked();
        g_session.presence_dirty = true;
    }
}

void lobby_reset_locked() {
    if (g_lobby.open) g_session.launch_role = g_lobby.previous_role;
    g_lobby = {};
    g_session.presence_dirty = true;
}

}  // namespace

void net_lobby_open() {
    pthread_mutex_lock(&g_session.lock);
    const auto role = g_session.launch_role;
    pthread_mutex_unlock(&g_session.lock);
    if (!net_session_choose_role(kLaunchAuto)) return;
    pthread_mutex_lock(&g_session.lock);
    lobby_reset_locked();
    g_lobby.open = true;
    g_lobby.previous_role = role;
    pthread_mutex_unlock(&g_session.lock);
}

void net_lobby_create() {
    uint32_t room = 0;
    if (!g_local_id || !random_bytes(reinterpret_cast<uint8_t*>(&room), sizeof(room)) || !room) return;
    pthread_mutex_lock(&g_session.lock);
    if (g_lobby.open && !g_lobby.presence.room && !g_session.world_active) {
        g_lobby.presence = {room, g_local_id, 0, 0, {}};
        g_lobby.room_lost = false;
        g_session.presence_dirty = true;
        LOG_INFO("lobby: created room %08x", room);
    }
    pthread_mutex_unlock(&g_session.lock);
}

void net_lobby_enter(uint32_t host, uint32_t room) {
    if (!room || !host || host == g_local_id) return;
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    const int index = peer_index_by_id(host);
    pthread_mutex_lock(&g_session.lock);
    if (g_lobby.open && !g_lobby.presence.room && !g_session.world_active &&
        lobby_peer_visible(index, monotonic_seconds())) {
        const auto& advertised = g_remote[index].lobby;
        if (advertised.host == host && advertised.room == room && !advertised.guest &&
            !(advertised.flags & menu_lobby::kStarted)) {
            g_lobby.presence = {room, host, g_local_id, 0, {}};
            g_lobby.room_lost = false;
            g_session.presence_dirty = true;
            LOG_INFO("lobby: requested seat at host=%08x room=%08x", host, room);
        }
    }
    pthread_mutex_unlock(&g_session.lock);
    pthread_mutex_unlock(&g_peers_lock);
}

void net_lobby_snapshot(LobbyStatus* out) {
    if (!out) return;
    *out = {};
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    pthread_mutex_lock(&g_session.lock);
    const double now = monotonic_seconds();
    const auto& room = g_lobby.presence;
    if (g_lobby.open && !room.room) {
        out->view = kLobbyBrowse;
        out->room_lost = g_lobby.room_lost;
        for (size_t i = 0; i < kMaxPeers; ++i) {
            const auto& remote = g_remote[i].lobby;
            if (lobby_peer_visible(i, now) && remote.room && remote.host == g_peers[i].id &&
                !remote.guest && !(remote.flags & menu_lobby::kStarted)) {
                out->rooms[out->count++] = {remote.host, remote.room};
            }
        }
    } else if (room.room) {
        out->room = room.room;
        out->host = room.host;
        const bool hosting = room.host == g_local_id;
        const int partner = peer_index_by_id(hosting ? room.guest : room.host);
        const menu_lobby::Presence remote = lobby_peer_in_room(partner, now)
            ? g_remote[partner].lobby : menu_lobby::Presence{};
        out->guest = hosting ? room.guest : (remote.guest == g_local_id ? g_local_id : 0);
        out->host_ready = ((hosting ? room.flags : remote.flags) & menu_lobby::kReady) != 0;
        out->guest_ready = out->guest && (((hosting ? remote.flags : room.flags) & menu_lobby::kReady) != 0);
        out->view = hosting ? ((room.flags & menu_lobby::kStarted) ? kLobbyChooseWorld : kLobbyHost)
                            : ((remote.flags & menu_lobby::kStarted) ? kLobbyWaiting : kLobbyGuest);
        out->can_start = hosting && menu_lobby::can_start(room, remote);
    }
    pthread_mutex_unlock(&g_session.lock);
    pthread_mutex_unlock(&g_peers_lock);
}

void net_lobby_toggle_ready() {
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    pthread_mutex_lock(&g_session.lock);
    const double now = monotonic_seconds();
    lobby_refresh_locked(now);
    auto& room = g_lobby.presence;
    const bool hosting = room.host == g_local_id;
    const int host = peer_index_by_id(room.host);
    const bool guest_admitted = !hosting && lobby_peer_in_room(host, now) &&
        g_remote[host].lobby.guest == g_local_id && !(g_remote[host].lobby.flags & menu_lobby::kStarted);
    if (room.room && !g_session.world_active && ((hosting && !(room.flags & menu_lobby::kStarted)) || guest_admitted)) {
        room.flags ^= menu_lobby::kReady;
        g_session.presence_dirty = true;
    }
    pthread_mutex_unlock(&g_session.lock);
    pthread_mutex_unlock(&g_peers_lock);
}

void net_lobby_start() {
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    pthread_mutex_lock(&g_session.lock);
    const double now = monotonic_seconds();
    lobby_refresh_locked(now);
    auto& room = g_lobby.presence;
    const int guest = peer_index_by_id(room.guest);
    if (room.host == g_local_id && !g_session.world_active && lobby_peer_in_room(guest, now) &&
        menu_lobby::can_start(room, g_remote[guest].lobby)) {
        room.flags |= menu_lobby::kStarted;
        g_session.launch_role = kLaunchHost;
        g_session.presence_dirty = true;
        LOG_INFO("lobby: host started room=%08x guest=%08x; choose a world", room.room, room.guest);
    }
    pthread_mutex_unlock(&g_session.lock);
    pthread_mutex_unlock(&g_peers_lock);
}

void net_lobby_close() {
    pthread_mutex_lock(&g_session.lock);
    if (!g_session.world_active) {
        lobby_reset_locked();
        lobby_leave_guest_locked();
    }
    pthread_mutex_unlock(&g_session.lock);
}

void net_lobby_menu_tick(bool at_menu, bool playing) {
    if (!at_menu) {
        if (playing) {
            pthread_mutex_lock(&g_session.lock);
            if (g_lobby.open) g_lobby.entered_world = true;
            pthread_mutex_unlock(&g_session.lock);
        }
        return;
    }

    uint32_t join_host = 0;
    uint8_t join_session[16]{};
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    pthread_mutex_lock(&g_session.lock);
    if (g_lobby.entered_world) lobby_reset_locked();
    if (g_lobby.open) {
        const double now = monotonic_seconds();
        lobby_refresh_locked(now);
        const auto& room = g_lobby.presence;
        const int host = peer_index_by_id(room.host);
        if (room.room && room.host != g_local_id && lobby_peer_in_room(host, now)) {
            const auto& remote = g_remote[host];
            if ((remote.lobby.flags & menu_lobby::kStarted) && remote.lobby.guest == g_local_id) {
                g_session.launch_role = kLaunchGuest;
                if (!g_session.active && !g_session.explicit_leave && !g_session.join_request_pending &&
                    !g_session.join_accepted && now >= g_session.join_retry_after &&
                    (remote.flags & kSessionFlagIsHost) &&
                    menu_lobby::can_join_world(remote.lobby, room, g_local_id)) {
                    join_host = room.host;
                    memcpy(join_session, remote.session_id, sizeof(join_session));
                }
            }
        }
    }
    pthread_mutex_unlock(&g_session.lock);
    pthread_mutex_unlock(&g_peers_lock);
    if (join_host) net_session_join(join_host, join_session);
}
