#include "modloader/src/world.cpp"
#include "modloader/src/net.cpp"
#include <cassert>

void mod_logf(const char*, const char*, ...) {}
void net_note_session_role(bool, uint32_t) {}

static void check_policy() {
    using namespace menu_lobby;
    constexpr uint32_t host_id = 17;
    constexpr uint32_t guest_id = 29;
    Presence host{1, host_id, guest_id, 0, {}};
    Presence guest{1, host_id, guest_id, 0, {}};
    assert(valid({}, guest_id));
    assert(valid(host, host_id));
    assert(valid(guest, guest_id));
    assert(!can_start(host, guest));
    host.flags = kReady;
    assert(!can_start(host, guest));
    guest.flags = kReady;
    assert(can_start(host, guest));
    assert(!can_join_world(host, guest, guest_id));
    host.flags |= kStarted;
    assert(!can_start(host, guest));
    assert(can_join_world(host, guest, guest_id));
    assert(!can_join_world(host, guest, guest_id + 1));
    ++guest.room;
    assert(!can_join_world(host, guest, guest_id));
    assert(!can_start(host, guest));
    guest.flags |= kStarted;
    assert(!valid(guest, guest_id));
    host.reserved[2] = 1;
    assert(!valid(host, host_id));
}

static void receive(const SessionPresencePacket& packet) {
    sockaddr_in source{};
    source.sin_family = AF_INET;
    source.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    source.sin_port = htons(kLanPort);
    handle_packet(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet), source, monotonic_seconds());
}

int main() {
    check_policy();
    pthread_once(&g_peers_lock_once, init_peers_lock);

    g_local_id = 10;
    net_lobby_open();
    net_lobby_create();
    SessionPresencePacket host{};
    local_session_copy(&host);
    assert(host.lobby.room && host.lobby.host == 10 && host.lobby.guest == 0);
    auto host_state = g_lobby;

    g_local_id = 20;
    g_lobby = {};
    net_lobby_open();
    receive(host);
    LobbyStatus view{};
    net_lobby_snapshot(&view);
    assert(view.view == kLobbyBrowse && view.count == 1 && view.rooms[0].room == host.lobby.room);
    net_lobby_enter(10, host.lobby.room);
    net_lobby_toggle_ready(); // Cannot ready up before the host reserves the seat.
    assert(g_lobby.presence.flags == 0);
    SessionPresencePacket guest{};
    local_session_copy(&guest);
    auto guest_state = g_lobby;

    g_local_id = 10;
    g_lobby = host_state;
    receive(guest);
    net_lobby_menu_tick(true, false);
    net_lobby_toggle_ready();
    net_lobby_start(); // One ready player is insufficient.
    net_lobby_snapshot(&view);
    assert(view.view == kLobbyHost && !view.can_start && view.host_ready && view.guest == 20);
    local_session_copy(&host);
    host_state = g_lobby;

    g_local_id = 20;
    g_lobby = guest_state;
    receive(host);
    net_lobby_toggle_ready();
    net_lobby_snapshot(&view);
    assert(view.view == kLobbyGuest && view.guest_ready && view.host_ready);
    local_session_copy(&guest);
    guest_state = g_lobby;

    g_local_id = 10;
    g_lobby = host_state;
    receive(guest);
    net_lobby_snapshot(&view);
    assert(view.can_start);
    net_lobby_start();
    net_lobby_snapshot(&view);
    assert(view.view == kLobbyChooseWorld && !view.can_start);
    local_session_copy(&host);
    host_state = g_lobby;

    g_local_id = 20;
    g_lobby = guest_state;
    receive(host);
    net_lobby_menu_tick(true, false);
    net_lobby_snapshot(&view);
    assert(view.view == kLobbyWaiting);
    assert(!g_session.join_request_pending); // No playable host save yet.

    g_local_id = 10;
    g_lobby = host_state;
    net_lobby_close();
    local_session_copy(&host);
    g_local_id = 20;
    g_lobby = guest_state;
    receive(host);
    net_lobby_menu_tick(true, false);
    net_lobby_snapshot(&view);
    assert(view.view == kLobbyBrowse && view.room_lost);
}
