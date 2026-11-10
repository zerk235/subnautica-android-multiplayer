// Real encoder/decoder and UDP on device loopback; no game, save or Wi-Fi changes.
#define SNMOD_BASE_GEOMETRY_CAPTURE
#define SNMOD_WORLD_TEST_ROOT "/data/local/tmp/snmod_event_recovery_tests"
#include "modloader/src/world.cpp"
#include "modloader/src/net.cpp"
#include <cassert>
#include <vector>
#include <algorithm>

void mod_logf(const char*, const char*, ...) {}
void net_note_session_role(bool, uint32_t) {}

struct Device {
    uint32_t id;
    int socket;
    sockaddr_in address{};
    EventRing events{};
    WorldEvent inbox[kEventInboxSize]{};
    int head = 0, tail = 0;
};
Device a{}, b{}, c{};
Device* current = nullptr;

void open_device(Device& d, uint32_t id) {
    d.id = id;
    d.socket = socket(AF_INET, SOCK_DGRAM, 0);
    assert(d.socket >= 0);
    d.address.sin_family = AF_INET;
    d.address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(d.socket, reinterpret_cast<sockaddr*>(&d.address), sizeof(d.address)) == 0);
    socklen_t length = sizeof(d.address);
    assert(getsockname(d.socket, reinterpret_cast<sockaddr*>(&d.address), &length) == 0);
    assert(fcntl(d.socket, F_SETFL, O_NONBLOCK) == 0);
}
void activate(Device& d, Device& other, int peer_slot = 0) {
    if (current) {
        current->events = g_events;
        memcpy(current->inbox, g_event_inbox, sizeof(g_event_inbox));
        current->head = g_event_inbox_head; current->tail = g_event_inbox_tail;
    }
    current = &d;
    g_local_id = d.id; g_socket = d.socket;
    g_events = d.events;
    memcpy(g_event_inbox, d.inbox, sizeof(g_event_inbox));
    g_event_inbox_head = d.head; g_event_inbox_tail = d.tail;
    memset(g_peers, 0, sizeof(g_peers)); memset(g_remote, 0, sizeof(g_remote));
    auto& peer = g_peers[peer_slot];
    peer.active = true; peer.id = other.id; peer.raw = other.address;
    peer.last_seen = monotonic_seconds();
    auto& remote = g_remote[peer_slot];
    remote.valid = remote.protocol_v3 = true;
    remote.flags = kSessionFlagMember | kSessionFlagHasWorld;
    remote.last_presence = monotonic_seconds(); remote.epoch = g_session.epoch;
    remote.host_id = g_session.host_id;
    memcpy(remote.session_id, g_session.session_id, 16);
}
using Datagram = std::vector<uint8_t>;
std::vector<Datagram> read_packets(Device& d) {
    std::vector<Datagram> result;
    uint8_t data[2048];
    for (;;) {
        int length = recv(d.socket, data, sizeof(data), 0);
        if (length < 0) { assert(errno == EAGAIN || errno == EWOULDBLOCK); break; }
        result.emplace_back(data, data + length);
    }
    return result;
}
void feed(const std::vector<Datagram>& packets, const Device& from) {
    for (const auto& packet : packets)
        handle_packet(packet.data(), packet.size(), from.address, monotonic_seconds());
}
std::vector<uint32_t> delivered() {
    WorldEvent events[kEventInboxSize];
    size_t n = net_copy_world_events(events, kEventInboxSize);
    std::vector<uint32_t> result;
    for (size_t i = 0; i < n; ++i) result.push_back(events[i].sequence);
    return result;
}
void post(uint8_t kind = kWorldEventBuild) {
    WorldEvent event{}; event.kind = kind;
    strcpy(event.object_id, "test-object");
    assert(queue_local_event(event, true));
}
double tick = 0;
void service() { service_world_events(monotonic_seconds() + (tick += 31)); }

