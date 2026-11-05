#include "modloader/src/net.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <pthread.h>

// Exercise the admission function used by pose, time, vehicle and event transport.
// Simulated time covers long sessions and recovery without running a game or writing saves.
constexpr unsigned kSessionFlagMember = 1, kSessionFlagHasWorld = 2;
PeerState g_peers[kMaxPeers]{};
struct RemotePresence {
    bool valid, protocol_v3;
    double last_presence;
    unsigned flags, epoch, host_id;
    uint8_t session_id[16];
} g_remote[kMaxPeers]{};
struct {
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    bool local_member = true, local_has_world = true;
    unsigned epoch = 1, host_id = 42;
    uint8_t session_id[16]{1};
} g_session;
double clock_now = 100;
double monotonic_seconds() { return clock_now; }
bool same_session(const uint8_t* a, const uint8_t* b) { return memcmp(a,b,16)==0; }
#include "modloader/src/session_peer_gate.inc"

void good_peer() {
    g_peers[0] = {};
    g_peers[0].active = true;
    g_peers[0].last_seen = clock_now;
    auto& remote = g_remote[0];
    remote = {};
    remote.valid = remote.protocol_v3 = true;
    remote.last_presence = clock_now;
    remote.flags = kSessionFlagMember | kSessionFlagHasWorld;
    remote.epoch = g_session.epoch;
    remote.host_id = g_session.host_id;
    memcpy(remote.session_id, g_session.session_id, 16);
}
void blocked(const char* expected) {
    const char* actual = session_peer_block_reason_locked(0,true);
    assert(actual && strcmp(actual,expected)==0);
    assert(!session_peer_eligible_locked(0,true));
}
int main() {
    good_peer();
    for (int second=0; second<600; ++second) {
        clock_now += 1;
        g_peers[0].last_seen = g_remote[0].last_presence = clock_now;
        assert(session_peer_eligible_locked(0,true));
    }
    g_peers[0].active=false; blocked("peer_inactive"); good_peer();
    assert(!session_peer_eligible_locked(kMaxPeers,true));
    g_peers[0].last_seen=clock_now-kPeerTimeout; assert(session_peer_eligible_locked(0,true));
    g_peers[0].last_seen-=.001; blocked("peer_timeout"); good_peer();
    g_remote[0].valid=false; blocked("presence_missing"); good_peer();
    g_remote[0].protocol_v3=false; blocked("protocol_mismatch"); good_peer();
    // Discovery/HELLO can stay fresh while presence has stopped.
    g_remote[0].last_presence=clock_now-kPeerTimeout-.001;
    blocked("presence_timeout"); good_peer();
    g_remote[0].flags=0; blocked("remote_not_member"); good_peer();
    g_remote[0].flags=kSessionFlagMember; blocked("remote_world_missing");
    assert(session_peer_eligible_locked(0,false)); good_peer();
    g_session.local_member=false; blocked("local_not_member"); g_session.local_member=true;
    g_session.local_has_world=false; blocked("local_world_missing");
    assert(session_peer_eligible_locked(0,false)); g_session.local_has_world=true;
    ++g_remote[0].epoch; blocked("epoch_mismatch"); good_peer();
    ++g_remote[0].host_id; blocked("host_mismatch"); good_peer();
    g_remote[0].session_id[1]=3; blocked("session_mismatch"); good_peer();
    // Recover on the next matching presence; never admit an unrelated world/session.
    assert(session_peer_eligible_locked(0,true));
    puts("PASS: actual session gate, 10-minute simulated session, each rejection and recovery");
}
