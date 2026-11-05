// Test the real packet encoder/receiver, counters and socket errors. No game process,
// broadcast, pump thread or save write is involved; UDP stays on this device's loopback.
#define SNMOD_BASE_GEOMETRY_CAPTURE
#define SNMOD_WORLD_TEST_ROOT "/data/local/tmp/snmod_pose_transport_tests"
#include "modloader/src/world.cpp"
#include "modloader/src/net.cpp"
#include <cassert>

void mod_logf(const char*, const char*, ...) {}
void net_note_session_role(bool, uint32_t) {}

void matching_presence() {
    auto& remote = g_remote[0];
    remote.valid = remote.protocol_v3 = true;
    remote.flags = kSessionFlagMember | kSessionFlagHasWorld;
    remote.last_presence = monotonic_seconds();
    remote.epoch = g_session.epoch;
    remote.host_id = g_session.host_id;
    memcpy(remote.session_id, g_session.session_id, 16);
}
int main() {
    pthread_once(&g_peers_lock_once, init_peers_lock);
    g_local_id = 1;
    g_session.local_member = g_session.local_has_world = true;
    g_session.epoch = 1;
    g_session.host_id = 1;
    g_session.session_id[0] = 7;
    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    assert(receiver >= 0);
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(receiver, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == 0);
    socklen_t length = sizeof(endpoint);
    assert(getsockname(receiver, reinterpret_cast<sockaddr*>(&endpoint), &length) == 0);
    timeval timeout{2,0};
    assert(setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    g_socket = socket(AF_INET, SOCK_DGRAM, 0);
    assert(g_socket >= 0);
    PeerState* peer = claim_peer(2, endpoint, monotonic_seconds());
    assert(peer == &g_peers[0]);
    matching_presence();
    LocalState local{};
    local.valid = true;
    local.x = 12.5f; local.y = -8; local.z = 100;
    local.flags = kFlagSwimming;
    int error = -1;
    assert(send_state(*peer,local,&error) == sizeof(StatePacket) && error == 0);
    StatePacket packet{};
    assert(recv(receiver,&packet,sizeof(packet),0) == sizeof(packet));
    assert(packet.id == 1 && packet.x == local.x && packet.flags == local.flags);
    packet.id = 2;
    auto receive = [&] {
        handle_packet(reinterpret_cast<uint8_t*>(&packet),sizeof(packet),endpoint,
                      monotonic_seconds());
    };
    receive();
    assert(g_pose_stream[0].rx_seen == 1 && g_pose_stream[0].rx_applied == 1);
    assert(peer->x == 12.5f);
    receive();
    assert(g_pose_stream[0].rx_seen == 2 && g_pose_stream[0].rx_stale == 1);
    packet.sequence++;
    g_remote[0].flags &= ~kSessionFlagHasWorld;
    receive();
    assert(g_pose_stream[0].rx_blocked == 1);
    assert(strcmp(g_pose_stream[0].last_rx_block,"remote_world_missing") == 0);
    matching_presence(); receive();
    assert(g_pose_stream[0].rx_applied == 2 && peer->sequence == packet.sequence);
    // Removing the missing-world cause resumes immediately. A foreign session stays blocked.
    g_remote[0].session_id[1] = 9;
    packet.sequence++; receive();
    assert(g_pose_stream[0].rx_blocked == 2);
    assert(strcmp(g_pose_stream[0].last_rx_block,"session_mismatch") == 0);
    matching_presence(); receive();
    assert(g_pose_stream[0].rx_applied == 3);
    close(g_socket); g_socket = -1;
    assert(send_state(*peer,local,&error) == -1 && error == EBADF);
    // A timed-out slot starts a new stream; old counters/sequence must not leak on reuse.
    peer->last_seen = monotonic_seconds() - kPeerTimeout - 1;
    drop_stale_peers(monotonic_seconds());
    assert(!peer->active);
    peer = claim_peer(2,endpoint,monotonic_seconds()); matching_presence();
    assert(g_pose_stream[0].rx_seen == 0 && !peer->seq_valid);
    packet.sequence = 1; receive();
    assert(g_pose_stream[0].rx_applied == 1 && peer->sequence == 1);
    close(receiver);
    puts("PASS: real UDP pose roundtrip, actual receive/reorder/session gates, recovery, socket error and slot reuse");
}
