// Real encoder/decoder and UDP on device loopback; no game, save or Wi-Fi changes.
#define SNMOD_BASE_GEOMETRY_CAPTURE
#define SNMOD_WORLD_TEST_ROOT "/data/local/tmp/snmod_event_recovery_tests"
#include "modloader/src/world.cpp"
#include "modloader/src/net.cpp"
#include "modloader/src/item_drop_sync.h"
#include "modloader/src/shared_progress.h"
#include <cassert>
#include <vector>
#include <algorithm>
#include <thread>

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

    // A recovery-capable drop must carry the original Id and type on the wire.
    // A pickup arriving while the prefab is loading cancels the pending drop.
    ++g_session.epoch;
    activate(a, c);
    WorldEvent drop{};
    drop.kind = kWorldEventDrop;
    drop.tech_type = 52; // Ruby, from an area where the guest may lack the original.
    drop.guid[0] = 42;
    drop.qw = 1;
    drop.x = 12;
    drop.vz = 0.5f;
    strcpy(drop.object_id, "dropped-ruby");
    net_post_world_event(drop, true);
    WorldEvent pickup{};
    pickup.kind = kWorldEventPickup;
    pickup.tech_type = drop.tech_type;
    memcpy(pickup.guid, drop.guid, sizeof(pickup.guid));
    strcpy(pickup.object_id, drop.object_id);
    net_post_world_event(pickup, true);
    WorldEvent bay = drop;
    bay.tech_type = item_drop_sync::kVehicleBay;
    bay.build_flags = item_drop_sync::kBayDeployed;
    bay.guid[0] = 43;
    strcpy(bay.object_id, "deployed-bay");
    net_post_world_event(bay, true);
    WorldEvent locker_motion = drop;
    locker_motion.kind = kWorldEventLockerMotion;
    locker_motion.tech_type = item_drop_sync::kWaterproofLocker;
    locker_motion.build_flags = item_drop_sync::kLockerMoving;
    locker_motion.guid[0] = 45;
    strcpy(locker_motion.object_id, "moving-waterproof-locker");
    assert(net_try_post_world_event(locker_motion));
    service();
    packets = read_packets(c);
    activate(c, a);
    feed(packets, a);
    assert(net_copy_world_events(decoded, 4) == 4);
    const auto& movement = decoded[3];
    assert(item_drop_sync::valid_locker_motion(movement));
    assert(movement.build_flags == item_drop_sync::kLockerMoving &&
           movement.x == locker_motion.x && movement.vz == locker_motion.vz &&
           strcmp(movement.object_id, locker_motion.object_id) == 0 &&
           !memcmp(movement.guid, locker_motion.guid, sizeof(movement.guid)));
    assert(!item_drop_sync::valid_spawn(movement));
    auto release = movement;
    release.build_flags = 0;
    assert(item_drop_sync::valid_locker_motion(release));
    release.build_flags = 2;
    assert(!item_drop_sync::valid_locker_motion(release));
    assert(item_drop_sync::valid_spawn(decoded[2]));
    assert(item_drop_sync::vehicle_bay_deployed(decoded[2]));
    decoded[2].build_flags = 0;
    assert(!item_drop_sync::vehicle_bay_deployed(decoded[2]));
    assert(item_drop_sync::valid_spawn(decoded[0]));
    assert(decoded[0].tech_type == drop.tech_type && strcmp(decoded[0].object_id, drop.object_id) == 0 &&
           decoded[0].x == drop.x && decoded[0].vz == drop.vz);
    assert(item_drop_sync::near_drop(decoded[0], 0, 0, 0));
    assert(!item_drop_sync::near_drop(decoded[0], 50, 0, 0));
    assert(decoded[1].kind == kWorldEventPickup &&
           item_drop_sync::supersedes(decoded[1], decoded[0]));
    decoded[1].guid[0]++;
    assert(!item_drop_sync::supersedes(decoded[1], decoded[0]));
    for (int tech : {3, 15, 17, 32, 35, 62, 2503, 3501, 3502, 3504,
                     item_drop_sync::kGravTrap, item_drop_sync::kWaterproofLocker}) {
        decoded[0].tech_type = tech;
        assert(item_drop_sync::valid_spawn(decoded[0]));
    }
    auto resource_pickup = decoded[1];
    assert(resource_pickup.tech_type == pickup.tech_type &&
           strcmp(resource_pickup.object_id, pickup.object_id) == 0);
    assert(item_drop_sync::has_item_identity(resource_pickup));
    assert(!item_drop_sync::valid_spawn(resource_pickup));
    resource_pickup.object_id[0] = '\0';
    assert(!item_drop_sync::has_item_identity(resource_pickup));
    decoded[0].tech_type = 16;
    memset(decoded[0].object_id, 'x', sizeof(decoded[0].object_id));
    assert(!item_drop_sync::valid_spawn(decoded[0]));

    // Nearby locker recovery uses the drop transport, with create-only semantics.
    WorldEvent locker_presence = locker_motion;
    locker_presence.kind = kWorldEventDrop;
    locker_presence.build_flags = item_drop_sync::kLockerEnsurePresent;
    WorldEvent locker_pickup = locker_presence;
    locker_pickup.kind = kWorldEventPickup;
    locker_pickup.build_flags = 0;
    assert(item_drop_sync::valid_spawn(locker_presence));
    assert(item_drop_sync::locker_presence(locker_presence));
    assert(!item_drop_sync::locker_presence(bay));
    assert(!item_drop_sync::locker_presence(locker_motion));
    assert(!item_drop_sync::supersedes(locker_presence, locker_pickup));
    assert(item_drop_sync::supersedes(locker_pickup, locker_presence));
    ++g_session.epoch;
    activate(a, c);
    net_post_world_event(locker_presence, true);
    service();
    packets = read_packets(c);
    activate(c, a);
    feed(packets, a);
    assert(net_copy_world_events(decoded, 4) == 1);
    assert(item_drop_sync::locker_presence(decoded[0]));
    assert(strcmp(decoded[0].object_id, locker_presence.object_id) == 0);

    // The next pilot receives the same native identity on the real vehicle wire path.
    activate(a, c);
    VehicleState seamoth{};
    strcpy(seamoth.classId, "SeaMoth");
    strcpy(seamoth.object_id, "native-seamoth-id");
    seamoth.guid[0] = 44;
    seamoth.x = 20;
    send_vehicle(g_peers[0], seamoth);
    packets = read_packets(c);
    auto wire = std::find_if(packets.begin(), packets.end(), [](const Datagram& datagram) {
        return datagram.size() == sizeof(VehiclePacket) && datagram[4] == kPacketVehicle;
    });
    assert(wire != packets.end());
    activate(c, a);
    feed({*wire}, a);
    VehicleState vehicles[2]{};
    assert(net_copy_vehicles(vehicles, 2) == 1);
    assert(vehicles[0].pilot == a.id && vehicles[0].guid[0] == 44 &&
           strcmp(vehicles[0].object_id, seamoth.object_id) == 0 && vehicles[0].x == 20);
    VehiclePacket bad{};
    memcpy(&bad, wire->data(), sizeof(bad));
    bad.sequence++;
    bad.x = 21;
    memset(bad.object_id, 'x', sizeof(bad.object_id));
    memcpy(wire->data(), &bad, sizeof(bad));
    feed({*wire}, a);
    assert(net_copy_vehicles(vehicles, 2) == 1 && vehicles[0].x == 20);

    ++g_session.epoch;
    activate(a, c);
    WorldEvent shared{};
    shared.kind = kWorldEventSharedProgress;
    shared.build_flags = shared_progress::PodRepair;
    shared.constructed_amount = 1;
    assert(net_try_post_world_event(shared));
    shared.build_flags = shared_progress::Door;
    shared.reserved16 = shared_progress::Open | shared_progress::Unlocked | shared_progress::Unsealed;
    strcpy(shared.object_id, "wreck-id/4/1");
    assert(net_try_post_world_event(shared));
    shared.build_flags = shared_progress::Blueprint;
    shared.tech_type = 504;
    assert(net_try_post_world_event(shared));
    WorldEvent radio{};
    radio.kind = kWorldEventSharedProgress;
    radio.build_flags = shared_progress::RadioRepair;
    radio.constructed_amount = 0.75f;
    assert(net_try_post_world_event(radio));
    service();
    packets = read_packets(c);
    activate(c, a);
    feed(packets, a);
    assert(net_copy_world_events(decoded, 4) == 4);
    for (const auto& event : decoded) assert(event.kind == shared.kind && shared_progress::valid(event));
    assert(decoded[0].constructed_amount == 1 && decoded[1].reserved16 == 7 &&
           !strcmp(decoded[1].object_id, "wreck-id/4/1") && decoded[2].tech_type == 504);
    decoded[1].reserved16 = 0x8000;
    assert(!shared_progress::valid(decoded[1]));
    assert(decoded[3].build_flags == shared_progress::RadioRepair &&
           decoded[3].constructed_amount == 0.75f);
    decoded[3].constructed_amount = NAN;
    assert(!shared_progress::valid(decoded[3]));

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
    g_creature_motion_tx={};g_creature_motion_rx={};
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
    // An idle pump or an in-flight prefix must not delay the first send of a
    // pickup command. Drop the first packet and deliver the new tail out of order.
    read_packets(a);read_packets(b);
    ++g_session.epoch;activate(a,b);
    const double now=monotonic_seconds();
    service_world_events(now);read_packets(b); // idle heartbeat
    post(kWorldEventFishPickup);service_world_events(now+.010);
    auto first=read_packets(b);assert(first.size()==1);
    post(kWorldEventFishPickup);service_world_events(now+.020);
    auto tail=read_packets(b);assert(tail.size()==1);
    activate(b,a);feed(tail,a);assert(delivered().empty());
    read_packets(a); // Keep the sender's watermark at zero.
    activate(a,b);
    for(int i=2;i<kEventSendWindow+1;++i)post(kWorldEventFishPickup);
    service_world_events(now+.030);
    assert(read_packets(b).size()==kEventSendWindow-2);
    service_world_events(now+.040);assert(read_packets(b).empty());
    service_world_events(now+.010+kEventResendInterval+.001);
    packets=read_packets(b);assert(packets.size()==kEventSendWindow);
    activate(b,a);feed(packets,a);
    assert((delivered()==std::vector<uint32_t>{1,2,3,4,5,6,7,8}));
    packets=read_packets(a);activate(a,b);feed(packets,b);
    service_world_events(now+.010+kEventResendInterval+.012);
    packets=read_packets(b);assert(packets.size()==1);
    activate(b,a);feed(packets,a);assert((delivered()==std::vector<uint32_t>{9}));
    // A loaded guest can post pickups while the pump checks for a newer host save.
    ++g_session.epoch;
    activate(b, a);
    const double refresh_time = monotonic_seconds();
    g_session.active = g_session.join_accepted = true;
    g_session.last_host_presence = refresh_time;
    g_session.host_world_gen_seen = 1;
    g_remote[0].world_gen = 2;
    alarm(10);
    std::thread pump([refresh_time] {
        for (int i = 0; i < 2000; ++i) process_session_tick(refresh_time);
    });
    for (int i = 0; i < 2000; ++i) post(kWorldEventPickup);
    pump.join();
    alarm(0);
    assert(g_session.world_refresh_pending && g_session.state == kSessionGuest);
    assert(g_events.next_sequence == 2000);
    puts("PASS: concurrent guest pickup events and host world refresh");

    strcpy(g_session.slot, "guest-world");
    const uint8_t joined_world[16]{0x35};
    memcpy(g_session.content_guid, joined_world, sizeof(joined_world));
    g_session.content_guid_valid = true;
    uint8_t checkpoint_world[16]{};
    assert(net_session_guest_world("guest-world", checkpoint_world));
    assert(memcmp(joined_world, checkpoint_world, sizeof(joined_world)) == 0);
    assert(!net_session_guest_world("another-world", checkpoint_world));
    g_session.explicit_leave = true;
    assert(!net_session_guest_world("guest-world", checkpoint_world));
    g_session.explicit_leave = false;
    g_session.local_host = true;
    assert(!net_session_guest_world("guest-world", checkpoint_world));

    close(a.socket); close(b.socket); close(c.socket);
    puts("PASS: real UDP ordered first packet, lost ACK, 31s outage, offline actions, endpoint/slot reuse, new recipient baseline, forged ACK gates, bounded history gap and session reset");
}
