// Deterministic two-peer harness for the actual channel implementation; no Unity or saves.
#include "modloader/src/base_geometry_reliable.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <cassert>
#include <cstdio>
#include <vector>

constexpr uint32_t kMagic = 0x534E4D31;
constexpr size_t kMaxPeers = 2;
constexpr uint8_t kPacketGeometryFragment = 17, kPacketGeometryAck = 18;
struct PacketHeader { uint32_t magic; uint8_t type; uint8_t reserved[3]; };
struct PeerState { uint32_t id = 0; sockaddr_in raw{}; bool active = false; };
struct Session { pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; bool local_member = true;
                 bool local_has_world = true; uint32_t epoch = 1; uint8_t session_id[16]{}; };
struct Datagram { uint16_t from, to; std::vector<uint8_t> bytes; };
std::vector<Datagram> wire;
double clock_now = 0;
sockaddr_in address(uint16_t port) {
    sockaddr_in out{}; out.sin_family = AF_INET; out.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    out.sin_port = htons(port); return out;
}

// Two independent copies of the channel with mocked, lossy UDP and roster checks.
#define NODE(NAME, ID, PORT) \
namespace NAME { \
uint32_t g_local_id = ID; Session g_session; PeerState g_peers[kMaxPeers]; \
pthread_mutex_t g_peers_lock = PTHREAD_MUTEX_INITIALIZER; \
pthread_once_t g_peers_lock_once = PTHREAD_ONCE_INIT; \
void init_peers_lock() {} \
double monotonic_seconds() { return clock_now; } \
int peer_index_by_id(uint32_t id) { for (size_t i=0;i<kMaxPeers;++i) if(g_peers[i].active && g_peers[i].id==id) return static_cast<int>(i); return -1; } \
bool session_peer_eligible_locked(size_t index, bool) { return index < kMaxPeers && g_peers[index].active; } \
int send_to(const void* data, size_t size, const sockaddr_in& dest) { \
    const auto* p = static_cast<const uint8_t*>(data); \
    wire.push_back(Datagram{PORT, ntohs(dest.sin_port), std::vector<uint8_t>(p,p+size)}); return static_cast<int>(size); \
}

NODE(first, 1, 10001)
#include "modloader/src/base_geometry_channel.inc"
}
NODE(second, 2, 10002)
#include "modloader/src/base_geometry_channel.inc"
}
#undef NODE

int main() {
    first::g_session.session_id[0] = second::g_session.session_id[0] = 42;
    first::g_peers[0] = PeerState{2, address(10002), true};
    second::g_peers[0] = PeerState{1, address(10001), true};
    base_geometry::GhostEnvelope e;
    e.object_id = "corridor-123"; e.target_base_id = "base-456";
    e.ghost_class = "BaseAddCorridorGhost"; e.target_offset = {-1, 2, 3};
    e.target_base_anchor = {4, 5, 6};
    e.geometry.shape = {16, 16, 16};
    const size_t n = base_geometry::kMaxCells;
    e.geometry.faces.resize(n * 6); e.geometry.cells.resize(n); e.geometry.links.resize(n);
    e.geometry.masks.resize(n); e.geometry.glass.resize(n);
    e.geometry.cells[17] = 4; e.geometry.faces[31] = 133; e.geometry.glass[3] = 1;
    assert(first::geometry_enqueue(e));
    assert(first::geometry_enqueue(e)); // same object is not queued twice
    bool dropped_fragment = false, dropped_ack = false;
    base_geometry::GhostEnvelope received;
    uint32_t origin = 0;
    bool delivered = false;
    for (unsigned step = 0; step < 2000 && !delivered; ++step) {
        clock_now = step * 0.1;
        first::geometry_service(clock_now); second::geometry_service(clock_now);
        auto packets = std::move(wire); wire.clear();
        for (const Datagram& p : packets) {
            assert(p.bytes.size() <= 1100);
            const uint8_t type = p.bytes[4];
            if (type == kPacketGeometryFragment && !dropped_fragment) { dropped_fragment = true; continue; }
            if (type == kPacketGeometryAck && !dropped_ack) { dropped_ack = true; continue; }
            const sockaddr_in from = address(p.from);
            if (p.to == 10001) first::geometry_handle_packet(p.bytes.data(), p.bytes.size(), from, clock_now);
            if (p.to == 10002) second::geometry_handle_packet(p.bytes.data(), p.bytes.size(), from, clock_now);
        }
        delivered = second::geometry_take(&origin, &received);
    }
    assert(delivered && dropped_fragment && dropped_ack && origin == 1);
    assert(received.object_id == e.object_id && received.target_base_id == e.target_base_id &&
           received.target_offset == e.target_offset && received.geometry.cells == e.geometry.cells &&
           received.target_base_anchor == e.target_base_anchor &&
           received.geometry.faces == e.geometry.faces && received.geometry.glass == e.geometry.glass);
    assert(!second::geometry_take(&origin, &received));

    // Invalid addresses and session IDs must not allocate receive slots or generate ACKs.
    std::vector<uint8_t> encoded, frame, packet;
    assert(base_geometry::encode_envelope(e, &encoded));
    base_geometry::TransferKey key;
    key.origin = 1; key.generation = 9999; key.object = base_geometry::object_key(e.object_id);
    assert(base_geometry::make_fragment(key, encoded, 0, &frame));
    first::geometry_add_prefix(&packet, kPacketGeometryFragment, first::geometry_session());
    packet.insert(packet.end(), frame.begin(), frame.end());
    wire.clear();
    second::geometry_handle_packet(packet.data(), packet.size(), address(12345), clock_now);
    assert(wire.empty());
    auto wrong_session = packet; wrong_session[8] ^= 1;
    second::geometry_handle_packet(wrong_session.data(), wrong_session.size(), address(10001), clock_now);
    assert(wire.empty());
    auto invalid = packet; invalid[second::kGeometrySessionPrefix + 40] = 0;
    second::geometry_handle_packet(invalid.data(), invalid.size(), address(10001), clock_now);
    assert(wire.empty());
    for (const auto& rx : second::g_geometry_rx) assert(!rx.used || rx.key.generation != 9999);

    // Many completed pieces must not exhaust the receive table before its timeout.
    e.geometry.shape = {1, 1, 1}; e.geometry.faces.assign(6, 0); e.geometry.cells.assign(1, 4);
    e.geometry.links.assign(1, 0); e.geometry.masks.assign(1, 0); e.geometry.glass.assign(1, 0);
    for (int piece = 0; piece < 40; ++piece) {
        e.object_id = "piece-" + std::to_string(piece);
        assert(first::geometry_enqueue(e));
        bool got = false;
        for (int step = 0; step < 100 && !got; ++step) {
            clock_now += 0.1;
            first::geometry_service(clock_now); second::geometry_service(clock_now);
            auto packets = std::move(wire); wire.clear();
            for (const auto& p : packets) {
                const sockaddr_in from = address(p.from);
                if (p.to == 10001) first::geometry_handle_packet(p.bytes.data(), p.bytes.size(), from, clock_now);
                if (p.to == 10002) second::geometry_handle_packet(p.bytes.data(), p.bytes.size(), from, clock_now);
            }
            got = second::geometry_take(&origin, &received);
        }
        assert(got && origin == 1 && received.object_id == e.object_id);
    }
    std::puts("geometry channel: lossy two-peer test passed");
}