int main() {
    pthread_once(&g_peers_lock_once, init_peers_lock);
    g_session.local_member = g_session.local_has_world = true;
    g_session.epoch = 1; g_session.host_id = 1; g_session.session_id[0] = 7;
    open_device(a, 1); open_device(b, 2); open_device(c, 3);
    activate(a, b); post(); post(kWorldEventRemove); service();
    auto packets = read_packets(b);
    assert(packets.size() == 3); // status, build, remove
    activate(b, a);
    // First datagram is sequence 2: it must not establish a false delivered prefix.
    feed({packets[2]}, a); assert(delivered().empty());
    assert(g_events.origins[0].next_sequence == 1);
    feed({packets[1]}, a);
    assert((delivered() == std::vector<uint32_t>{1, 2}));
    read_packets(a); // deliberately lose every ACK
    activate(a, b); service(); // 31 seconds later: old sender expired at 5 seconds
    assert(net_pending_world_event_count() == 2);
    packets = read_packets(b);
    activate(b, a); feed(packets, a); assert(delivered().empty());
    auto acknowledgements = read_packets(a);
    activate(a, b); feed(acknowledgements, b);
    assert(event_recipient(2, false)->acknowledged == 2);
    assert(net_pending_world_event_count() == 0);

    // Real discovery timeout, then actions while no eligible peer exists.
    g_peers[0].last_seen = monotonic_seconds() - kPeerTimeout - 1;
    drop_stale_peers(monotonic_seconds()); assert(!g_peers[0].active);
    for (int i = 0; i < 30; ++i) post(i % 2 ? kWorldEventRemove : kWorldEventBuild);
    service(); assert(read_packets(b).empty());
    assert(event_recipient(2, false)->acknowledged == 2);
    // Reclaim under a different slot AND endpoint; delivery identity stays intact.
    close(b.socket); open_device(b, 2);
    activate(b, a); activate(a, b, 3);
    std::vector<uint32_t> seen;
    while (event_recipient(2, false)->acknowledged < 32) {
        service(); packets = read_packets(b);
        assert(packets.size() <= kEventSendWindow + 1);
        std::reverse(packets.begin(), packets.end());
        activate(b, a); feed(packets, a);
        auto batch = delivered(); seen.insert(seen.end(), batch.begin(), batch.end());
        acknowledgements = read_packets(a);
        activate(a, b, 3); feed(acknowledgements, b);
    }
    assert(seen.size() == 30);
    for (size_t i = 0; i < seen.size(); ++i) assert(seen[i] == i + 3);
    assert(net_pending_world_event_count() == 0);

    // A genuinely new process has the downloaded snapshot. Never replay the
    // retained old commands into it; its immutable start boundary is explicit.
    activate(a, c); service(); read_packets(c);
    assert(event_recipient(3, false)->first_sequence == 33);
    post(); post(kWorldEventRemove); service(); packets = read_packets(c);
    std::reverse(packets.begin(), packets.end());
    activate(c, a); feed(packets, a);
    assert((delivered() == std::vector<uint32_t>{33, 34}));
    acknowledgements = read_packets(a);
    activate(a, c); feed(acknowledgements, c);
    auto* recipient = event_recipient(3, false);
    assert(recipient->acknowledged == 34);
    assert(event_recipient(2, false)->acknowledged == 32); // no recycled ACK bits

    // Wrong context, endpoint, stream and fabricated future ACKs cannot retire data.
    post(); service(); read_packets(c);
    WorldEventAckPacket ack{};
    ack.header.magic = kMagic; ack.header.type = kPacketWorldEventAck;
    ack.id = 3; ack.origin_id = 1; ack.sequence = 35; ack.stream = event_stamp(*recipient);
    auto receive_ack = [&](const sockaddr_in& address) {
        handle_packet(reinterpret_cast<uint8_t*>(&ack), sizeof(ack), address, monotonic_seconds());
    };
    ++ack.stream.session_id[0]; receive_ack(c.address); --ack.stream.session_id[0];
    ++ack.stream.epoch; receive_ack(c.address); --ack.stream.epoch;
    ++ack.stream.stream_id; receive_ack(c.address); --ack.stream.stream_id;
    ++ack.stream.first_sequence; receive_ack(c.address); --ack.stream.first_sequence;
    receive_ack(b.address);
    ack.sequence = 999; receive_ack(c.address);
    assert(recipient->acknowledged == 34);
    ack.sequence = 35; receive_ack(c.address); assert(recipient->acknowledged == 35);

    // A long outage exceeds bounded history: fail explicitly, never skip commands
    // or acknowledge a fictitious prefix to make the queue appear healthy.
    for (int i = 0; i < kEventRingSize + 3; ++i) post();
    assert(recipient->history_lost);
    service(); packets = read_packets(c);
    assert(packets.size() == 1 && packets[0][4] == kPacketWorldEventStatus);
    activate(c, a); feed(packets, a);
    assert(g_events.origins[0].history_lost && delivered().empty());
    assert(g_events.origins[0].next_sequence == 35);

    // A temporary world gate must not erase receive prefixes or pending history.
    g_session.local_has_world = false;
    assert(delivered().empty() && g_events.origins[0].next_sequence == 35);
    g_session.local_has_world = true;
    // Epoch change clears transport/inbox before game delivery, even if no pump
    // iteration occurred yet. Delayed packets from that epoch remain refused.
    WorldEvent stale{}; stale.sequence = 999; assert(inbox_push(stale));
    ++g_session.epoch;
    assert(delivered().empty());
    feed(packets, a);
    assert(!g_events.origins[0].used && delivered().empty());

    // Full game inbox cannot advance the ACK. Buffered contiguous data must drain
    // on an idle status after space is freed, even without any new build traffic.
    activate(c, a);
    assert(event_lock_context()); pthread_mutex_unlock(&g_event_lock);
    WorldEvent dummy{};
    for (int i = 0; i < kEventInboxSize - 2; ++i) assert(inbox_push(dummy));
    WorldEventPacket incoming{};
    incoming.header.magic = kMagic; incoming.header.type = kPacketWorldEvent;
    incoming.id = incoming.origin_id = 1; incoming.kind = kWorldEventBuild;
    memcpy(incoming.stream.session_id, g_session.session_id, 16);
    incoming.stream.epoch = g_session.epoch;
    incoming.stream.stream_id = 77; incoming.stream.first_sequence = 100;
    auto receive_event = [&] {
        handle_packet(reinterpret_cast<uint8_t*>(&incoming), sizeof(incoming), a.address,
                      monotonic_seconds());
    };
    incoming.sequence = 101; receive_event();
    incoming.sequence = 100; receive_event();
    assert(g_events.origins[0].next_sequence == 101);
    assert(origin_has_pending(&g_events.origins[0], 101));
    auto full = delivered(); assert(full.size() == kEventInboxSize - 1 && full.back() == 100);
    WorldEventStatusPacket idle{};
    idle.header.magic = kMagic; idle.header.type = kPacketWorldEventStatus;
    idle.id = 1; idle.stream = incoming.stream;
    idle.latest_sequence = 101; idle.oldest_sequence = 100;
    handle_packet(reinterpret_cast<uint8_t*>(&idle), sizeof(idle), a.address, monotonic_seconds());
    assert((delivered() == std::vector<uint32_t>{101}));
    assert(g_events.origins[0].next_sequence == 102);
    incoming.stream.stream_id = 78; incoming.stream.first_sequence = 200;
    incoming.sequence = 200; receive_event();
    assert((delivered() == std::vector<uint32_t>{200}));
    incoming.stream.stream_id = 77; incoming.stream.first_sequence = 100;
    incoming.sequence = 102; receive_event(); assert(delivered().empty());
    // The interior tail is part of the real encoder/decoder, not only a local
    // carrier. Verify every byte through UDP before continuing sequence tests.
    WorldEvent interior{};interior.kind=kWorldEventBuild;interior.tech_type=1504;
    interior.build_flags=kWorldBuildInterior|kWorldBuildComplete|kWorldBuildIsInside|kWorldBuildAttached;
    interior.constructed_amount=1;strcpy(interior.object_id,"wire-locker");
    for(int i=0;i<16;++i)interior.interior_parent_guid[i]=uint8_t(i+1);
    const float local_pose[7]={-2.25f,3.5f,7.125f,0,.70710677f,0,.70710677f};
    memcpy(interior.interior_local_pose,local_pose,sizeof(local_pose));
    ++g_session.epoch;activate(a,c);
    assert(queue_local_event(interior,true));service();
    packets=read_packets(c);activate(c,a);feed(packets,a);
    WorldEvent decoded[4]{};assert(net_copy_world_events(decoded,4)==1);
    assert(decoded[0].tech_type==1504&&decoded[0].build_flags==interior.build_flags);
    assert(!memcmp(decoded[0].interior_parent_guid,interior.interior_parent_guid,16));
    assert(!memcmp(decoded[0].interior_local_pose,local_pose,sizeof(local_pose)));
    // Locker payload is opaque: byte values may include zero, NaN bit patterns
    // and unterminated strings. Only kind 6 may use this alternative layout.
    ++g_session.epoch;activate(a,c);
    WorldEvent contents{};contents.kind=kWorldEventLockerContents;
    constexpr size_t start=offsetof(WorldEvent,guid),length=sizeof(WorldEvent)-start;
    contents.reserved16=length;
    for(size_t i=0;i<length;++i)reinterpret_cast<uint8_t*>(&contents)[start+i]=uint8_t(0xff-i);
    assert(net_try_post_world_event(contents));service();
    packets=read_packets(c);activate(c,a);feed(packets,a);
    assert(net_copy_world_events(decoded,4)==1&&decoded[0].kind==kWorldEventLockerContents);
    assert(decoded[0].reserved16==length&&!memcmp(reinterpret_cast<uint8_t*>(decoded)+start,reinterpret_cast<uint8_t*>(&contents)+start,length));
    // Single-creature channel uses real UDP and the same session/endpoint gate.
    namespace cm=creature_motion;
    g_creature_motion_tx.clear();g_creature_motion_rx.clear();
    cm::Message request;request.host=request.target=1;request.sender=3;
    request.epoch=g_session.epoch;memcpy(request.session.data(),g_session.session_id,16);
    request.nonce=0x1234567812345678ULL;request.fish[0]=4;request.prefab[0]=5;
    read_packets(a);read_packets(c);
    activate(c,a);assert(net_creature_probe_send(request));creature_motion_service();
    packets=read_packets(a);assert(packets.size()==1&&packets[0][4]==kPacketCreatureProbe);
    activate(a,c);feed(packets,c);cm::Message received;
    assert(net_creature_probe_take(&received)&&received.nonce==request.nonce&&received.fish==request.fish);
    assert(!net_creature_probe_take(&received));
    cm::Message pose=request;pose.kind=cm::Pose;pose.sender=1;pose.target=3;pose.sequence=1;pose.position={12,3,-4};
    assert(net_creature_probe_send(pose));pose.sequence=2;pose.position[0]=13;assert(net_creature_probe_send(pose));
    creature_motion_service();packets=read_packets(c);assert(packets.size()==1); // Pose mailbox coalesces.
    activate(c,a);feed(packets,a);
    assert(net_creature_probe_take(&received)&&received.sequence==2&&received.position[0]==13);
    feed(packets,b);assert(!net_creature_probe_take(&received)); // Wrong endpoint.
    g_remote[0].flags=0;feed(packets,a);assert(!net_creature_probe_take(&received));
    activate(c,a);++g_session.epoch;feed(packets,a);assert(!net_creature_probe_take(&received));
    // Old outbound session cannot be enqueued after a context transition.
    assert(!net_creature_probe_send(request));
    close(a.socket); close(b.socket); close(c.socket);
    puts("PASS: real UDP ordered first packet, lost ACK, 31s outage, offline actions, endpoint/slot reuse, new recipient baseline, forged ACK gates, bounded history gap and session reset");
}
