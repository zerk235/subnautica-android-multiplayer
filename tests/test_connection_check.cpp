#include <cassert>

#define SNMOD_BASE_GEOMETRY_CAPTURE
#include "modloader/src/world.cpp"
#include "modloader/src/net.cpp"

void mod_logf(const char*, const char*, ...) {}
void net_note_session_role(bool, uint32_t) {}

using namespace connection_check;

void expect_notice(WorldNotice expected) {
    WorldNotice actual;
    assert(net_take_world_notice(&actual) && actual == expected);
    assert(!net_take_world_notice(&actual));
}

void expect_silence() {
    WorldNotice notice;
    assert(!net_take_world_notice(&notice));
}

int main() {
    constexpr uint32_t local = 0xc0a82b02;       // 192.168.43.2
    constexpr uint32_t mask = 0xffffff00;
    constexpr uint32_t hotspot = 0xc0a82b01;     // 192.168.43.1
    constexpr uint32_t other_wifi = 0xc0a80101;  // 192.168.1.1
    assert(same_subnet(local, mask, hotspot));
    assert(!same_subnet(local, mask, other_wifi));
    assert(!same_subnet(local, 0, hotspot));
    assert(same_subnet(local, 0xffff0000, other_wifi));

    VisiblePeers peers{};
    assert(diagnose(false, peers) == Discovery::NoLanAddress);
    assert(diagnose(true, peers) == Discovery::NoPeers);
    peers.total = 1;
    peers.incompatible = 1;
    assert(diagnose(true, peers) == Discovery::IncompatibleHost);
    peers.incompatible = 0;
    peers.same_subnet_peers = 1;
    assert(diagnose(true, peers) == Discovery::HostNotReady);
    peers.hosts = 1;
    peers.compatible_hosts = 1;
    // A different peer on our network cannot hide the target host's subnet mismatch.
    peers.total = 2;
    assert(diagnose(true, peers) == Discovery::OtherSubnet);
    peers.same_subnet_hosts = 1;
    assert(diagnose(true, peers) == Discovery::JoinUnanswered);

    Wait wait;
    assert(!wait.update(Stage::GuestDiscovery, 0, 1));
    assert(!wait.update(Stage::GuestDiscovery, 0, 30.99));
    assert(wait.update(Stage::GuestDiscovery, 0, 31));
    assert(!wait.update(Stage::GuestDiscovery, 0, 600));
    assert(!wait.update(Stage::GuestTransfer, 0, 601));
    assert(!wait.update(Stage::GuestTransfer, 32, 630));
    assert(!wait.update(Stage::GuestTransfer, 32, 631));
    assert(wait.update(Stage::GuestTransfer, 32, 660));
    assert(!wait.update(Stage::GuestLoad, 0, 661));
    assert(!wait.update(Stage::Idle, 0, 670));
    assert(!wait.update(Stage::Idle, 0, 705));
    assert(!wait.update(Stage::HostWaiting, 0, 706));
    assert(wait.update(Stage::HostWaiting, 0, 736));

    const double now = monotonic_seconds();
    pthread_once(&g_peers_lock_once, init_peers_lock);
    connection_check_tick(now);
    connection_check_tick(now + 31);
    expect_silence(); // default role and no session does not warn solo players

    g_session.launch_role = kLaunchGuest;
    connection_check_tick(now + 32);
    connection_check_tick(now + 61);
    expect_silence();
    connection_check_tick(now + 62);
    WorldNotice offline;
    assert(net_take_world_notice(&offline));
    assert(offline == WorldNotice::GuestNoLan || offline == WorldNotice::GuestNoPeers);
    connection_check_tick(now + 65);
    expect_silence();

    g_session.join_accepted = true;
    g_session.host_id = g_connection_progress_host = 7;
    g_handoff_state = kWorldHandoffReceiving;
    connection_check_tick(now + 66);
    g_connection_received_chunks = 3;
    connection_check_tick(now + 91);
    connection_check_tick(now + 96);
    expect_silence();
    connection_check_tick(now + 121);
    expect_notice(WorldNotice::GuestTransferStalled);
    g_handoff_state = kWorldHandoffWritten;
    connection_check_tick(now + 123);
    connection_check_tick(now + 153);
    expect_notice(WorldNotice::GuestLoadStalled);
    g_session.local_has_world = true;
    connection_check_tick(now + 154);
    connection_check_tick(now + 185);
    expect_silence();

    g_session.launch_role = kLaunchHost;
    g_session.local_member = g_session.local_host = true;
    g_session.session_id[0] = 1;
    connection_check_tick(now + 186);
    connection_check_tick(now + 216);
    expect_notice(WorldNotice::HostNoGuest);

    g_peers[0].active = true;
    g_peers[0].last_seen = now + 250;
    g_remote[0].join_accepted = true;
    g_remote[0].admitted_session[0] = 1;
    connection_check_tick(now + 250);
    connection_check_tick(now + 253);
    expect_silence();
    g_peers[0].active = false;
    connection_check_tick(now + 256);
    connection_check_tick(now + 286);
    expect_notice(WorldNotice::HostNoGuest);
}
