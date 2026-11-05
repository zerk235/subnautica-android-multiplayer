#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "world.h"
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
#include "base_geometry_reliable.h"
#endif

// Session role changes are queued through this callback; Unity/save side effects stay on
// managed_hook.cpp's game thread.
void net_note_session_role(bool is_host, uint32_t host_id);

// Slot auto-load request: raised when a guest slot lands complete, consumed at the main
// menu. Defined after the internal namespace; set_handoff is the only producer.
void net_world_autoload_notify(const char* slot, uint32_t generation);
void net_world_autoload_cancel();
bool session_world_servable(const char* slot);

namespace {

constexpr uint32_t kMagic = 0x534E4D31;  // "SNM1"

// Fills the pump-thread call sites above LocalSession with a forward entry point; the
// definition lands next to g_session, where the session struct is complete.
void rebuild_offer_from_disk();

// Called by the guest's world-complete finalize while the receive buffers are still
// alive: adopts the received gameinfo guid as the session's world identity and arms
// the refresh detector's baseline with the generation the transfer carried. Without
// the guid adoption a later load of the refreshed slot trips kSessionConflict (the
// session would still remember the previous world). Runs on the pump thread.
void adopt_received_world_identity(uint32_t generation);

enum PacketType : uint8_t {
    kPacketHello = 1,
    kPacketState = 2,
    kPacketWorldRequest = 3,
    kPacketWorldManifest = 4,
    kPacketWorldChunk = 5,
    kPacketWorldAck = 6,
    kPacketWorldComplete = 7,
    kPacketWorldRefuse = 8,
    kPacketWorldEvent = 9,
    kPacketWorldEventAck = 10,
    kPacketTime = 11,
    kPacketVehicle = 12,
    kPacketSessionPresence = 13,
    kPacketJoinRequest = 14,
    kPacketJoinReply = 15,
    kPacketSessionLeave = 16,
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
    kPacketGeometryFragment = 17,
    kPacketGeometryAck = 18,
#endif
};

constexpr uint32_t kSessionFlagIsHost = 1u << 0;
constexpr uint32_t kSessionFlagHasWorld = 1u << 1;
constexpr uint32_t kSessionFlagMember = 1u << 2;
constexpr double kElectionConvergenceSeconds = 2.0;
constexpr double kJoinRetrySeconds = 1.0;
constexpr size_t kGameBuildCapacity = 32;
constexpr size_t kModVersionCapacity = 41;
constexpr char kGameBuild[] = "1.22.83315";
#ifndef MOD_VERSION
#define MOD_VERSION "unknown"
#endif
constexpr char kModVersion[] = MOD_VERSION;

// A chunk is sized to stay well inside one Wi-Fi datagram. The old 512-byte receive buffer
// truncated anything longer silently, and 1 kB keeps the transfer clear of IP fragmentation,
// which is what actually loses packets on a phone hotspot.
constexpr double kWorldChunkTimeout = 0.35;    // repeat the window after this
constexpr double kWorldRequestInterval = 2.0;  // ask again until a host answers
constexpr double kWorldRequestRetryInterval = 10.0;  // after a failed transfer, back off
constexpr double kGuestIdleSeconds = 30.0;     // retire transfer slots nobody advances
constexpr int kWorldMaxGuests = 2;
constexpr size_t kWorldOfferLimit = kWorldMaxTransferBytes;

constexpr double kTimePacketInterval = 1.0;  // the host publishes its clock at 1 Hz

constexpr double kVehicleInterval = 0.1;  // piloted vehicles ship at 10 Hz
constexpr size_t kMaxVehicles = 8;        // one slot per driving peer, room to spare
constexpr double kVehicleTimeout = 5.0;   // drop poses nobody refreshes (mirrors peers)

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;
    uint8_t type;
    uint8_t reserved[3];
};

// Set by the phone that is hosting this session. Sent in hello so a peer can tell the two roles
// apart without having to guess from the save: the host is whoever opened the session, not
// whoever happens to have a world loaded. Both used to load a world - a guest that received the
// host's save has one too - and "has a world" therefore said nothing about the role.
constexpr uint32_t kHelloIsHost = 1u << 0;

struct HelloPacket {
    PacketHeader header;
    uint32_t id;
    uint32_t flags;
};

struct StatePacket {
    PacketHeader header;
    uint32_t id;
    uint32_t sequence;
    float x;
    float y;
    float z;
    float yaw;
    float pitch;
    float view_yaw;
    uint32_t flags;
    float anim_floats[kNetAnimFloats];
    uint8_t anim_underwater;
    uint8_t anim_cinematics;
    uint8_t anim_float_count;
    // TechType of the held item as an integer, 0 for bare hands. The receiver instantiates a
    // visual copy of that item onto the peer's avatar, so both players see the same tool.
    uint32_t held_tech;
    // Worn equipment: suit male_geo bitmask plus Foots/Head slot TechTypes. The receiver
    // replicates the mask on the avatar, so the peer is shown in what it really wears -
    // including suits no TechType table could name.
    uint32_t suit_tech;
    uint32_t fins_tech;
    uint32_t head_tech;
    uint8_t environment_id[16];
    HeldRotation held_rotation;
    uint8_t held_rotation_valid;
    HeldOffset held_offset;
};

// Piloted-vehicle pose. Packed like every other packet; 72 bytes on the wire.
struct VehiclePacket {
    PacketHeader header;
    uint32_t id;
    uint32_t sequence;
    uint8_t guid[16];
    char classId[16];  // vehicle prefab class id; the receiver clones from it when the guid
                       // has no local match (separate saves mean the pilot's sub does not
                       // exist on the peer until it is spawned here)
    float x, y, z;
    float yaw, pitch, roll;
};

// The guest says "I have no world, send me yours". Sent on a timer rather than once so a
// guest that boots after the host still gets served.
struct WorldRequestPacket {
    PacketHeader header;
    uint32_t id;
};

// Slot name, then one entry per file: name, exact size, FNV-1a of the contents. The guest
// allocates against these sizes and refuses the transfer if a checksum does not match, so the
// manifest is what makes a partial write impossible to mistake for a world.
struct WorldManifestPacket {
    PacketHeader header;
    uint32_t id;
    char slot[24];
    uint8_t file_count;
    uint8_t reserved[3];
    struct {
        char name[32];
        uint32_t size;
        uint32_t checksum;
    } files[kWorldFileCount];
    // v4: the offering WorldOffer's generation, so the guest can adopt it as its
    // refresh-detect baseline (host_world_gen_seen) once the transfer lands.
    uint32_t generation;
};

struct WorldChunkPacket {
    PacketHeader header;
    uint32_t id;
    uint8_t file_index;
    uint8_t reserved8;
    uint16_t reserved16;
    uint32_t offset;
    uint16_t payload_size;
    uint16_t reserved16b;
    uint8_t payload[kWorldChunkPayload];
};

// Stop-and-wait: the host may be up to kWorldChunkWindow chunks ahead of the guest's high
// water mark, and resends from there. Being a little ahead hides the round trip without
// needing per-chunk state that a reset could corrupt.
constexpr uint32_t kWorldChunkWindow = 4;

// Offer generation the last accepted manifest carried (set in the manifest handler,
// consumed by adopt_received_world_identity in the finalize). Lives next to the
// receive state because it describes exactly that transfer.
uint32_t g_receive_generation = 0;

struct WorldAckPacket {
    PacketHeader header;
    uint32_t id;
    uint8_t file_index;
    uint8_t reserved8;
    uint16_t reserved16;
    uint32_t received;
};

struct WorldCompletePacket {
    PacketHeader header;
    uint32_t id;
    uint8_t ok;
    uint8_t reserved[3];
};

// An event plus the sequence that makes it replayable. origin_id survives a relay: when the host
// forwards a guest's pickup it keeps the guest's id and sequence, so the other guests recognise
// the copy as the same event instead of treating it as new. One fixed size serves every kind:
// pickup/drop leave the build tail zeroed, build events leave the throw velocity zeroed.
struct WorldEventPacket {
    PacketHeader header;
    uint32_t id;
    uint32_t origin_id;
    uint32_t sequence;
    uint8_t kind;
    uint8_t build_flags;
    uint16_t reserved16;
    uint8_t guid[16];
    uint8_t class_id[16];
    int32_t tech_type;
    float constructed_amount;
    float x;
    float y;
    float z;
    float qx;
    float qy;
    float qz;
    float qw;
    float vx;
    float vy;
    float vz;
    char object_id[kWorldEventObjectIdSize];
    int32_t proto_version;
    int32_t face_linked_module_type;
    float face_linked_position[3];
    int32_t face_cell[3];
    uint32_t face_direction;
    uint8_t face_has_value;
    uint8_t reserved8[3];
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
    uint32_t geometry_hash_low;
    uint32_t geometry_hash_high;
#endif
};

// Cumulative rather than per-event: a guest that acks sequence 40 has implicitly received
// everything below it, so one packet covers a burst and a lost ack costs a repeat.
struct WorldEventAckPacket {
    PacketHeader header;
    uint32_t id;
    uint32_t origin_id;
    uint32_t sequence;
};

// The host's DayNightCycle clock (timePassedAsDouble) plus the rate it is actually advancing
// at, measured on the host itself. One sample per second is enough: the guest interpolates
// between samples with the shipped rate instead of assuming 72 game-seconds per real second -
// the assumption was wrong and made the guest's clock race forward and snap back constantly,
// which is visible as the sun sprinting across the sky and jumping home.
struct TimePacket {
    PacketHeader header;
    double time_passed;
    double rate;
};

// Session-layer packets (protocol v4, types 13-16). Same rules as the transport packets:
// 8-byte PacketHeader, fixed fields, #pragma pack(1), validated before use (exact size,
// zeroed reserved/padding, NUL-terminated zero-padded strings). Field order below is the
// wire order; static_asserts underneath pin every size.
struct SessionPresencePacket {
    PacketHeader header;
    uint16_t protocol_version;  // kSessionProtocolVersion (4); anything else is hidden
    uint16_t packet_size;       // must equal sizeof(*this); rejects truncated reads
    uint32_t sender_id;
    uint32_t sequence;      // per-sender counter; stale repeats are dropped
    uint32_t session_epoch;  // ++ on every host migration; higher wins
    uint32_t host_id;        // who the sender believes the host is (== sender_id on host)
    uint8_t flags;           // kSessionFlagIsHost/HasWorld/Member
    uint8_t player_count;    // host + world-loaded members (host counts, others ignore)
    uint8_t max_players;
    uint8_t reserved;
    uint8_t session_id[16];    // random per host launch, preserved across migration;
                               // zeroed = not in any session
    uint8_t content_guid[16];  // gameinfo.json world GUID (content identity, not session)
    char slot[24];             // host slot name, NUL-terminated
    char game_build[kGameBuildCapacity];  // compared exactly ("1.22.83315")
    char mod_version[kModVersionCapacity];  // 40-char git hash + NUL, compared exactly
    // v4: the sender's current offer generation (host) or the offer generation it has
    // adopted (guest that already received a world); 0 while worldless.
    uint32_t world_gen;
};

struct JoinRequestPacket {
    PacketHeader header;
    uint16_t protocol_version;
    uint16_t packet_size;
    uint32_t sender_id;
    uint32_t target_host_id;  // unicast: exactly one host is asked, never broadcast
    uint32_t session_epoch;   // epoch the guest saw; stale asks are refused
    uint8_t session_id[16];   // session being joined
    uint64_t request_nonce;   // random per attempt; repeats get the cached reply
};

struct JoinReplyPacket {
    PacketHeader header;
    uint16_t protocol_version;
    uint16_t packet_size;
    uint32_t sender_id;      // the host answering
    uint32_t recipient_id;   // the guest that asked
    uint32_t session_epoch;
    uint8_t session_id[16];
    uint64_t request_nonce;  // echoed so the guest can match the answer
    uint8_t status;          // SessionJoinStatus: ACCEPTED or machine-readable reason
    uint8_t reason;          // == status (kept as a pair for future detail codes)
    uint8_t reserved[2];
};

struct SessionLeavePacket {
    PacketHeader header;
    uint16_t protocol_version;
    uint16_t packet_size;
    uint32_t sender_id;
    uint32_t host_id;        // session being left (lets the host drop the right roster entry)
    uint32_t session_epoch;
    uint8_t session_id[16];
};
#pragma pack(pop)

// Wire contract: both phones and the PC peers must agree on these sizes.
// If any assert fires, update the Python peers and AGENT_HANDOFF §4 together.
static_assert(sizeof(PacketHeader) == 8, "header");
static_assert(sizeof(HelloPacket) == 16, "hello");
static_assert(sizeof(StatePacket) == 172, "state with interior identity and held tool world pose");
static_assert(sizeof(VehiclePacket) == 72, "vehicle");
static_assert(sizeof(WorldRequestPacket) == 12, "world request");
static_assert(sizeof(WorldManifestPacket) == 244, "manifest with saved entity cells");
static_assert(sizeof(WorldChunkPacket) == 1048, "chunk");
static_assert(sizeof(WorldAckPacket) == 20, "world ack");
static_assert(sizeof(WorldCompletePacket) == 16, "complete");
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
static_assert(sizeof(WorldEventPacket) == 280, "event with geometry fingerprint");
static_assert(sizeof(WorldEvent) == 268, "event game-side carrier with geometry fingerprint");
static_assert(offsetof(WorldEventPacket, geometry_hash_low) == 272 &&
              offsetof(WorldEvent, geometry_hash_low) == 260, "geometry fingerprint wire offset");
#else
static_assert(sizeof(WorldEventPacket) == 272, "event");
// header (8) + id (4) on top of the game-side carrier
static_assert(sizeof(WorldEvent) == 260, "event game-side carrier");
#endif
static_assert(sizeof(WorldEventPacket) == sizeof(WorldEvent) + 12, "event wire mirror");
static_assert(sizeof(WorldEventAckPacket) == 20, "event ack");
static_assert(sizeof(TimePacket) == 24, "time");
static_assert(sizeof(SessionPresencePacket) == 165, "session presence");
static_assert(sizeof(JoinRequestPacket) == 48, "join request");
static_assert(sizeof(JoinReplyPacket) == 52, "join reply");
static_assert(sizeof(SessionLeavePacket) == 40, "session leave");

int g_socket = -1;
uint32_t g_local_id = 0;
PeerState g_peers[kMaxPeers];
// Received vehicle poses, same lock as the peer table (handle_packet holds it).
VehicleState g_vehicles[kMaxVehicles];

static unsigned long long g_rx_hello = 0;
static unsigned long long g_rx_state = 0;
static unsigned long long g_rx_other = 0;
static unsigned long long g_rx_bytes = 0;

void net_rx_stats(unsigned long long* hello, unsigned long long* state,
                  unsigned long long* other, unsigned long long* bytes) {
    *hello = g_rx_hello;
    *state = g_rx_state;
    *other = g_rx_other;
    *bytes = g_rx_bytes;
}

unsigned long long net_rx_hello() { return g_rx_hello; }
unsigned long long net_rx_state() { return g_rx_state; }
unsigned long long net_rx_other() { return g_rx_other; }
unsigned long long net_rx_bytes() { return g_rx_bytes; }
double g_next_hello = 0.0;
uint32_t g_next_sequence = 0;

// Day/night clock state. g_local_* is written by the game thread every frame (an anchor the
// pump ships while this phone is the host); g_host_* is written by the pump when a TimePacket
// arrives and read by the game thread, which does the actual correction.
double g_next_time = 0.0;
pthread_mutex_t g_time_lock = PTHREAD_MUTEX_INITIALIZER;
double g_local_time = 0.0;
double g_local_rate = 0.0;
bool g_local_time_in_game = false;
double g_host_time = 0.0;
double g_host_time_at = 0.0;
double g_host_rate = 0.0;
bool g_host_time_valid = false;

pthread_t g_pump;
bool g_pump_running = false;

// ---------------------------------------------------------------------------
// Watchdog
//
// If the pump thread stops beating for kWatchdogStallMs the process is wedged
// on a lock. The watchdog never takes any of the mod's locks, so it keeps
// running while everyone else is stuck; abort() makes crash_dump write a
// tombstone with every thread's backtrace.
// ---------------------------------------------------------------------------
constexpr uint64_t kWatchdogStallMs = 60000;
volatile uint64_t g_pump_beat_ms = 0;
pthread_t g_watchdog;
bool g_watchdog_running = false;

static uint64_t MonotonicMs() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void* WatchdogMain(void*) {
    pthread_setname_np(pthread_self(), "rlds.watchdog");
    while (g_watchdog_running) {
        usleep(5 * 1000 * 1000);
        uint64_t now_ms = MonotonicMs();
        uint64_t beat = g_pump_beat_ms;
        if (beat == 0) continue;
        uint64_t stall = now_ms - beat;
        if (stall <= kWatchdogStallMs) continue;
        LOG_ERROR("watchdog: pump heartbeat stale for %llu ms (thread wedged on a lock?) - aborting for a tombstone",
                  (unsigned long long)stall);
        usleep(500 * 1000);
        abort();
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// World handoff state
//
// Everything below the offer buffer is pump-thread only. The game thread reaches it through
// net_offer_world() on the way in and the net_world_handoff_* accessors on the way out.
// ---------------------------------------------------------------------------

int send_to(const void* data, size_t size, const struct sockaddr_in& addr,
            int* error_out = nullptr);

struct OfferFile {
    char name[32];
    uint32_t size;
    uint32_t checksum;
    uint8_t* data;
};

struct WorldOffer {
    bool valid;
    char slot[24];
    int file_count;
    OfferFile files[kWorldFileCount];
    size_t total_bytes;
    uint64_t generation;
    // Reference count. 1 is the publication itself (the g_offer_snapshot pointer); each
    // guest transfer holding the snapshot adds one. Freed on the drop to zero, so a
    // re-capture mid-transfer can neither free data under an active sender nor mix two
    // worlds into one transfer.
    int refs;
};

struct WorldGuest {
    bool active;
    uint32_t id;
    struct sockaddr_in raw;
    char slot[24];
    int file_count;
    struct {
        uint32_t size;
        uint32_t checksum;
    } files[kWorldFileCount];
    uint32_t total_chunks;
    // The guest's contiguous progress, from its acks. Everything is resent from here, so a
    // lost packet costs one window rather than the whole transfer, and a guest that restarted
    // with empty buffers simply pulls the window again.
    uint32_t acked;
    // How far the wire has been written. Compared against acked so the sender does not
    // outrun the guest, and against the timeout so it repeats the window when nothing comes
    // back.
    uint32_t sent_upto;
    double last_send;
    uint32_t resends;
    bool finished;
    bool request_seen;
    // Set once the offer's slot name has been copied into this guest, so the "already has it"
    // check does not depend on pump_guest having run yet.
    bool slot_known;
    // The immutable snapshot this transfer started from. Held by reference, so a later
    // capture cannot free or replace the data this transfer is sending. Released when
    // the guest slot is retired.
    struct WorldOffer* snapshot;
    // Last pump-side activity (request, ack, send pass). Idle guests release their slot,
    // which also fixes reconnect churn exhausting the two transfer slots.
    double last_activity;
};

// Written by the game thread through net_offer_world; the pump reads it only through
// borrowed references, never the shared pointer directly.
pthread_mutex_t g_offer_lock = PTHREAD_MUTEX_INITIALIZER;
WorldOffer* g_offer_snapshot = nullptr;
uint64_t g_offer_generation = 0;
WorldGuest g_guests[kWorldMaxGuests];

// Callers hold g_offer_lock. Frees the snapshot when the last reference is gone.
void offer_release_locked(WorldOffer* snap) {
    if (!snap) return;
    if (--snap->refs > 0) return;
    for (int i = 0; i < kWorldFileCount; i++) free(snap->files[i].data);
    free(snap);
}

// Callers hold g_offer_lock. Adds a reference for a guest about to borrow the snapshot.
void offer_acquire_locked(WorldOffer* snap) {
    if (snap) snap->refs++;
}

// Lock-taking wrapper for callers outside the claim path.
void offer_release(WorldOffer* snap) {
    if (!snap) return;
    pthread_mutex_lock(&g_offer_lock);
    offer_release_locked(snap);
    pthread_mutex_unlock(&g_offer_lock);
}

pthread_mutex_t g_handoff_lock = PTHREAD_MUTEX_INITIALIZER;
WorldHandoffState g_handoff_state = kWorldHandoffIdle;
char g_handoff_slot[24] = {0};
char g_handoff_detail[128] = {0};

// ---------------------------------------------------------------------------
// Reliable world events
//
// The ring below is the retry queue. Every slot is one event that has been sent but not yet
// acknowledged, so the pump can walk it and repeat whatever is due. A ring rather than a list
// because the alternative - a heap with per-peer ownership - buys nothing here: an event only
// leaves the queue once every peer has it, and that is a single condition on the whole slot.
// ---------------------------------------------------------------------------

constexpr int kEventRingSize = 128;     // power of two, so the mask does the modulo
constexpr int kEventRingMask = kEventRingSize - 1;
constexpr double kEventResendInterval = 0.25;
constexpr double kEventExpiry = 5.0;

// Log names for the event kinds; anything unknown stays honest in the log instead of
// masquerading as a pickup.
const char* event_kind_name(uint8_t kind) {
    switch (kind) {
        case kWorldEventPickup: return "pickup";
        case kWorldEventDrop: return "drop";
        case kWorldEventBuild: return "build";
        case kWorldEventProgress: return "progress";
        case kWorldEventRemove: return "remove";
        default: return "event";
    }
}

// The receive path drops kinds it does not know: a peer running a newer protocol must not
// have its events misread as pickups by an older build.
bool event_kind_known(uint8_t kind) {
    return kind == kWorldEventPickup || kind == kWorldEventDrop ||
           kind == kWorldEventBuild || kind == kWorldEventProgress ||
           kind == kWorldEventRemove;
}

struct EventSlot {
    bool used;
    bool local_only;   // produced here and already applied; never needs to be echoed back to us
    uint32_t origin_id;
    uint32_t sequence;
    uint8_t kind;
    uint8_t build_flags;
    uint8_t guid[16];
    uint8_t class_id[16];
    int32_t tech_type;
    float constructed_amount;
    float x, y, z;
    float qx, qy, qz, qw;
    float vx, vy, vz;
    char object_id[kWorldEventObjectIdSize];
    int32_t proto_version;
    int32_t face_linked_module_type;
    float face_linked_position[3];
    int32_t face_cell[3];
    uint32_t face_direction;
    uint8_t face_has_value;
    uint8_t reserved8[3];
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
    uint32_t geometry_hash_low;
    uint32_t geometry_hash_high;
#endif
    // One bit per peer slot. Cleared when the peer times out, because a peer that left will
    // never acknowledge and would otherwise pin the event in the queue forever.
    uint32_t acked;
    double last_send;
    double created;
};

// One ring for the local origin, plus one delivery window per remote origin.

// One ring for the local origin, plus one delivery window per remote origin.
// The window has to be per origin rather than a single counter: the host numbers its own
// events from its own counter while forwarding guests' events with their numbers intact, so a
// shared counter would let a high host sequence hide a lower guest sequence still in flight and
// silently drop it as a duplicate.
constexpr int kEventOriginSlots = kMaxPeers + 1;
// Out-of-order hold: pickup/drop are commands, not pose, so "2 before 1" must NOT advance
// past 1 - the old highest-seen watermark acked events it had only heard of, and an event
// overtaken by a later one was lost forever once its retransmits were answered as duplicates.
constexpr int kEventPendingSize = 16;

struct EventOrigin {
    bool used;
    uint32_t id;
    // Contiguous-delivery window: next_sequence is the lowest sequence not yet handed to the
    // game inbox. The ack only ever covers what actually reached the inbox, so a gap stalls
    // the watermark and the sender retransmits into it - which is what a cumulative ack
    // must do over UDP. seq_valid arms the window on the first event from this origin.
    bool seq_valid;
    uint32_t next_sequence;
    bool pending_used[kEventPendingSize];
    WorldEvent pending[kEventPendingSize];
};

struct EventRing {
    uint32_t next_sequence;
    EventSlot slots[kEventRingSize];
    EventOrigin origins[kEventOriginSlots];
    uint32_t peer_bits;
};

pthread_mutex_t g_event_lock = PTHREAD_MUTEX_INITIALIZER;
EventRing g_events;
uint32_t g_event_peer_count = 0;
unsigned long long g_event_rx = 0;
unsigned long long g_event_tx = 0;
unsigned long long g_event_dup = 0;

// Inbound queue. The pump may not touch Pickupable, so received events wait here for the game
// thread to drain them from Player.Update.
constexpr int kEventInboxSize = 64;
WorldEvent g_event_inbox[kEventInboxSize];
int g_event_inbox_head = 0;
int g_event_inbox_tail = 0;

// Queue for the game thread. Returns false when full: the caller must then keep the
// event (or drop it without acking), never acknowledge a byte the game has not received.
bool inbox_push(const WorldEvent& event) {
    int next = (g_event_inbox_head + 1) % kEventInboxSize;
    if (next == g_event_inbox_tail) return false;
    g_event_inbox[g_event_inbox_head] = event;
    g_event_inbox_head = next;
    return true;
}

bool origin_has_pending(const EventOrigin* origin, uint32_t sequence) {
    for (int i = 0; i < kEventPendingSize; i++) {
        if (origin->pending_used[i] && origin->pending[i].sequence == sequence) {
            return true;
        }
    }
    return false;
}

bool origin_store_pending(EventOrigin* origin, const WorldEvent& event) {
    for (int i = 0; i < kEventPendingSize; i++) {
        if (origin->pending_used[i]) continue;
        origin->pending_used[i] = true;
        origin->pending[i] = event;
        return true;
    }
    return false;
}

// Delivers buffered events that have become contiguous. A full inbox stops the drain
// mid-window: the buffered event stays for the next receive, and the ack watermark
// stays below it.
void drain_pending(EventOrigin* origin) {
    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (int i = 0; i < kEventPendingSize; i++) {
            if (!origin->pending_used[i]) continue;
            if (origin->pending[i].sequence != origin->next_sequence) continue;
            if (!inbox_push(origin->pending[i])) return;
            origin->pending_used[i] = false;
            origin->next_sequence++;
            progressed = true;
        }
    }
}

void set_handoff(WorldHandoffState state, const char* slot, const char* detail);

uint32_t guest_total_chunks(const WorldGuest& guest) {
    uint32_t total = 0;
    for (int i = 0; i < kWorldFileCount; i++) total += chunks_for(guest.files[i].size);
    return total;
}

// Locates the flat chunk ordinal within the manifest. Returns false when the ordinal is past
// the end, which is how a guest that already has everything is detected.
bool locate_chunk(const WorldGuest& guest, uint32_t ordinal, int* out_file, uint32_t* out_offset) {
    uint32_t remaining = ordinal;
    for (int i = 0; i < kWorldFileCount; i++) {
        uint32_t count = chunks_for(guest.files[i].size);
        if (remaining < count) {
            *out_file = i;
            *out_offset = remaining * kWorldChunkPayload;
            return true;
        }
        remaining -= count;
    }
    return false;
}

// The slot auto-load request lives at file scope (net_world_autoload_notify below); this
// is the only producer: a complete guest slot hands itself to the game thread here.
void set_handoff(WorldHandoffState state, const char* slot, const char* detail) {
    pthread_mutex_lock(&g_handoff_lock);
    g_handoff_state = state;
    snprintf(g_handoff_slot, sizeof(g_handoff_slot), "%s", slot ? slot : "");
    snprintf(g_handoff_detail, sizeof(g_handoff_detail), "%s", detail ? detail : "");
    pthread_mutex_unlock(&g_handoff_lock);
    // Every Written comes out of the guest receive handler, which set g_receive_generation
    // before completing, so it is the generation this slot was transferred for. The game
    // thread decides what to do with the request; the pump only records that a complete
    // slot exists.
    if (state == kWorldHandoffWritten && slot && *slot) {
        net_world_autoload_notify(slot, g_receive_generation);
    }
}

double monotonic_seconds();

WorldGuest* find_guest(uint32_t id) {    for (size_t i = 0; i < kWorldMaxGuests; i++) {
        if (g_guests[i].active && g_guests[i].id == id) return &g_guests[i];
    }
    return nullptr;
}

// A peer that asks for a world gets a transfer slot, whether or not one already existed: the
// address is refreshed because the guest may have restarted with a new source port.
WorldGuest* claim_guest(uint32_t id, const struct sockaddr_in& addr) {
    if (id == g_local_id) return nullptr;

    WorldGuest* guest = find_guest(id);
    if (guest) {
        guest->raw = addr;
        guest->last_activity = monotonic_seconds();
        return guest;
    }

    for (size_t i = 0; i < kWorldMaxGuests; i++) {
        if (g_guests[i].active) continue;
        memset(&g_guests[i], 0, sizeof(g_guests[i]));
        g_guests[i].active = true;
        g_guests[i].id = id;
        g_guests[i].raw = addr;
        g_guests[i].last_activity = monotonic_seconds();
        LOG_INFO("world: guest %08x asked for the world", id);
        return &g_guests[i];
    }

    LOG_WARN("world: guest %08x refused, all %d transfer slots are busy", id, kWorldMaxGuests);
    return nullptr;
}

// Retires a guest slot: drops its snapshot reference and frees the slot for the next
// guest. Guests used to persist forever, so reconnect churn (a new id per app restart)
// could exhaust the two transfer slots after a few reconnections.
void release_guest(WorldGuest* guest) {
    if (!guest || !guest->active) return;
    if (guest->snapshot) offer_release(guest->snapshot);
    memset(guest, 0, sizeof(*guest));
}

// Every packet carries the sender's id, so a receiver can drop its own loopback with one
// comparison. Host-to-guest packets are addressed to a single guest through its sockaddr, so
// they carry the host's id rather than the guest's.
void send_manifest(const WorldGuest& guest) {
    // Reads the guest's own snapshot, not the shared g_offer_snapshot: the offer may have
    // been replaced by a newer capture while this transfer is in flight, and a transfer
    // must stay on the snapshot it was claimed from.
    const WorldOffer* offer = guest.snapshot;
    if (!offer || !offer->valid) return;
    WorldManifestPacket packet{};
    packet.header.magic = kMagic;
    packet.header.type = kPacketWorldManifest;
    packet.id = g_local_id;
    packet.file_count = static_cast<uint8_t>(kWorldFileCount);
    snprintf(packet.slot, sizeof(packet.slot), "%s", offer->slot);
    for (int i = 0; i < kWorldFileCount; i++) {
        snprintf(packet.files[i].name, sizeof(packet.files[i].name), "%s",
                 offer->files[i].name);
        packet.files[i].size = offer->files[i].size;
        packet.files[i].checksum = offer->files[i].checksum;
    }
    packet.generation = static_cast<uint32_t>(offer->generation);
    send_to(&packet, sizeof(packet), guest.raw);
}

void send_refuse(const WorldGuest& guest, const char* why) {
    WorldCompletePacket packet{};
    packet.header.magic = kMagic;
    packet.header.type = kPacketWorldRefuse;
    packet.id = g_local_id;
    packet.ok = 0;
    send_to(&packet, sizeof(packet), guest.raw);
    LOG_WARN("world: refused guest %08x: %s", guest.id, why);
}

// How far the guest has contiguously, and whether it has everything. The contiguous prefix
// matters: a chunk that arrived early must not be counted, or the host would believe a gap is
// filled and skip it.
void guest_progress(const WorldManifest& manifest, uint32_t* out_chunks, bool* out_complete) {
    uint32_t chunks = 0;
    for (int i = 0; i < kWorldFileCount; i++) {
        uint32_t have = world_receive_contiguous(i);
        if (have >= chunks_for(manifest.files[i].size)) {
            chunks += chunks_for(manifest.files[i].size);
            continue;
        }
        *out_chunks = chunks + have;
        *out_complete = false;
        return;
    }
    *out_chunks = chunks;
    *out_complete = true;
}

// Sends the window starting at the guest's confirmed position. Called on a timer, so the same
// path serves both "the guest is making progress, extend the window" and "nothing came back,
// repeat it".
void pump_guest(WorldGuest& guest, double now) {
    if(!session_world_servable(guest.slot_known ? guest.slot : nullptr)) {
        release_guest(&guest);return; // pump owns guest snapshots; retire old-world sends here
    }
    if (!guest.request_seen) return;
    guest.last_activity = now;

    if (guest.total_chunks == 0) {
        // Claim the current offer under the lock. From here the guest reads only its own
        // reference, so a later net_offer_world() swap can never touch this transfer. A
        // guest rewound after a host restart re-claims here, picking up a newer snapshot
        // if the world was re-captured in between.
        pthread_mutex_lock(&g_offer_lock);
        WorldOffer* snap = g_offer_snapshot;
        bool have_snapshot = snap && snap->valid;
        pthread_mutex_unlock(&g_offer_lock);

        if (!have_snapshot) {
            // Defined next to g_session below: the pump must not touch session state
            // before that struct is in scope.
            rebuild_offer_from_disk();
        }

        pthread_mutex_lock(&g_offer_lock);
        snap = g_offer_snapshot;
        if (!snap || !snap->valid) {
            pthread_mutex_unlock(&g_offer_lock);
            return;
        }
        offer_acquire_locked(snap);
        WorldOffer* previous = guest.snapshot;
        guest.snapshot = snap;
        snprintf(guest.slot, sizeof(guest.slot), "%s", snap->slot);
        guest.slot_known = true;
        for (int i = 0; i < kWorldFileCount; i++) {
            guest.files[i].size = snap->files[i].size;
            guest.files[i].checksum = snap->files[i].checksum;
        }
        uint64_t generation = snap->generation;
        size_t total_bytes = snap->total_bytes;
        pthread_mutex_unlock(&g_offer_lock);
        // Drop the previous claim outside the lock.
        if (previous && previous != snap) offer_release(previous);

        guest.total_chunks = guest_total_chunks(guest);
        guest.acked = 0;
        guest.sent_upto = 0;
        send_manifest(guest);
        LOG_INFO("world: offering slot %s to %08x (gen %llu), %zu bytes in %u chunks",
                 guest.slot, guest.id, (unsigned long long)generation, total_bytes,
                 guest.total_chunks);
        return;
    }

    if (guest.acked >= guest.total_chunks) {
        // The guest has every byte; it now verifies and writes the slot itself and says so.
        // Reported once, because the sender is deliberately mute from here on.
        if (!guest.finished) {
            guest.finished = true;
            LOG_INFO("world: guest %08x received all %u chunks of slot %s, %u window repeats",
                     guest.id, guest.total_chunks, guest.slot, guest.resends);
        }
        return;
    }

    uint32_t target = guest.acked + kWorldChunkWindow;
    if (target > guest.total_chunks) target = guest.total_chunks;

    bool behind = guest.sent_upto < target;
    bool timed_out = (now - guest.last_send) >= kWorldChunkTimeout;
    if (!behind && !timed_out) return;

    for (uint32_t ordinal = guest.acked; ordinal < target; ordinal++) {
        int file_index = 0;
        uint32_t offset = 0;
        if (!locate_chunk(guest, ordinal, &file_index, &offset)) return;

        uint32_t remaining = guest.files[file_index].size - offset;
        uint32_t take = remaining < kWorldChunkPayload ? remaining : kWorldChunkPayload;

        WorldChunkPacket packet{};
        packet.header.magic = kMagic;
        packet.header.type = kPacketWorldChunk;
        packet.id = g_local_id;
        packet.file_index = static_cast<uint8_t>(file_index);
        packet.offset = offset;
        packet.payload_size = static_cast<uint16_t>(take);
        // Data comes from this transfer's own snapshot, never from the shared pointer:
        // the offer may have been re-captured since the claim.
        memcpy(packet.payload, guest.snapshot->files[file_index].data + offset, take);
        send_to(&packet, sizeof(packet), guest.raw);
    }

    if (guest.sent_upto > 0 && guest.sent_upto == target && !behind) guest.resends++;
    guest.sent_upto = target;
    guest.last_send = now;
}

// The pump thread owns the peer table; the game thread reads copies of it. Recursive so the
// pump can call net_peer_count() while already holding the lock.
pthread_mutex_t g_peers_lock;
pthread_once_t g_peers_lock_once = PTHREAD_ONCE_INIT;
struct RemotePresence {
    // Last v3 presence heard from one peer-table slot, indexed in parallel with g_peers.
    // Written by the pump on packet receipt; read under g_peers_lock. A zeroed entry means
    // "never heard a valid presence" (or a foreign-protocol peer, which is tracked but
    // hidden from the session list).
    bool valid;
    bool protocol_v3;  // false = speaks another protocol: kept for logs, never synced with
    uint32_t sequence;  // their presence counter; older repeats are dropped (reorder guard)
    uint32_t epoch;     // their view of the session epoch; higher wins on conflict
    uint32_t host_id;   // who they believe the host is
    uint8_t flags;      // kSessionFlagIsHost/HasWorld/Member
    uint8_t player_count;  // host's count (host + world-loaded members); others ignore it
    uint8_t max_players;
    uint8_t session_id[16];    // zeroed = not in any session
    uint8_t content_guid[16];  // world identity they advertise (zeroed when worldless)
    uint32_t world_gen;        // v4: offer generation they advertise (0 when worldless)
    char slot[24];             // their slot name (meaningful with HAS_WORLD)
    char game_build[kGameBuildCapacity];  // e.g. "1.22.83315"; compared exactly
    char mod_version[kModVersionCapacity];  // 40-char git hash; compared exactly
    double last_presence;                   // pump timestamp of the last valid packet
    bool join_accepted;  // WE accepted this peer's join (host side): its world requests
                         // are served and its events/state are synced
    // Host-issued admission survives reordered pre-JOIN presence packets. Bind it to
    // the exact session/epoch, independently of what the guest currently advertises.
    uint8_t admitted_session[16];
    uint32_t admitted_epoch;
    uint64_t last_join_nonce;  // nonce of the last answered JOIN (idempotent repeats)
    bool cached_reply_valid;
    JoinReplyPacket cached_reply;  // exact bytes to resend on a repeated nonce
};
RemotePresence g_remote[kMaxPeers];

bool join_grant_matches(const RemotePresence& remote, const uint8_t session[16], uint32_t epoch) {
    return remote.join_accepted && remote.admitted_epoch == epoch &&
           memcmp(remote.admitted_session, session, 16) == 0;
}

// Pump-owned counters, separate from the wire/public peer snapshot. HELLO alone must not
// be mistaken for a healthy pose stream. Report both sending and receiving per peer.
struct PoseStreamStats {
    unsigned long long tx_ok, tx_failed, rx_seen, rx_applied, rx_blocked, rx_stale;
    double last_rx, last_applied;
    int last_send_errno;
    const char* last_rx_block;
};
PoseStreamStats g_pose_stream[kMaxPeers]{};

void init_peers_lock() {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_peers_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

// The pump thread reads this snapshot, the game thread writes it.
pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;
LocalState g_local{};
double g_local_publish_at = 0.0;
uint64_t g_local_serial = 0;
// Piloted-vehicle snapshot, same discipline: written every frame while driving.
VehicleState g_local_vehicle{};

struct LocalSession {
    pthread_mutex_t lock;  // guards every field below; pump and game thread share it
    SessionState state;    // UI-facing state (Discovering/Host/Guest/Failed/...)
    SessionJoinStatus reason;  // machine-readable reason for the current state
    char detail[128];          // human-readable detail for logs and future UI
    bool active;               // false = no session selected or joined at all
    bool world_active;  // gameplay gate: set by the game thread while a world is live,
                        // independent of when the save subsystem exposes the slot
    bool local_has_world;  // a world is loaded here (from net_session_set_local_world)
    bool local_member;     // this phone belongs to session_id below
    bool local_host;       // this phone is the session's world authority
    SessionLaunchRole launch_role;
    bool role_reset_pending;
    bool resume_host_slot; // loaded a peer-received slot after losing in-memory JOIN state
    uint32_t epoch;        // incremented on every host migration; higher wins
    uint32_t host_id;      // current host's peer id (g_local_id when we host)
    uint32_t presence_sequence;  // our own presence counter (reorder guard for peers)
    uint8_t session_id[16];      // random per host launch; preserved across migration
    uint8_t content_guid[16];    // gameinfo.json session GUID: the WORLD identity,
                                 // as opposed to session_id which is the network identity
    bool content_guid_valid;
    char slot[24];  // host slot name being shared (ours when hosting)
    // Explicitly selected join target (future UI or auto-join policy). Kept separate
    // from the discovered roster so seeing another session never changes our role.
    bool selected;
    uint32_t selected_host;
    uint32_t selected_epoch;
    uint8_t selected_session[16];
    uint8_t selected_guid[16];
    char selected_slot[24];
    uint64_t join_nonce;  // random per join attempt; lets the host answer repeats
                          // idempotently instead of opening a second slot
    bool join_request_pending;  // a JOIN is in flight (1/sec retry until answered)
    bool join_accepted;         // host said yes; enables world requests
    double join_started;        // pump timestamp of the current attempt (diagnostics)
    double last_join_send;      // last JOIN transmit (1/sec throttle)
    // Earliest time a new JOIN may go out after a refusal. Without this a refused
    // guest reselects the same host on the next pump iteration and the pair
    // request/refuses at pump rate. Version mismatches back off longer.
    double join_retry_after;
    double last_host_presence;  // last time the current host's presence was seen
    double election_started;    // 0 = no election running; otherwise start timestamp
                                // of the 2 s roster-convergence wait
    bool presence_dirty;        // state changed: broadcast presence immediately,
                                // don't wait for the 1 s heartbeat
    bool explicit_leave;        // UI asked to leave; consumed once by the pump
    double leave_cooldown_until;  // auto-join stays off this long after a manual leave
    double world_request_last;    // last WORLD_REQUEST transmit (2 s cadence, 10 s retry)
    bool world_refresh_pending;   // host's presence carries a newer content GUID than ours;
                                  // cleared once the refresh transfer starts
    uint32_t host_world_gen_seen;  // last host offer generation we have on disk (v4)
};

LocalSession g_session = {PTHREAD_MUTEX_INITIALIZER};

// The published offer is a cache, not the authority. A role flicker between save ticks clears
// it via net_offer_world(nullptr), and the next save tick may only republish seconds later -
// that window used to strand every joining guest with "offer unavailable" refusals. The world
// itself is alive here and its slot files sit on disk, so rebuild the publication from those
// files and publish it like any other generation. File reads need no Unity, so this is
// pump-safe; if a file is caught mid-autosave the capture fails cleanly and the guest pumps
// again later, exactly the same behavior as a world too large to stage.
void rebuild_offer_from_disk() {
    // A full capture is roughly thirty kilobytes, so one attempt costs nothing - but a guest
    // waiting for a manifest repumps every second, and a capture caught mid-autosave would
    // retry at that rate until the save finishes writing. A short cooldown keeps the disk
    // from being re-read faster than the game could plausibly finish an autosave.
    static double last_attempt = 0.0;
    double now = monotonic_seconds();
    if (now - last_attempt < 2.0) return;
    last_attempt = now;

    char serve_slot[24]{};
    pthread_mutex_lock(&g_session.lock);
    bool can_serve = g_session.active && g_session.local_member && g_session.slot[0];
    if (can_serve) snprintf(serve_slot, sizeof(serve_slot), "%s", g_session.slot);
    pthread_mutex_unlock(&g_session.lock);
    if (!can_serve) return;
    WorldPayload disk;
    if (world_disk_capture(serve_slot, &disk)) {
        net_offer_world(&disk, serve_slot);
        world_payload_free(&disk);
        LOG_INFO("world: offer was cleared, rebuilt it from the slot on disk");
    }
}

void adopt_received_world_identity(uint32_t generation) {
    uint8_t received_guid[16]{};
    if (!world_receive_content_guid(received_guid)) return;
    pthread_mutex_lock(&g_session.lock);
    memcpy(g_session.content_guid, received_guid, sizeof(g_session.content_guid));
    g_session.content_guid_valid = true;
    g_session.host_world_gen_seen = generation;
    g_session.world_refresh_pending = false;
    pthread_mutex_unlock(&g_session.lock);
}
double monotonic_seconds() {    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

void sleep_millis(long millis) {
    struct timespec ts;
    ts.tv_sec = millis / 1000;
    ts.tv_nsec = (millis % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}

uint32_t make_local_id() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    uint32_t seed = static_cast<uint32_t>(tv.tv_sec) ^ static_cast<uint32_t>(tv.tv_usec);
    // Mix the bits so two devices booting in the same millisecond rarely collide.
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed ? seed : 1;
}

void session_set_state_locked(SessionState state, SessionJoinStatus reason, const char* detail) {
    if (g_session.state == state && g_session.reason == reason &&
        strcmp(g_session.detail, detail ? detail : "") == 0) {
        return;
    }
    g_session.state = state;
    g_session.reason = reason;
    snprintf(g_session.detail, sizeof(g_session.detail), "%s", detail ? detail : "");
    g_session.presence_dirty = true;
    LOG_INFO("session state: state=%d reason=%d detail=%s", int(state), int(reason),
             g_session.detail);
}

bool random_bytes(uint8_t* out, size_t size) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t done = 0;
        while (done < size) {
            ssize_t got = read(fd, out + done, size - done);
            if (got <= 0) break;
            done += static_cast<size_t>(got);
        }
        close(fd);
        if (done == size) return true;
    }
    // Last-resort fallback keeps the session usable on a device with no urandom node.
    struct timeval tv{};
    gettimeofday(&tv, nullptr);
    uint64_t state = (static_cast<uint64_t>(tv.tv_sec) << 32) ^ tv.tv_usec ^
                     (static_cast<uint64_t>(g_local_id) << 17);
    for (size_t i = 0; i < size; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        out[i] = static_cast<uint8_t>(state >> 24);
    }
    return false;
}

bool fixed_string_valid(const char* value, size_t capacity) {
    if (!value || capacity == 0) return false;
    const void* end = memchr(value, '\0', capacity);
    if (!end) return false;
    for (const char* p = value; p < static_cast<const char*>(end); ++p) {
        if (static_cast<unsigned char>(*p) < 0x20) return false;
    }
    return true;
}

bool fixed_string_zero_padded(const char* value, size_t capacity) {
    if (!fixed_string_valid(value, capacity)) return false;
    const char* end = static_cast<const char*>(memchr(value, '\0', capacity));
    for (const char* p = end; p < value + capacity; ++p)
        if (*p != '\0') return false;
    return true;
}

bool header_reserved_zero(const PacketHeader& header) {
    return header.reserved[0] == 0 && header.reserved[1] == 0 &&
           header.reserved[2] == 0;
}

bool bytes_nonzero(const uint8_t* value, size_t size) {
    uint8_t bits = 0;
    for (size_t i = 0; i < size; ++i) bits |= value[i];
    return bits != 0;
}

bool same_session(const uint8_t a[16], const uint8_t b[16]) {
    return memcmp(a, b, 16) == 0;
}

bool same_content(const uint8_t a[16], const uint8_t b[16]) {
    return memcmp(a, b, 16) == 0;
}

void fill_address(const struct sockaddr_in& addr, char* out) {
    const char* text = inet_ntoa(addr.sin_addr);
    snprintf(out, 16, "%s", text ? text : "0.0.0.0");
}

PeerState* find_peer_by_id(uint32_t id) {
    for (size_t i = 0; i < kMaxPeers; i++) {
        if (g_peers[i].active && g_peers[i].id == id) return &g_peers[i];
    }
    return nullptr;
}

int peer_index_by_id(uint32_t id) {
    for (size_t i = 0; i < kMaxPeers; ++i) {
        if (g_peers[i].active && g_peers[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

PeerState* claim_peer(uint32_t id, const struct sockaddr_in& addr, double now) {
    if (id == g_local_id) return nullptr;

    PeerState* existing = find_peer_by_id(id);
    if (existing) {
        // A peer that only says hello must stay alive too, and its address can change
        // when it reconnects from a different port.
        if (existing->raw.sin_addr.s_addr != addr.sin_addr.s_addr ||
            existing->raw.sin_port != addr.sin_port) {
            char address[16]{};
            fill_address(addr, address);
            LOG_INFO("peer endpoint: %08x %s:%u -> %s:%u", id, existing->address,
                     existing->port, address, ntohs(addr.sin_port));
        }
        existing->last_seen = now;
        existing->port = ntohs(addr.sin_port);
        existing->raw = addr;
        fill_address(addr, existing->address);
        return existing;
    }

    for (size_t i = 0; i < kMaxPeers; i++) {
        if (g_peers[i].active) continue;
        memset(&g_peers[i], 0, sizeof(g_peers[i]));
        memset(&g_remote[i], 0, sizeof(g_remote[i]));
        g_pose_stream[i] = {};
        g_peers[i].active = true;
        g_peers[i].id = id;
        g_peers[i].port = ntohs(addr.sin_port);
        g_peers[i].last_seen = now;
        g_peers[i].last_sent = 0.0;
        g_peers[i].raw = addr;
        fill_address(addr, g_peers[i].address);
        LOG_INFO("peer %08x joined from %s:%u", id, g_peers[i].address, g_peers[i].port);
        return &g_peers[i];
    }
    return nullptr;
}

// A failed send is reported per destination at most once every kSendErrorInterval seconds. The
// old single-slot memo only compared against the last destination, so a broadcast and a peer
// unicast failing back to back reset it every time and both printed on every hello. The usual
// cause next to an otherwise working link is the app sitting in the background: Android blocks
// the socket with EPERM until it is foregrounded again, and one line per interval says that.
constexpr double kSendErrorInterval = 15.0;
int send_to(const void* data, size_t size, const struct sockaddr_in& addr,
            int* error_out) {
    if (error_out) *error_out = 0;
    if (g_socket < 0) {
        if (error_out) *error_out = EBADF;
        return -1;
    }
    int sent = static_cast<int>(sendto(g_socket, data, size, 0,
                                        reinterpret_cast<const struct sockaddr*>(&addr),
                                        sizeof(addr)));
    if (sent < 0) {
        int err = errno;
        if (error_out) *error_out = err;
        char key[64];
        snprintf(key, sizeof(key), "%s:%u", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

        struct ErrorMemo {
            char key[64];
            double next;
        };
        static ErrorMemo memos[8] = {};
        double now = monotonic_seconds();
        ErrorMemo* slot = nullptr;
        for (ErrorMemo& memo : memos) {
            if (strcmp(memo.key, key) == 0) {
                slot = &memo;
                break;
            }
        }
        if (!slot) {
            for (ErrorMemo& memo : memos) {
                if (memo.key[0] == '\0') {
                    slot = &memo;
                    break;
                }
            }
        }
        if (!slot) slot = &memos[0];
        if (now >= slot->next) {
            snprintf(slot->key, sizeof(slot->key), "%s", key);
            slot->next = now + kSendErrorInterval;
            LOG_WARN("sendto %s failed: %s", key, strerror(err));
        }
    }
    return sent;
}

// Android refuses sendto(255.255.255.255) with EPERM unless the app holds a
// WifiManager multicast lock, so the subnet directed broadcast address is used instead.
//
// A device usually has several AF_INET interfaces (wlan0/wlan1, tethering, VPN, cellular) and a
// subnet broadcast is only useful on the one shared with the other phones. The Wi-Fi interface is
// therefore preferred by name rather than taking whichever interface getifaddrs lists first, and
// the cellular interfaces are skipped outright - Android rejects a broadcast sent out of them.
bool is_wifi_interface(const char* name) { return strncmp(name, "wlan", 4) == 0; }
bool is_cellular_interface(const char* name) {
    return strncmp(name, "rmnet", 5) == 0 || strncmp(name, "ccmni", 5) == 0 ||
           strncmp(name, "pdp", 3) == 0;
}

bool broadcast_for_interface(struct ifaddrs* interfaces, bool wifi_only,
                             struct sockaddr_in* out) {
    for (struct ifaddrs* it = interfaces; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        if (!it->ifa_netmask) continue;
        if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK)) continue;
        if (wifi_only) {
            if (!is_wifi_interface(it->ifa_name)) continue;
        } else if (is_cellular_interface(it->ifa_name)) {
            continue;
        }

        uint32_t ip = ntohl(reinterpret_cast<struct sockaddr_in*>(it->ifa_addr)->sin_addr.s_addr);
        uint32_t mask = ntohl(reinterpret_cast<struct sockaddr_in*>(it->ifa_netmask)->sin_addr.s_addr);
        if (ip == 0 || mask == 0) continue;

        out->sin_addr.s_addr = htonl(ip | ~mask);
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_INFO("broadcast target %s via %s", inet_ntoa(out->sin_addr), it->ifa_name);
        }
        return true;
    }
    return false;
}

void fill_subnet_broadcast(struct sockaddr_in* out) {
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(kLanPort);
    out->sin_addr.s_addr = htonl(INADDR_BROADCAST);

    struct ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) return;

    if (!broadcast_for_interface(interfaces, true, out)) {
        broadcast_for_interface(interfaces, false, out);
    }
    freeifaddrs(interfaces);
}

void send_hello() {
    HelloPacket packet{};
    packet.header.magic = kMagic;
    packet.header.type = kPacketHello;
    packet.id = g_local_id;
    packet.flags = net_session_is_host() ? kHelloIsHost : 0u;

    // Subnet broadcast reaches every device on the Wi-Fi without a server.
    struct sockaddr_in broadcast{};
    fill_subnet_broadcast(&broadcast);
    send_to(&packet, sizeof(packet), broadcast);

    // Unicast to known peers as well: some access points drop broadcast traffic.
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    for (size_t i = 0; i < kMaxPeers; i++) {
        if (!g_peers[i].active) continue;
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(g_peers[i].port);
        inet_pton(AF_INET, g_peers[i].address, &addr.sin_addr);
        send_to(&packet, sizeof(packet), addr);
    }
    pthread_mutex_unlock(&g_peers_lock);
}

bool local_session_copy(SessionPresencePacket* packet, uint8_t* local_flags_out = nullptr) {
    if (!packet) return false;
    memset(packet, 0, sizeof(*packet));
    pthread_mutex_lock(&g_session.lock);
    packet->header.magic = kMagic;
    packet->header.type = kPacketSessionPresence;
    packet->protocol_version = kSessionProtocolVersion;
    packet->packet_size = sizeof(*packet);
    packet->sender_id = g_local_id;
    packet->sequence = ++g_session.presence_sequence;
    packet->session_epoch = g_session.epoch;
    packet->host_id = g_session.host_id;
    if (g_session.local_host && g_session.local_member) packet->flags |= kSessionFlagIsHost;
    if (g_session.local_has_world && g_session.local_member)
        packet->flags |= kSessionFlagHasWorld;
    if (g_session.local_member) packet->flags |= kSessionFlagMember;
    if (g_session.local_member)
        memcpy(packet->session_id, g_session.session_id, sizeof(packet->session_id));
    if (g_session.local_member && g_session.content_guid_valid)
        memcpy(packet->content_guid, g_session.content_guid, sizeof(packet->content_guid));
    snprintf(packet->slot, sizeof(packet->slot), "%s", g_session.slot);
    pthread_mutex_unlock(&g_session.lock);
    // Offer generation out of the session guard on purpose: lock order elsewhere is
    // session -> offer (rebuild_offer_from_disk takes session, then offer), matching here.
    pthread_mutex_lock(&g_offer_lock);
    packet->world_gen = (g_offer_snapshot && g_offer_snapshot->valid)
                            ? static_cast<uint32_t>(g_offer_snapshot->generation)
                            : 0;
    pthread_mutex_unlock(&g_offer_lock);

    packet->max_players = static_cast<uint8_t>(kMaxPeers + 1);
    snprintf(packet->game_build, sizeof(packet->game_build), "%s", kGameBuild);
    snprintf(packet->mod_version, sizeof(packet->mod_version), "%s", kModVersion);
    if (local_flags_out) *local_flags_out = packet->flags;

    // Count the host plus members with a loaded world. A menu-only member is not a player yet.
    uint8_t players = (packet->flags & kSessionFlagIsHost) &&
                              (packet->flags & kSessionFlagHasWorld)
                          ? 1
                          : 0;
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    for (size_t i = 0; i < kMaxPeers; ++i) {
        const RemotePresence& remote = g_remote[i];
        if (!g_peers[i].active || !remote.valid ||
            (remote.flags & kSessionFlagMember) == 0 ||
            (remote.flags & kSessionFlagHasWorld) == 0 ||
            !same_session(remote.session_id, packet->session_id)) {
            continue;
        }
        if (players < packet->max_players) ++players;
    }
    pthread_mutex_unlock(&g_peers_lock);
    packet->player_count = players;
    return true;
}

void send_session_presence() {
    SessionPresencePacket packet{};
    local_session_copy(&packet);
    struct sockaddr_in broadcast{};
    fill_subnet_broadcast(&broadcast);
    send_to(&packet, sizeof(packet), broadcast);

    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    for (size_t i = 0; i < kMaxPeers; ++i) {
        if (!g_peers[i].active) continue;
        send_to(&packet, sizeof(packet), g_peers[i].raw);
    }
    pthread_mutex_unlock(&g_peers_lock);
    pthread_mutex_lock(&g_session.lock);
    g_session.presence_dirty = false;
    pthread_mutex_unlock(&g_session.lock);
}

#include "session_peer_gate.inc"

int send_state(const PeerState& peer, const LocalState& local, int* error_out) {
    StatePacket packet{};
    packet.header.magic = kMagic;
    packet.header.type = kPacketState;
    packet.id = g_local_id;
    packet.sequence = ++g_next_sequence;
    packet.x = local.x;
    packet.y = local.y;
    packet.z = local.z;
    packet.yaw = local.yaw;
    packet.pitch = local.pitch;
    packet.view_yaw = local.view_yaw;
    packet.flags = local.flags;
    int float_count = local.anim_float_count;
    if (float_count < 0) float_count = 0;
    if (float_count > kNetAnimFloats) float_count = kNetAnimFloats;
    packet.anim_float_count = static_cast<uint8_t>(float_count);
    for (int i = 0; i < float_count; i++) {
        packet.anim_floats[i] = local.anim_floats[i];
    }
    packet.anim_underwater = local.anim_underwater;
    packet.anim_cinematics = local.anim_cinematics;
    packet.held_tech = local.held_tech;
    packet.suit_tech = local.suit_tech;
    packet.fins_tech = local.fins_tech;
    packet.head_tech = local.head_tech;
    memcpy(packet.environment_id,local.environment_id,sizeof(packet.environment_id));
    HeldRotation rotation{};
    packet.held_rotation_valid=local.held_tech && local.held_rotation_valid &&
        held_rotation_normalize(local.held_rotation,&rotation);
    packet.held_rotation=rotation;
    packet.flags&=~kFlagHeldWorldPose;
    if (local.held_tech==751 && packet.held_rotation_valid &&
        (local.flags&kFlagHeldWorldPose) && held_offset_valid(local.held_offset)) {
        packet.flags|=kFlagHeldWorldPose;
        packet.held_offset=local.held_offset;
    }
    return send_to(&packet, sizeof(packet), peer.raw, error_out);
}

void send_vehicle(const PeerState& peer, const VehicleState& vehicle) {
    VehiclePacket packet{};
    packet.header.magic = kMagic;
    packet.header.type = kPacketVehicle;
    packet.id = g_local_id;
    packet.sequence = ++g_next_sequence;
    memcpy(packet.guid, vehicle.guid, sizeof(packet.guid));
    memcpy(packet.classId, vehicle.classId, sizeof(packet.classId));
    packet.x = vehicle.x;
    packet.y = vehicle.y;
    packet.z = vehicle.z;
    packet.yaw = vehicle.yaw;
    packet.pitch = vehicle.pitch;
    packet.roll = vehicle.roll;
    send_to(&packet, sizeof(packet), peer.raw);
}

// Transfer-side admission control. The host serves chunks only to guests it accepted
// (join_accepted on its own roster); a guest accepts manifests/chunks/refusals only from
// the host of the session it actually joined, matched by id, address AND session id.
// Anything else is rejected with a log line - an unauthenticated LAN peer can otherwise
// inject world bytes into either side. Called with g_peers_lock held.
bool authorize_world_sender(uint32_t id, const struct sockaddr_in& addr, bool from_host) {
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    int index = peer_index_by_id(id);
    bool authorized = false;
    if (index >= 0) {
        const RemotePresence& remote = g_remote[index];
        bool same_address = g_peers[index].raw.sin_addr.s_addr == addr.sin_addr.s_addr &&
                            g_peers[index].raw.sin_port == addr.sin_port;
        bool fresh_presence = monotonic_seconds() - remote.last_presence <= kPeerTimeout;
        pthread_mutex_lock(&g_session.lock);
        bool same = fresh_presence && g_session.local_member && remote.valid && remote.protocol_v3 &&
                    same_session(remote.session_id, g_session.session_id) &&
                    remote.epoch == g_session.epoch && same_address;
        if (from_host) {
            authorized = same && !g_session.local_host && g_session.join_accepted &&
                         id == g_session.host_id && remote.host_id == id &&
                         (remote.flags & (kSessionFlagIsHost | kSessionFlagMember)) ==
                             (kSessionFlagIsHost | kSessionFlagMember);
        } else {
            // The original host records JOIN acceptance directly. After migration, a
            // surviving SESSION_MEMBER proves prior admission through the shared session
            // roster; its refreshed Presence must name this host and epoch.
            bool accepted_member = join_grant_matches(remote, g_session.session_id, g_session.epoch) ||
                                   (g_session.epoch > 1 &&
                                    (remote.flags & kSessionFlagMember));
            authorized = same && g_session.local_host && accepted_member &&
                         remote.host_id == g_local_id;
        }
        pthread_mutex_unlock(&g_session.lock);
    }
    pthread_mutex_unlock(&g_peers_lock);
    return authorized;
}

#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
#include "base_geometry_channel.inc"
#endif

void handle_world_packet(const uint8_t* data, size_t size, const struct sockaddr_in& addr,
                         double now) {
    if (size < sizeof(PacketHeader)) return;

    PacketHeader header{};
    memcpy(&header, data, sizeof(header));
    if (header.type >= kPacketWorldRequest && header.type <= kPacketWorldRefuse) {
        if (size < sizeof(PacketHeader) + sizeof(uint32_t)) return;
        uint32_t sender_id = 0;
        memcpy(&sender_id, data + sizeof(PacketHeader), sizeof(sender_id));
        bool from_host = header.type == kPacketWorldManifest ||
                         header.type == kPacketWorldChunk ||
                         header.type == kPacketWorldRefuse;
        if (!authorize_world_sender(sender_id, addr, from_host)) {
            LOG_WARN("world: packet type %u from unjoined/out-of-session peer %08x rejected",
                     header.type, sender_id);
            return;
        }
    }

    if (header.type == kPacketWorldRequest) {
        if (size < sizeof(WorldRequestPacket)) return;
        WorldRequestPacket packet{};
        memcpy(&packet, data, sizeof(packet));
        if (packet.id == g_local_id) return;

        // Offer presence is checked under the lock; the request handler itself never
        // touches the snapshot's contents. A cleared publication is not a refusal reason
        // when the host still owns a live world: the slot files on disk are servable, and
        // the claim path below rebuilds the offer from them on demand.
        pthread_mutex_lock(&g_offer_lock);
        bool can_serve = g_offer_snapshot && g_offer_snapshot->valid;
        pthread_mutex_unlock(&g_offer_lock);
        if (!can_serve) {
            char probe_slot[24]{};
            pthread_mutex_lock(&g_session.lock);
            bool host_path = g_session.active && g_session.local_host &&
                             g_session.local_has_world && g_session.local_member &&
                             g_session.slot[0];
            if (host_path) snprintf(probe_slot, sizeof(probe_slot), "%s", g_session.slot);
            pthread_mutex_unlock(&g_session.lock);
            can_serve = host_path && world_disk_ready(probe_slot);
        }

        if (!can_serve) {
            // Nothing loaded, or the world was too large to stage. Saying so plainly beats
            // letting the guest wait for a manifest that will never arrive.
            static WorldGuest unknown;
            memset(&unknown, 0, sizeof(unknown));
            unknown.id = packet.id;
            unknown.raw = addr;
            send_refuse(unknown, "no world loaded on this device");
            return;
        }
        WorldGuest* guest = claim_guest(packet.id, addr);
        if (!guest) return;
        guest->request_seen = true;
        if (!guest->slot_known) {
            pthread_mutex_lock(&g_offer_lock);
            if (g_offer_snapshot) {
                snprintf(guest->slot, sizeof(guest->slot), "%s", g_offer_snapshot->slot);
            }
            pthread_mutex_unlock(&g_offer_lock);
            guest->slot_known = true;
        }
        // No check for world_slot_exists() here, deliberately. That call reads this device's own
        // save directory, and this device is the host: the slot is the one it is currently
        // playing, so it always exists and every guest would be refused. Whether the guest
        // already holds the slot is only knowable on the guest, and it decides that itself when
        // the manifest arrives and it finds the files already written.
        // The guest repeats its request until it is satisfied, so a request arriving mid
        // transfer is normal and must not throw away acknowledged bytes. Only a guest that has
        // never acked anything is rewound, which is the case of a guest that restarted with
        // empty buffers and is now waiting on a manifest it no longer has.
        if (guest->total_chunks != 0 && guest->acked == 0 && guest->sent_upto != 0) {
            LOG_INFO("world: guest %08x restarted with empty buffers, rewinding", guest->id);
            guest->total_chunks = 0;
            guest->acked = 0;
            guest->sent_upto = 0;
            guest->last_send = 0.0;
        }
        return;
    }

    if (header.type == kPacketWorldManifest) {
        if (size < sizeof(WorldManifestPacket)) return;
        WorldManifestPacket packet{};
        memcpy(&packet, data, sizeof(packet));
        if (packet.id == g_local_id) return;

        // A manifest while a transfer is already running means the host restarted the
        // transfer. Start over rather than interleaving two sets of buffers.
        world_reset_receive();

        if (!fixed_string_zero_padded(packet.slot, sizeof(packet.slot))) {
            set_handoff(kWorldHandoffFailed, "", "manifest slot name is malformed");
            return;
        }
        for (int i = 0; i < kWorldFileCount; i++) {
            if (!fixed_string_zero_padded(packet.files[i].name,
                                          sizeof(packet.files[i].name))) {
                set_handoff(kWorldHandoffFailed, packet.slot,
                            "manifest filename is malformed");
                return;
            }
        }

        WorldManifest manifest{};
        snprintf(manifest.slot, sizeof(manifest.slot), "%s", packet.slot);
        manifest.file_count = packet.file_count;
        for (int i = 0; i < kWorldFileCount; i++) {
            snprintf(manifest.files[i].name, sizeof(manifest.files[i].name), "%s",
                     packet.files[i].name);
            manifest.files[i].size = packet.files[i].size;
            manifest.files[i].checksum = packet.files[i].checksum;
        }
        // Remember the generation the host is serving; the finalize consumes it through
        // adopt_received_world_identity. Reset together with the buffers (above), so a
        // failed transfer never adopts a stale value.
        g_receive_generation = packet.generation;

        if (!world_begin_receive(manifest)) {
            LOG_WARN("world: manifest from %08x rejected", packet.id);
            set_handoff(kWorldHandoffFailed, packet.slot, "manifest rejected");
            return;
        }
        set_handoff(kWorldHandoffReceiving, manifest.slot, "receiving");
        return;
    }

    if (header.type == kPacketWorldAck) {
        if (size < sizeof(WorldAckPacket)) return;
        WorldAckPacket packet{};
        memcpy(&packet, data, sizeof(packet));
        if (packet.id == g_local_id) return;

        WorldGuest* guest = find_guest(packet.id);
        if (!guest || guest->total_chunks == 0) return;

        // Clamped rather than trusted: the ack is the only thing that moves the send position,
        // and a guest that overstates its progress would make the host skip chunks forever.
        if (packet.received > guest->total_chunks) {
            LOG_WARN("world: guest %08x acked %u of %u chunks, clamped", guest->id,
                     packet.received, guest->total_chunks);
            packet.received = guest->total_chunks;
        }
        if (packet.received > guest->acked) {
            guest->acked = packet.received;
            LOG_INFO("world: guest %08x acknowledged %u/%u chunks", guest->id, guest->acked,
                     guest->total_chunks);
        }
        return;
    }

    if (header.type == kPacketWorldChunk) {
        if (size < sizeof(WorldChunkPacket)) return;
        WorldChunkPacket packet{};
        memcpy(&packet, data, sizeof(packet));
        if (packet.id == g_local_id) return;
        if (packet.payload_size == 0 || packet.payload_size > kWorldChunkPayload) return;

        if (!world_receive_chunk(packet.file_index, packet.offset, packet.payload,
                                 packet.payload_size)) {
            // False also covers a chunk past the file end, but the common case here is
            // buffers gone (reset after a failed transfer, manifest never armed). Rate-lim-
            // ited: a stalled rewind paints one line per few seconds, not one per chunk.
            static double last_discard = -60.0;
            double now_s = monotonic_seconds();
            if (now_s - last_discard >= 5.0) {
                last_discard = now_s;
                LOG_WARN("world: chunk (file %d, offset %u) discarded, no receive buffers;"
                         " waiting for the guest to re-request",
                         packet.file_index, packet.offset);
            }
            return;
        }

        const WorldManifest* manifest = world_receive_manifest();
        if (!manifest) return;

        uint32_t received = 0;
        bool complete = false;
        guest_progress(*manifest, &received, &complete);

        if (complete) {
            // The guest owns the receive buffers, so the guest is the one that has to verify
            // and write the slot. The host holds no copy and could not check anything.
            //
            // world_write_slot releases the receive buffers when it is done, and releasing them
            // clears the manifest. The slot name is therefore copied out first: reporting the
            // outcome - and handing the name to the load step - needs it after that point.
            char slot[sizeof(manifest->slot)];
            snprintf(slot, sizeof(slot), "%s", manifest->slot);

            // Verify BEFORE trusting anything on disk. A partial slot left behind by an
            // earlier failed transfer must not be mistaken for success, and a checksum
            // mismatch must be reported as failure - never as ok.
            bool ok = false;
            if (!world_receive_verify(*manifest)) {
                set_handoff(kWorldHandoffFailed, slot, "checksum mismatch, nothing written");
                world_reset_receive();
            } else {
                uint8_t received_guid[16]{};
                uint8_t expected_guid[16]{};
                pthread_mutex_lock(&g_session.lock);
                bool expected_valid = g_session.content_guid_valid &&
                                      !g_session.local_host &&
                                      g_session.local_member;
                bool allow_new_guid = g_session.world_refresh_pending;
                const bool preserve_local = expected_valid && g_session.launch_role==kLaunchGuest &&
                    !g_session.world_active && g_session.join_accepted && !g_session.explicit_leave;
                memcpy(expected_guid, g_session.content_guid, sizeof(expected_guid));
                pthread_mutex_unlock(&g_session.lock);
                // Refresh exception: when the guest is pulling a newer world on purpose
                // (world_refresh_pending), the received guid legitimately differs from
                // the session's remembered one - accepting it is the whole point. The
                // baseline is armed by adopt_received_world_identity below.
                bool guid_ok = expected_valid &&
                               world_receive_content_guid(received_guid) &&
                               (same_content(received_guid, expected_guid) ||
                                allow_new_guid);
                if (!guid_ok) {
                    LOG_ERROR("world: received content GUID does not match selected session");
                    set_handoff(kWorldHandoffFailed, slot, "content GUID mismatch, nothing written");
                    world_reset_receive();
                } else {
                    WorldSlotCheck slot_check = world_slot_exists(slot, *manifest);
                    // An explicit guest can have the former host's byte-identical
                    // native save. It still needs the transaction below: retain the
                    // native original and commit the guest marker before autoload.
                    const bool adopt_native = preserve_local && !world_slot_is_host_received(slot);
                    if (slot_check == kWorldSlotMatches && !adopt_native) {
                        adopt_received_world_identity(g_receive_generation);
                        set_handoff(kWorldHandoffWritten, slot,
                                    "slot already on disk, all manifest files verified");
                        LOG_INFO("world: slot %s exactly matches manifest, skipping write", slot);
                        world_reset_receive();
                        ok = true;
                    } else if (slot_check == kWorldSlotConflict ||
                               (slot_check == kWorldSlotMatches && adopt_native)) {
                        // A slot this device received from a peer earlier may be refreshed by
                        // the same peer's newer copy: that is the "host published a newer
                        // world" flow, and the stale local bytes are exactly what should be
                        // replaced. A slot the game built locally has no marker and the
                        // strict refusal stands.
                        if (!world_slot_is_host_received(slot) && !preserve_local) {
                            set_handoff(kWorldHandoffFailed, slot,
                                        "existing slot conflicts; overwrite is forbidden");
                            world_reset_receive();
                        } else if (!world_write_slot(*manifest,preserve_local)) {
                            set_handoff(kWorldHandoffFailed, slot,
                                        "slot could not be written");
                            world_reset_receive();
                        } else {
                            adopt_received_world_identity(g_receive_generation);
                            set_handoff(kWorldHandoffWritten, slot,
                                        "peer-received slot refreshed from a newer host copy");
                            LOG_INFO("world: slot %s refreshed, waiting for the load step",
                                     slot);
                            ok = true;
                        }
                    } else if (slot_check == kWorldSlotCheckError) {
                        set_handoff(kWorldHandoffFailed, slot,
                                    "existing slot conflicts; overwrite is forbidden");
                        world_reset_receive();
                    } else if (!world_write_slot(*manifest)) {
                        set_handoff(kWorldHandoffFailed, slot, "slot could not be written");
                        world_reset_receive();
                    } else {
                        adopt_received_world_identity(g_receive_generation);
                        set_handoff(kWorldHandoffWritten, slot,
                                    "slot on disk, not loaded yet");
                        LOG_INFO("world: slot %s verified and written, waiting for the load step",
                                 slot);
                        ok = true;
                    }
                }
            }

            // Tell the host how it went. On failure the buffers above are dropped, so the
            // host must rewind and the guest re-requests (see the request gate in pump):
            // an ok=1 on a failed transfer would stop the resends and stall forever.
            WorldCompletePacket done{};
            done.header.magic = kMagic;
            done.header.type = kPacketWorldComplete;
            done.id = g_local_id;
            done.ok = ok ? 1 : 0;
            send_to(&done, sizeof(done), addr);
            return;
        }

        WorldAckPacket ack{};
        ack.header.magic = kMagic;
        ack.header.type = kPacketWorldAck;
        ack.id = g_local_id;
        ack.file_index = packet.file_index;
        ack.received = received;
        send_to(&ack, sizeof(ack), addr);
        return;
    }

    if (header.type == kPacketWorldComplete) {
        if (size < sizeof(WorldCompletePacket)) return;
        WorldCompletePacket packet{};
        memcpy(&packet, data, sizeof(packet));
        if (packet.id == g_local_id) return;

        WorldGuest* guest = find_guest(packet.id);
        if (!guest) return;
        if (!packet.ok) {
            // The guest dropped its buffers and will re-request. The acked position
            // it reported before is gone with them: rewind so the resend restarts
            // from zero instead of skipping chunks the guest no longer has. Clearing
            // total_chunks matters as much as clearing the position: pump_guest treats
            // a non-zero count as "manifest already sent" and would only resend chunks,
            // which this guest now drops until a fresh manifest arrives.
            guest->total_chunks = 0;
            guest->acked = 0;
            guest->sent_upto = 0;
            guest->last_send = 0.0;
            guest->finished = false;
            LOG_WARN("world: guest %08x reported a failed transfer, rewound to chunk 0",
                     guest->id);
            return;
        }

        // The guest has the buffers, so it did the verifying and the writing. All that is left
        // here is to stop sending: verifying on this side would read buffers that were never
        // allocated on this side.
        guest->finished = true;
        LOG_INFO("world: guest %08x confirmed slot %s is on its disk", guest->id, guest->slot);
        release_guest(guest);
        return;
    }

    if (header.type == kPacketWorldRefuse) {
        if (size < sizeof(WorldCompletePacket)) return;
        WorldCompletePacket packet{};
        memcpy(&packet, data, sizeof(packet));
        if (packet.id == g_local_id) return;

        WorldGuest* guest = find_guest(packet.id);
        if (guest && !guest->finished) {
            guest->finished = true;
            LOG_WARN("world: guest %08x refused the world transfer", guest->id);
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Reliable world events: send
// ---------------------------------------------------------------------------

// The peer's slot index doubles as its bit in EventSlot::acked. Peers are recycled rather than
// compacted, so an index that comes back means a different phone and any bit set for it has to
// be cleared - otherwise a new peer inherits acknowledgements it never sent.
uint32_t event_peer_bit(uint32_t peer_id) {
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    uint32_t bit = 0;
    for (size_t i = 0; i < kMaxPeers; i++) {
        if (g_peers[i].active && g_peers[i].id == peer_id &&
            session_peer_eligible_locked(i, true)) {
            bit = 1u << i;
            break;
        }
    }
    pthread_mutex_unlock(&g_peers_lock);
    return bit;
}

// Everyone currently online, as a bitmask. Read under the event lock so it is consistent with
// the acked bits it is compared against.
uint32_t active_peer_mask() {
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    uint32_t mask = 0;
    for (size_t i = 0; i < kMaxPeers; i++) {
        if (!g_peers[i].active) continue;
        if (g_peers[i].last_seen + kPeerTimeout <= monotonic_seconds()) continue;
        if (!session_peer_eligible_locked(i, true)) continue;
        mask |= 1u << i;
    }
    pthread_mutex_unlock(&g_peers_lock);
    return mask;
}

void send_world_event(const EventSlot& slot, const struct sockaddr_in& addr) {
    WorldEventPacket packet{};
    packet.header.magic = kMagic;
    packet.header.type = kPacketWorldEvent;
    // id is this phone, because it is the one sending. origin_id stays the player whose action
    // caused it, which is what lets the host relay without rewriting the identity.
    packet.id = g_local_id;
    packet.origin_id = slot.origin_id;
    packet.sequence = slot.sequence;
    packet.kind = slot.kind;
    packet.build_flags = slot.build_flags;
    memcpy(packet.guid, slot.guid, sizeof(packet.guid));
    memcpy(packet.class_id, slot.class_id, sizeof(packet.class_id));
    packet.tech_type = slot.tech_type;
    packet.constructed_amount = slot.constructed_amount;
    packet.x = slot.x;
    packet.y = slot.y;
    packet.z = slot.z;
    packet.qx = slot.qx;
    packet.qy = slot.qy;
    packet.qz = slot.qz;
    packet.qw = slot.qw;
    packet.vx = slot.vx;
    packet.vy = slot.vy;
    packet.vz = slot.vz;
    memcpy(packet.object_id, slot.object_id, sizeof(packet.object_id));
    packet.proto_version = slot.proto_version;
    packet.face_linked_module_type = slot.face_linked_module_type;
    memcpy(packet.face_linked_position, slot.face_linked_position,
           sizeof(packet.face_linked_position));
    memcpy(packet.face_cell, slot.face_cell, sizeof(packet.face_cell));
    packet.face_direction = slot.face_direction;
    packet.face_has_value = slot.face_has_value;
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
    packet.geometry_hash_low = slot.geometry_hash_low;
    packet.geometry_hash_high = slot.geometry_hash_high;
#endif
    send_to(&packet, sizeof(packet), addr);
    g_event_tx++;
}

// Hands one event to every peer. Callers hold g_event_lock.
void broadcast_world_event(const EventSlot& slot) {
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    double now = monotonic_seconds();
    for (size_t i = 0; i < kMaxPeers; i++) {
        if (!g_peers[i].active) continue;
        if (now - g_peers[i].last_seen > kPeerTimeout) continue;
        if (!session_peer_eligible_locked(i, true)) continue;
        if (slot.origin_id == g_peers[i].id) continue;   // it already has this one
        send_world_event(slot, g_peers[i].raw);
    }
    pthread_mutex_unlock(&g_peers_lock);
}

// Called from the game thread. Returns false only when the queue is full, which is reported
// rather than silently dropping the event: a player who picks something up and watches it stay
// on the floor deserves a log line.
bool queue_local_event(const WorldEvent& event, bool already_applied) {
    // Taken before the event lock, for the same reason service_world_events takes it there: the
    // peer lock is never held while the event lock is held.
    uint32_t live = active_peer_mask();

    pthread_mutex_lock(&g_event_lock);

    EventSlot& slot = g_events.slots[g_events.next_sequence & kEventRingMask];
    if (slot.used) {
        // The ring wrapped onto an event that never got acknowledged. Overwriting is the right
        // call: the alternative is refusing all further events until the peer comes back.
        LOG_WARN("event: retry queue full, dropping unacked %s %u from %08x",
                 event_kind_name(slot.kind), slot.sequence, slot.origin_id);
        g_events.slots[g_events.next_sequence & kEventRingMask].used = false;
    }

    memset(&slot, 0, sizeof(slot));
    slot.used = true;
    slot.local_only = already_applied;
    slot.origin_id = g_local_id;
    slot.sequence = ++g_events.next_sequence;
    slot.kind = event.kind;
    slot.build_flags = event.build_flags;
    memcpy(slot.guid, event.guid, sizeof(slot.guid));
    memcpy(slot.class_id, event.class_id, sizeof(slot.class_id));
    slot.tech_type = event.tech_type;
    slot.constructed_amount = event.constructed_amount;
    memcpy(slot.object_id, event.object_id, sizeof(slot.object_id));
    slot.proto_version = event.proto_version;
    slot.face_linked_module_type = event.face_linked_module_type;
    memcpy(slot.face_linked_position, event.face_linked_position,
           sizeof(slot.face_linked_position));
    memcpy(slot.face_cell, event.face_cell, sizeof(slot.face_cell));
    slot.face_direction = event.face_direction;
    slot.face_has_value = event.face_has_value;
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
    slot.geometry_hash_low = event.geometry_hash_low;
    slot.geometry_hash_high = event.geometry_hash_high;
#endif
    slot.x = event.x;
    slot.y = event.y;
    slot.z = event.z;
    slot.qx = event.qx;
    slot.qy = event.qy;
    slot.qz = event.qz;
    slot.qw = event.qw;
    slot.vx = event.vx;
    slot.vy = event.vy;
    slot.vz = event.vz;
    // Nothing is pre-acknowledged, and this is the whole reason the event is reliable. already_applied
    // means the local game has already made the change, not that the peers have seen it; starting
    // from active_peer_mask() marked every live peer as having the event before it was ever sent,
    // so the first service pass saw outstanding == 0 and retired the slot with no retransmit.
    slot.acked = 0;
    slot.created = monotonic_seconds();
    slot.last_send = 0.0;

    EventSlot copy = slot;
    pthread_mutex_unlock(&g_event_lock);

    broadcast_world_event(copy);
    return true;
}

// Retransmit loop, on the pump thread. Anything unacknowledged goes out again, and a slot whose
// peers have all gone is released so the ring keeps moving.
void service_world_events(double now) {
    // Taken before the event lock and never while holding it: the peer lock and the event lock
    // are never nested, because the peer lock is what serialises the pose path.
    uint32_t live = active_peer_mask();

    pthread_mutex_lock(&g_event_lock);
    for (int i = 0; i < kEventRingSize; i++) {
        EventSlot& slot = g_events.slots[i];
        if (!slot.used) continue;

        uint32_t outstanding = live & ~slot.acked;
        if (outstanding == 0) {
            slot.used = false;
            continue;
        }
        if (now - slot.created > kEventExpiry) {
            LOG_WARN("event: %s %u from %08x expired unacked", event_kind_name(slot.kind),
                     slot.sequence, slot.origin_id);
            slot.used = false;
            continue;
        }
        if (now - slot.last_send < kEventResendInterval) continue;

        EventSlot copy = slot;
        pthread_mutex_unlock(&g_event_lock);
        broadcast_world_event(copy);
        pthread_mutex_lock(&g_event_lock);

        // The slot may have been reused while the lock was dropped, so re-find it by sequence.
        for (int j = 0; j < kEventRingSize; j++) {
            if (g_events.slots[j].used && g_events.slots[j].sequence == copy.sequence &&
                g_events.slots[j].origin_id == copy.origin_id) {
                g_events.slots[j].last_send = now;
                break;
            }
        }
    }

    // A peer that dropped has to stop counting against every queued event.
    if (live != g_events.peer_bits) {
        for (int i = 0; i < kEventRingSize; i++) {
            if (g_events.slots[i].used) g_events.slots[i].acked &= live;
        }
        g_events.peer_bits = live;
    }

    pthread_mutex_unlock(&g_event_lock);
}

// ---------------------------------------------------------------------------
// Reliable world events: receive
// ---------------------------------------------------------------------------

void handle_event_packet(const uint8_t* data, size_t size, const struct sockaddr_in& addr) {
    if (size < sizeof(PacketHeader)) return;

    PacketHeader header{};
    memcpy(&header, data, sizeof(header));

    uint32_t sender_id = 0;
    if (header.type == kPacketWorldEventAck && size >= sizeof(WorldEventAckPacket)) {
        WorldEventAckPacket packet{};
        memcpy(&packet, data, sizeof(packet));
        sender_id = packet.id;
    } else if (header.type == kPacketWorldEvent && size >= sizeof(WorldEventPacket)) {
        WorldEventPacket packet{};
        memcpy(&packet, data, sizeof(packet));
        sender_id = packet.id;
    } else {
        return;
    }
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    int sender_index = peer_index_by_id(sender_id);
    bool allowed = sender_index >= 0 &&
                   session_peer_eligible_locked(static_cast<size_t>(sender_index), true);
    pthread_mutex_unlock(&g_peers_lock);
    if (!allowed) return;

    if (header.type == kPacketWorldEventAck) {
        if (size < sizeof(WorldEventAckPacket)) return;
        WorldEventAckPacket packet{};
        memcpy(&packet, data, sizeof(packet));
        if (packet.origin_id != g_local_id) return;

        // Resolved before taking the event lock: event_peer_bit takes the peer lock, and the
        // two locks must never be nested.
        uint32_t bit = event_peer_bit(packet.id);
        if (bit == 0) return;

        pthread_mutex_lock(&g_event_lock);
        for (int i = 0; i < kEventRingSize; i++) {
            EventSlot& slot = g_events.slots[i];
            // A cumulative ack covers every lower sequence, so the test is not equality.
            if (slot.used && slot.origin_id == packet.origin_id &&
                slot.sequence <= packet.sequence) {
                slot.acked |= bit;
            }
        }
        pthread_mutex_unlock(&g_event_lock);
        return;
    }

    if (size < sizeof(WorldEventPacket)) return;
    WorldEventPacket packet{};
    memcpy(&packet, data, sizeof(packet));
    if (packet.id == g_local_id) return;
    if (!event_kind_known(packet.kind)) return;

    WorldEvent event{};
    event.origin_id = packet.origin_id;
    event.sequence = packet.sequence;
    event.kind = packet.kind;
    event.build_flags = packet.build_flags;
    memcpy(event.guid, packet.guid, sizeof(event.guid));
    memcpy(event.class_id, packet.class_id, sizeof(event.class_id));
    event.tech_type = packet.tech_type;
    event.constructed_amount = packet.constructed_amount;
    memcpy(event.object_id, packet.object_id, sizeof(event.object_id));
    event.proto_version = packet.proto_version;
    event.face_linked_module_type = packet.face_linked_module_type;
    memcpy(event.face_linked_position, packet.face_linked_position,
           sizeof(event.face_linked_position));
    memcpy(event.face_cell, packet.face_cell, sizeof(event.face_cell));
    event.face_direction = packet.face_direction;
    event.face_has_value = packet.face_has_value;
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
    event.geometry_hash_low = packet.geometry_hash_low;
    event.geometry_hash_high = packet.geometry_hash_high;
#endif
    event.x = packet.x;
    event.y = packet.y;
    event.z = packet.z;
    event.qx = packet.qx;
    event.qy = packet.qy;
    event.qz = packet.qz;
    event.qw = packet.qw;
    event.vx = packet.vx;
    event.vy = packet.vy;
    event.vz = packet.vz;

    pthread_mutex_lock(&g_event_lock);

    EventOrigin* origin = nullptr;
    for (int i = 0; i < kEventOriginSlots; i++) {
        if (g_events.origins[i].used && g_events.origins[i].id == packet.origin_id) {
            origin = &g_events.origins[i];
            break;
        }
    }
    if (!origin) {
        for (int i = 0; i < kEventOriginSlots; i++) {
            if (!g_events.origins[i].used) {
                origin = &g_events.origins[i];
                memset(origin, 0, sizeof(*origin));
                origin->used = true;
                origin->id = packet.origin_id;
                break;
            }
        }
    }

    uint32_t ack_value = 0;
    bool send_ack = false;

    if (!origin) {
        // No origin slot: drop without ack so the sender keeps retransmitting while it
        // retries; when a slot frees up the event lands.
    } else if (!origin->seq_valid) {
        // First event from this origin arms the window at its own sequence. Anything the
        // sender still holds below it either predates us or was already dropped by the
        // sender's own expiry.
        if (inbox_push(event)) {
            origin->seq_valid = true;
            origin->next_sequence = event.sequence + 1;
            g_event_rx++;
            LOG_INFO("event: %s %u from %08x",
                     event_kind_name(event.kind), event.sequence,
                     event.origin_id);
            drain_pending(origin);
            ack_value = origin->next_sequence - 1;
            send_ack = true;
        }
        // else: inbox full - no ack, the sender retries after kEventResendInterval.
    } else if ((int32_t)(event.sequence - origin->next_sequence) < 0 ||
               origin_has_pending(origin, event.sequence)) {
        // Below the delivered prefix, or already buffered above the gap: a retransmit of
        // something handled. Counted, dropped quietly, ack repeats the delivered prefix.
        g_event_dup++;
        ack_value = origin->next_sequence - 1;
        send_ack = true;
    } else if (event.sequence == origin->next_sequence) {
        // In order.
        if (inbox_push(event)) {
            origin->next_sequence = event.sequence + 1;
            g_event_rx++;
            LOG_INFO("event: %s %u from %08x",
                     event_kind_name(event.kind), event.sequence,
                     event.origin_id);
            drain_pending(origin);
        }
        // The watermark moves only when the inbox accepted the event, so the ack never
        // claims a byte the game has not received.
        ack_value = origin->next_sequence - 1;
        send_ack = true;
    } else {
        // Out of order: hold in the reorder window until the gap fills. A full window
        // drops WITHOUT acking, so the sender retransmits the dropped one later instead
        // of reading the watermark as "delivered".
        if (!origin_store_pending(origin, event)) {
            static double s_last_pending_drop = 0.0;
            double now = monotonic_seconds();
            if (now - s_last_pending_drop > 5.0) {
                s_last_pending_drop = now;
                LOG_WARN("event: reorder window full, dropping %s %u (will be retransmitted)",
                         event_kind_name(event.kind), event.sequence);
            }
        }
        ack_value = origin->next_sequence - 1;
        send_ack = true;
    }

    pthread_mutex_unlock(&g_event_lock);

    if (!send_ack) return;
    WorldEventAckPacket ack{};
    ack.header.magic = kMagic;
    ack.header.type = kPacketWorldEventAck;
    ack.id = g_local_id;
    ack.origin_id = packet.origin_id;
    ack.sequence = ack_value;
    send_to(&ack, sizeof(ack), addr);
}

void send_join_reply(const JoinReplyPacket& reply, const struct sockaddr_in& addr) {
    send_to(&reply, sizeof(reply), addr);
}

// Presence intake. Called with g_peers_lock held; takes g_session.lock further down
// (lock order is always peers -> session, never the reverse). Validation is deliberately
// strict: a malformed presence is dropped silently rather than stored, because every
// field here drives role, roster and election decisions. In particular the flag
// combinations are cross-checked (a host must also be a member with a world and name
// itself; a member must carry a session id, an epoch and a content GUID), strings must
// be NUL-terminated within their fields, and versions outside v3 are tracked (for logs)
// but hidden from the session list and all sync paths.
void handle_presence_locked(const uint8_t* data, size_t size,
                            const struct sockaddr_in& addr, double now) {
    if (size != sizeof(SessionPresencePacket)) return;
    SessionPresencePacket packet{};
    memcpy(&packet, data, sizeof(packet));
    if (!header_reserved_zero(packet.header) || packet.packet_size != sizeof(packet) ||
        packet.sender_id == 0 ||
        packet.sender_id == g_local_id || packet.reserved != 0 ||
        packet.max_players != kMaxPeers + 1 || packet.player_count > packet.max_players ||
        (packet.flags & ~(kSessionFlagIsHost | kSessionFlagHasWorld | kSessionFlagMember)) != 0 ||
        !fixed_string_zero_padded(packet.slot, sizeof(packet.slot)) ||
        !fixed_string_zero_padded(packet.game_build, sizeof(packet.game_build)) ||
        !fixed_string_zero_padded(packet.mod_version, sizeof(packet.mod_version))) {
        return;
    }
    if (packet.protocol_version != kSessionProtocolVersion) {
        // Foreign protocol peers are hidden from the v3 session list and cannot enter sync.
        claim_peer(packet.sender_id, addr, now);
        int index = peer_index_by_id(packet.sender_id);
        if (index >= 0) {
            memset(&g_remote[index], 0, sizeof(g_remote[index]));
            g_remote[index].valid = true;
            g_remote[index].protocol_v3 = false;
            g_remote[index].last_presence = now;
        }
        return;
    }
    bool is_member = (packet.flags & kSessionFlagMember) != 0;
    bool is_host = (packet.flags & kSessionFlagIsHost) != 0;
    bool has_world = (packet.flags & kSessionFlagHasWorld) != 0;
    if (is_member != bytes_nonzero(packet.session_id, sizeof(packet.session_id)) ||
        (is_host && (!is_member || !has_world || packet.host_id != packet.sender_id)) ||
        (has_world && !is_member) ||
        (is_member && (!bytes_nonzero(packet.content_guid, sizeof(packet.content_guid)) ||
                       packet.session_epoch == 0)) ||
        (has_world && packet.slot[0] == '\0')) {
        return;
    }

    PeerState* peer = claim_peer(packet.sender_id, addr, now);
    if (!peer) return;
    int index = peer_index_by_id(packet.sender_id);
    if (index < 0) return;
    RemotePresence& remote = g_remote[index];
    if (remote.valid && remote.protocol_v3 &&
        static_cast<int32_t>(packet.sequence - remote.sequence) <= 0) {
        return;
    }

    bool was_join_accepted = remote.join_accepted;
    uint8_t admitted_session[16];
    memcpy(admitted_session, remote.admitted_session, sizeof(admitted_session));
    uint32_t admitted_epoch = remote.admitted_epoch;
    uint64_t old_nonce = remote.last_join_nonce;
    bool old_cache_valid = remote.cached_reply_valid;
    JoinReplyPacket old_reply = remote.cached_reply;
    memset(&remote, 0, sizeof(remote));
    remote.valid = true;
    remote.protocol_v3 = true;
    remote.sequence = packet.sequence;
    remote.epoch = packet.session_epoch;
    remote.host_id = packet.host_id;
    remote.flags = packet.flags;
    remote.player_count = packet.player_count;
    remote.max_players = packet.max_players;
    memcpy(remote.session_id, packet.session_id, sizeof(remote.session_id));
    memcpy(remote.content_guid, packet.content_guid, sizeof(remote.content_guid));
    remote.world_gen = packet.world_gen;
    memcpy(remote.slot, packet.slot, sizeof(remote.slot));
    memcpy(remote.game_build, packet.game_build, sizeof(remote.game_build));
    memcpy(remote.mod_version, packet.mod_version, sizeof(remote.mod_version));
    remote.last_presence = now;
    remote.join_accepted = was_join_accepted;
    memcpy(remote.admitted_session, admitted_session, sizeof(remote.admitted_session));
    remote.admitted_epoch = admitted_epoch;
    remote.last_join_nonce = old_nonce;
    remote.cached_reply_valid = old_cache_valid;
    remote.cached_reply = old_reply;

    bool role_changed = false;
    uint32_t changed_host = 0;
    pthread_mutex_lock(&g_session.lock);
    bool local_member = g_session.local_member;
    bool local_same = local_member &&
                      same_session(g_session.session_id, packet.session_id);
    if (local_same && is_host && packet.host_id == packet.sender_id) {
        if (packet.session_epoch > g_session.epoch) {
            g_session.epoch = packet.session_epoch;
            g_session.host_id = packet.host_id;
            g_session.last_host_presence = now;
            g_session.election_started = 0.0;
            if (g_session.local_host && packet.host_id != g_local_id) {
                g_session.local_host = false;
                g_session.presence_dirty = true;
                role_changed = true;
                changed_host = packet.host_id;
            }
            g_session.state = g_session.local_host ? kSessionHost : kSessionGuest;
            snprintf(g_session.detail, sizeof(g_session.detail), "%s",
                     g_session.local_host ? "host" : "host presence received");
        } else if (packet.session_epoch == g_session.epoch &&
                   (g_session.host_id == 0 || packet.sender_id <= g_session.host_id)) {
            bool adopts = g_session.host_id != packet.sender_id;
            bool this_wins = g_session.local_host && g_local_id < packet.sender_id;
            if (adopts && !this_wins) g_session.host_id = packet.sender_id;
            g_session.last_host_presence = now;
            g_session.election_started = 0.0;
            if (g_session.local_host && !this_wins && packet.sender_id < g_local_id) {
                g_session.local_host = false;
                g_session.host_id = packet.sender_id;
                g_session.state = kSessionGuest;
                g_session.presence_dirty = true;
                role_changed = true;
                changed_host = packet.sender_id;
            } else if (!g_session.local_host && g_session.host_id == packet.sender_id) {
                g_session.state = kSessionGuest;
            }
        }
    }
    pthread_mutex_unlock(&g_session.lock);

    // Role changes are delivered to the main thread; this packet handler never touches Unity.
    if (role_changed) net_note_session_role(false, changed_host);
}

void handle_join_request_locked(const uint8_t* data, size_t size,
                                const struct sockaddr_in& addr) {
    // Admission order matters: cheap structural checks first, then versions (a mismatch
    // stays visible in the roster but can never join), then availability, then staleness,
    // then the guest's own state. FULL/BUSY/STALE are transient - the guest backs off and
    // retries (10 s, 60 s for version mismatches). WORLD_UNAVAILABLE means this phone has
    // no servable offer right now, NOT that the guest did anything wrong.
    if (size != sizeof(JoinRequestPacket)) return;
    JoinRequestPacket request{};
    memcpy(&request, data, sizeof(request));
    if (!header_reserved_zero(request.header) || request.packet_size != sizeof(request) ||
        request.protocol_version != kSessionProtocolVersion ||
        request.sender_id == 0 || request.sender_id == g_local_id ||
        request.target_host_id != g_local_id || request.request_nonce == 0) {
        return;
    }
    PeerState* peer = claim_peer(request.sender_id, addr, monotonic_seconds());
    if (!peer) return;
    int index = peer_index_by_id(request.sender_id);
    if (index < 0) return;
    RemotePresence& remote = g_remote[index];
    if (remote.cached_reply_valid && remote.last_join_nonce == request.request_nonce) {
        send_join_reply(remote.cached_reply, addr);
        return;
    }

    SessionJoinStatus status = kJoinAccepted;
    uint8_t current_session[16]{};
    uint32_t current_epoch = 0;
    uint8_t current_guid[16]{};
    char current_slot[24]{};
    bool host_available = false;
    pthread_mutex_lock(&g_session.lock);
    host_available = g_session.active && g_session.local_host && g_session.local_has_world &&
                     g_session.local_member;
    current_epoch = g_session.epoch;
    memcpy(current_session, g_session.session_id, sizeof(current_session));
    memcpy(current_guid, g_session.content_guid, sizeof(current_guid));
    snprintf(current_slot, sizeof(current_slot), "%s", g_session.slot);
    pthread_mutex_unlock(&g_session.lock);
    // The published offer is a cache, not the authority: a role flicker between save ticks
    // clears it, and the claim path rebuilds it from the saves directory on demand. What
    // actually decides servability is whether the slot files can be read, so a missing
    // snapshot falls through to a disk probe instead of refusing the guest outright.
    pthread_mutex_lock(&g_offer_lock);
    bool offer_available = g_offer_snapshot && g_offer_snapshot->valid;
    pthread_mutex_unlock(&g_offer_lock);
    if (!offer_available && host_available)
        offer_available = world_disk_ready(current_slot);

    if (!remote.valid || !remote.protocol_v3) {
        status = kJoinProtocolMismatch;
    } else if (strcmp(remote.game_build, kGameBuild) != 0) {
        status = kJoinGameMismatch;
    } else if (strcmp(remote.mod_version, kModVersion) != 0) {
        status = kJoinModMismatch;
    } else if (!host_available || !offer_available) {
        status = kJoinWorldUnavailable;
    } else if (!same_session(request.session_id, current_session) ||
               request.session_epoch != current_epoch) {
        status = kJoinStaleSession;
    } else if (remote.flags & kSessionFlagHasWorld) {
        status = kJoinGuestInGame;
    } else {
        size_t members = 1;
        for (size_t i = 0; i < kMaxPeers; ++i) {
            if (g_peers[i].active && join_grant_matches(g_remote[i], current_session, current_epoch)) {
                ++members;
            }
        }
        if (!join_grant_matches(remote, current_session, current_epoch)) ++members;
        if (members > kMaxPeers + 1) status = kJoinFull;
    }

    WorldGuest* reserved_guest = nullptr;
    if (status == kJoinAccepted) {
        reserved_guest = claim_guest(request.sender_id, addr);
        if (!reserved_guest) status = kJoinBusy;
    }

    JoinReplyPacket reply{};
    reply.header.magic = kMagic;
    reply.header.type = kPacketJoinReply;
    reply.protocol_version = kSessionProtocolVersion;
    reply.packet_size = sizeof(reply);
    reply.sender_id = g_local_id;
    reply.recipient_id = request.sender_id;
    reply.session_epoch = current_epoch;
    // Echo the target session so a stale-session refusal still correlates with the
    // client's selected host/session and can be surfaced instead of being discarded.
    memcpy(reply.session_id, request.session_id, sizeof(reply.session_id));
    reply.request_nonce = request.request_nonce;
    reply.status = static_cast<uint8_t>(status);
    reply.reason = static_cast<uint8_t>(status);
    remote.last_join_nonce = request.request_nonce;
    remote.cached_reply = reply;
    remote.cached_reply_valid = true;
    if (status == kJoinAccepted) {
        remote.join_accepted = true;
        memcpy(remote.admitted_session, current_session, sizeof(remote.admitted_session));
        remote.admitted_epoch = current_epoch;
        remote.flags |= kSessionFlagMember;
        remote.flags &= ~kSessionFlagHasWorld;
        remote.host_id = g_local_id;
        remote.epoch = current_epoch;
        memcpy(remote.session_id, current_session, sizeof(remote.session_id));
        memcpy(remote.content_guid, current_guid, sizeof(remote.content_guid));
        snprintf(remote.slot, sizeof(remote.slot), "%s", current_slot);
        pthread_mutex_lock(&g_session.lock);
        g_session.presence_dirty = true;
        pthread_mutex_unlock(&g_session.lock);
        LOG_INFO("session: accepted JOIN from %08x nonce=%llu",
                 request.sender_id, static_cast<unsigned long long>(request.request_nonce));
    } else {
        if (reserved_guest) release_guest(reserved_guest);
        LOG_WARN("session: JOIN from %08x refused status=%u (host_available=%d "
                 "offer_available=%d epoch=%u)",
                 request.sender_id, static_cast<unsigned>(status), host_available ? 1 : 0,
                 offer_available ? 1 : 0, current_epoch);
    }
    send_join_reply(reply, addr);
}

void handle_join_reply_locked(const uint8_t* data, size_t size,
                              const struct sockaddr_in& addr) {
    if (size != sizeof(JoinReplyPacket)) return;
    JoinReplyPacket reply{};
    memcpy(&reply, data, sizeof(reply));
    if (!header_reserved_zero(reply.header) || reply.packet_size != sizeof(reply) ||
        reply.protocol_version != kSessionProtocolVersion ||
        reply.recipient_id != g_local_id || reply.status > kJoinWorldUnavailable ||
        reply.reason > kJoinWorldUnavailable || reply.status != reply.reason ||
        reply.reserved[0] || reply.reserved[1] ||
        (reply.status == kJoinAccepted &&
         (reply.session_epoch == 0 || !bytes_nonzero(reply.session_id, sizeof(reply.session_id))))) {
        return;
    }
    int peer_index = peer_index_by_id(reply.sender_id);
    if (peer_index < 0 ||
        g_peers[peer_index].raw.sin_addr.s_addr != addr.sin_addr.s_addr ||
        g_peers[peer_index].raw.sin_port != addr.sin_port) {
        return;
    }
    pthread_mutex_lock(&g_session.lock);
    bool matches = g_session.selected && g_session.join_request_pending &&
                   g_session.selected_host == reply.sender_id &&
                   g_session.join_nonce == reply.request_nonce &&
                   same_session(g_session.selected_session, reply.session_id);
    if (!matches) {
        pthread_mutex_unlock(&g_session.lock);
        return;
    }
    g_session.join_request_pending = false;
    if (reply.status == kJoinAccepted) {
        // Preserved before the overwrite below: a resume-host-slot guest quick-launched
        // into this very world carries its identity here, and the world it may have
        // already received/verified armed host_world_gen_seen ahead of the JOIN.
        uint8_t prior_guid[16];
        memcpy(prior_guid, g_session.content_guid, sizeof(prior_guid));
        bool prior_guid_valid = g_session.content_guid_valid;
        uint32_t prior_gen_seen = g_session.host_world_gen_seen;
        g_session.active = true;
        g_session.local_member = true;
        g_session.local_host = false;
        g_session.local_has_world = false;
        g_session.epoch = reply.session_epoch;
        g_session.host_id = reply.sender_id;
        memcpy(g_session.session_id, reply.session_id, sizeof(g_session.session_id));
        memcpy(g_session.content_guid, g_session.selected_guid,
               sizeof(g_session.content_guid));
        g_session.content_guid_valid = bytes_nonzero(g_session.selected_guid, 16);
        snprintf(g_session.slot, sizeof(g_session.slot), "%s", g_session.selected_slot);
        g_session.join_accepted = true;
        g_session.last_host_presence = monotonic_seconds();
        g_session.world_request_last = 0.0;
        g_session.host_world_gen_seen = 0;
        g_session.world_refresh_pending = false;
        // The quick-launched resume case: the guest's live world IS this session's world
        // (same content GUID the host advertised). Without this adoption the guest keeps
        // local_has_world=false - the save report that normally raises it already ran
        // before the JOIN - and every pose/event exchange stays blocked while the player
        // is in fact sitting in the world.
        if (g_session.resume_host_slot && g_session.world_active && prior_guid_valid &&
            same_content(prior_guid, g_session.selected_guid)) {
            g_session.local_has_world = true;
            g_session.resume_host_slot = false;
            g_session.host_world_gen_seen = prior_gen_seen;
            g_session.presence_dirty = true;
            session_set_state_locked(kSessionGuest, kJoinAccepted,
                                     "resumed host-received world adopted into session");
            LOG_INFO("session: adopted the live host-received world "
                     "(gen seen %u); pose/event sync enabled",
                     prior_gen_seen);
        } else {
            session_set_state_locked(kSessionTransferring, kJoinAccepted,
                                     "accepted; requesting world");
        }
    } else {
        g_session.reason = static_cast<SessionJoinStatus>(reply.reason);
        char message[64];
        snprintf(message, sizeof(message), "join refused (reason %u)", reply.reason);
        session_set_state_locked(kSessionFailed, g_session.reason, message);
        g_session.selected = false;
        // Back off before any reselect: otherwise the next pump iteration picks the
        // same host again and the pair spams request/refuse at pump rate. A host that
        // refused for a transient reason (busy, world not yet offered) becomes joinable
        // again after the cooldown; version mismatches wait longer.
        double cooldown = (reply.reason == kJoinProtocolMismatch ||
                           reply.reason == kJoinGameMismatch || reply.reason == kJoinModMismatch)
                              ? 60.0
                              : 10.0;
        g_session.join_retry_after = monotonic_seconds() + cooldown;
    }
    bool role_changed = (reply.status == kJoinAccepted);
    uint32_t role_host = reply.sender_id;
    pthread_mutex_unlock(&g_session.lock);
    if (role_changed) net_note_session_role(false, role_host);
}

void handle_session_leave_locked(const uint8_t* data, size_t size,
                                 const struct sockaddr_in& addr) {
    if (size != sizeof(SessionLeavePacket)) return;
    SessionLeavePacket packet{};
    memcpy(&packet, data, sizeof(packet));
    if (!header_reserved_zero(packet.header) || packet.packet_size != sizeof(packet) ||
        packet.protocol_version != kSessionProtocolVersion ||
        packet.sender_id == g_local_id || packet.host_id != g_local_id) {
        return;
    }
    int index = peer_index_by_id(packet.sender_id);
    if (index < 0 || !g_remote[index].join_accepted) return;
    if (g_peers[index].raw.sin_addr.s_addr != addr.sin_addr.s_addr ||
        g_peers[index].raw.sin_port != addr.sin_port) {
        return;
    }
    bool matches = false;
    pthread_mutex_lock(&g_session.lock);
    matches = g_session.local_host && packet.session_epoch == g_session.epoch &&
              same_session(packet.session_id, g_session.session_id);
    pthread_mutex_unlock(&g_session.lock);
    if (!matches || !same_session(packet.session_id, g_remote[index].session_id)) return;
    g_remote[index].join_accepted = false;
    g_remote[index].flags &= ~kSessionFlagMember;
    WorldGuest* guest = find_guest(packet.sender_id);
    if (guest) release_guest(guest);
    pthread_mutex_lock(&g_session.lock);
    g_session.presence_dirty = true;
    pthread_mutex_unlock(&g_session.lock);
    LOG_INFO("session: peer %08x left", packet.sender_id);
}

// The lock has to stay held while the caller uses the returned PeerState pointer, so it is
// taken here and released at the end of handle_packet.
void handle_packet(const uint8_t* data, size_t size, const struct sockaddr_in& addr,
                   double now) {
    if (size < sizeof(PacketHeader)) return;

    PacketHeader header{};
    memcpy(&header, data, sizeof(header));
    if (header.magic != kMagic) return;

    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);

    if (header.type == kPacketHello) {
        g_rx_hello++;
    } else if (header.type == kPacketState) {
        g_rx_state++;
    } else {
        g_rx_other++;
    }
    g_rx_bytes += size;

    if (header.type == kPacketHello && size >= sizeof(HelloPacket)) {
        HelloPacket packet{};
        memcpy(&packet, data, sizeof(packet));
        if (packet.id != g_local_id) claim_peer(packet.id, addr, now);
        pthread_mutex_unlock(&g_peers_lock);
        return;
    }

    if (header.type == kPacketSessionPresence) {
        handle_presence_locked(data, size, addr, now);
        pthread_mutex_unlock(&g_peers_lock);
        return;
    }
    if (header.type == kPacketJoinRequest) {
        handle_join_request_locked(data, size, addr);
        pthread_mutex_unlock(&g_peers_lock);
        return;
    }
    if (header.type == kPacketJoinReply) {
        handle_join_reply_locked(data, size, addr);
        pthread_mutex_unlock(&g_peers_lock);
        return;
    }
    if (header.type == kPacketSessionLeave) {
        handle_session_leave_locked(data, size, addr);
        pthread_mutex_unlock(&g_peers_lock);
        return;
    }

    // The world handoff runs outside the peer lock. It does its own bookkeeping and calls
    // send_to(), and holding the peer lock across a multi-packet burst would stall the 20 Hz
    // pose path for every other peer.
    if (header.type >= kPacketWorldRequest && header.type <= kPacketWorldRefuse) {
        pthread_mutex_unlock(&g_peers_lock);
        handle_world_packet(data, size, addr, now);
        pthread_mutex_lock(&g_peers_lock);
    }

    // Events take the peer lock inside event_peer_bit() and active_peer_mask(), so they have to
    // run with it released as well.
    if (header.type == kPacketWorldEvent || header.type == kPacketWorldEventAck) {
        pthread_mutex_unlock(&g_peers_lock);
        handle_event_packet(data, size, addr);
        pthread_mutex_lock(&g_peers_lock);
    }

#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
    if (header.type == kPacketGeometryFragment || header.type == kPacketGeometryAck) {
        pthread_mutex_unlock(&g_peers_lock);
        geometry_handle_packet(data, size, addr, now);
        pthread_mutex_lock(&g_peers_lock);
    }
#endif

    if (header.type == kPacketTime && size >= sizeof(TimePacket)) {
        TimePacket packet{};
        memcpy(&packet, data, sizeof(packet));
        int sender_index = -1;
        for (size_t i = 0; i < kMaxPeers; ++i) {
            if (g_peers[i].active &&
                g_peers[i].raw.sin_addr.s_addr == addr.sin_addr.s_addr &&
                g_peers[i].raw.sin_port == addr.sin_port) {
                sender_index = static_cast<int>(i);
                break;
            }
        }
        bool from_host = false;
        if (sender_index >= 0) {
            const RemotePresence& remote = g_remote[sender_index];
            pthread_mutex_lock(&g_session.lock);
            from_host = g_session.local_member && g_session.local_has_world &&
                        !g_session.local_host && remote.valid && remote.protocol_v3 &&
                        (remote.flags & (kSessionFlagIsHost | kSessionFlagHasWorld |
                                         kSessionFlagMember)) ==
                            (kSessionFlagIsHost | kSessionFlagHasWorld |
                             kSessionFlagMember) &&
                        remote.host_id == g_session.host_id &&
                        remote.epoch == g_session.epoch &&
                        now - remote.last_presence <= kPeerTimeout &&
                        same_session(remote.session_id, g_session.session_id);
            pthread_mutex_unlock(&g_session.lock);
        }
        if (!from_host || !isfinite(packet.time_passed) || !isfinite(packet.rate)) {
            pthread_mutex_unlock(&g_peers_lock);
            return;
        }
        pthread_mutex_lock(&g_time_lock);
        g_host_time = packet.time_passed;
        g_host_time_at = now;
        g_host_rate = packet.rate > 0.0 ? packet.rate : 0.0;
        g_host_time_valid = true;
        pthread_mutex_unlock(&g_time_lock);
        pthread_mutex_unlock(&g_peers_lock);
        return;
    }

    if (header.type == kPacketState && size >= sizeof(StatePacket)) {
        StatePacket packet{};
        memcpy(&packet, data, sizeof(packet));
        if (packet.id == g_local_id) {
            pthread_mutex_unlock(&g_peers_lock);
            return;
        }

        PeerState* peer = claim_peer(packet.id, addr, now);
        if (peer) {
            int peer_index = peer_index_by_id(packet.id);
            if (peer_index < 0) {
                pthread_mutex_unlock(&g_peers_lock);
                return;
            }
            PoseStreamStats& stream = g_pose_stream[peer_index];
            ++stream.rx_seen;
            stream.last_rx = now;
            const char* blocked = session_peer_block_reason_locked(peer_index, true);
            if (blocked) {
                ++stream.rx_blocked;
                stream.last_rx_block = blocked;
                pthread_mutex_unlock(&g_peers_lock);
                return;
            }
            peer->last_seen = now;
            // Reorder guard: at 20 Hz over hotspot Wi-Fi a delayed duplicate is
            // routine; applying it would roll pose AND flags backwards (the
            // piloting bit a newer packet set gets cleared -> the vehicle copy
            // is destroyed and respawned every few frames). Wrap-aware compare;
            // a sender restart rejoins through the 5 s peer timeout with a
            // zeroed slot.
            bool fresh = !peer->seq_valid ||
                         static_cast<int32_t>(packet.sequence - peer->sequence) > 0;
            if (!fresh) {
                ++stream.rx_stale;
                pthread_mutex_unlock(&g_peers_lock);
                return;
            }
            ++stream.rx_applied;
            stream.last_applied = now;
            peer->sequence = packet.sequence;
            peer->seq_valid = true;
            peer->x = packet.x;
            peer->y = packet.y;
            peer->z = packet.z;
            peer->yaw = packet.yaw;
            peer->pitch = packet.pitch;
            peer->view_yaw = packet.view_yaw;
            peer->flags = packet.flags;
            int float_count = packet.anim_float_count;
            if (float_count > kNetAnimFloats) float_count = kNetAnimFloats;
            peer->anim_float_count = float_count;
            for (int i = 0; i < float_count; i++) {
                peer->anim_floats[i] = packet.anim_floats[i];
            }
            peer->anim_underwater = packet.anim_underwater;
            peer->anim_cinematics = packet.anim_cinematics;
            peer->held_tech = packet.held_tech;
            peer->suit_tech = packet.suit_tech;
            peer->fins_tech = packet.fins_tech;
            peer->head_tech = packet.head_tech;
            memcpy(peer->environment_id,packet.environment_id,sizeof(peer->environment_id));
            peer->held_rotation={0,0,0,1};
            peer->held_rotation_valid=packet.held_tech && packet.held_rotation_valid==1 &&
                held_rotation_normalize(packet.held_rotation,&peer->held_rotation);
            peer->flags&=~kFlagHeldWorldPose;
            peer->held_offset={};
            if (packet.held_tech==751 && peer->held_rotation_valid &&
                (packet.flags&kFlagHeldWorldPose) && held_offset_valid(packet.held_offset)) {
                peer->flags|=kFlagHeldWorldPose;
                peer->held_offset=packet.held_offset;
            }
        }
    }

    if (header.type == kPacketVehicle && size >= sizeof(VehiclePacket)) {
        VehiclePacket packet{};
        memcpy(&packet, data, sizeof(packet));
        if (packet.id != g_local_id) {
            int peer_index = peer_index_by_id(packet.id);
            if (peer_index < 0 ||
                !session_peer_eligible_locked(static_cast<size_t>(peer_index), true)) {
                pthread_mutex_unlock(&g_peers_lock);
                return;
            }
            int slot = -1;
            int oldest = -1;
            for (size_t i = 0; i < kMaxVehicles; i++) {
                if (g_vehicles[i].active && g_vehicles[i].pilot == packet.id &&
                    memcmp(g_vehicles[i].guid, packet.guid, 16) == 0) {
                    slot = static_cast<int>(i);
                    break;
                }
                if (!g_vehicles[i].active) {
                    if (slot < 0) slot = static_cast<int>(i);
                } else if (oldest < 0 || g_vehicles[i].updated < g_vehicles[oldest].updated) {
                    oldest = static_cast<int>(i);
                }
            }
            if (slot < 0) slot = oldest;
            if (slot >= 0) {
                // New pilot/guid takes the slot over and re-arms the guard;
                // otherwise a stale pose must not teleport the copy backwards.
                bool is_new = !g_vehicles[slot].active ||
                              g_vehicles[slot].pilot != packet.id ||
                              memcmp(g_vehicles[slot].guid, packet.guid, 16) != 0;
                bool fresh =
                    is_new || !g_vehicles[slot].seq_valid ||
                    static_cast<int32_t>(packet.sequence - g_vehicles[slot].sequence) > 0;
                if (fresh) {
                    g_vehicles[slot].pilot = packet.id;
                    memcpy(g_vehicles[slot].guid, packet.guid, sizeof(g_vehicles[slot].guid));
                    memcpy(g_vehicles[slot].classId, packet.classId,
                           sizeof(g_vehicles[slot].classId));
                    g_vehicles[slot].x = packet.x;
                    g_vehicles[slot].y = packet.y;
                    g_vehicles[slot].z = packet.z;
                    g_vehicles[slot].yaw = packet.yaw;
                    g_vehicles[slot].pitch = packet.pitch;
                    g_vehicles[slot].roll = packet.roll;
                    g_vehicles[slot].sequence = packet.sequence;
                    g_vehicles[slot].seq_valid = true;
                    g_vehicles[slot].updated = now;
                    g_vehicles[slot].active = true;
                }
            }
        }
    }

    pthread_mutex_unlock(&g_peers_lock);
}

void drop_stale_peers(double now) {
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    for (size_t i = 0; i < kMaxPeers; i++) {
        if (!g_peers[i].active) continue;
        if (now - g_peers[i].last_seen <= kPeerTimeout) continue;
        LOG_INFO("peer %08x (%s:%u) timed out", g_peers[i].id, g_peers[i].address,
                 g_peers[i].port);
        memset(&g_remote[i], 0, sizeof(g_remote[i]));
        WorldGuest* guest = find_guest(g_peers[i].id);
        if (guest) release_guest(guest);
        memset(&g_peers[i], 0, sizeof(g_peers[i]));
    }
    pthread_mutex_unlock(&g_peers_lock);
}

// No UI exists yet, so a worldless, unjoined phone selects the first compatible
// visible host by itself - the exact behavior the menu phone has today (it pulls
// whoever serves). An explicit future UI overrides via net_session_join/leave; a
// manual leave suppresses auto-join for 30 s so it does not instantly rejoin.
void session_auto_join_tick(double now) {
    pthread_mutex_lock(&g_session.lock);
    bool resume_host_slot = g_session.resume_host_slot;
    char resume_slot[sizeof(g_session.slot)]{};
    uint8_t resume_guid[sizeof(g_session.content_guid)]{};
    if (resume_host_slot) {
        snprintf(resume_slot, sizeof(resume_slot), "%s", g_session.slot);
        memcpy(resume_guid, g_session.content_guid, sizeof(resume_guid));
    }
    // A resume-host-slot guest (loaded the host's received save without JOIN state) DOES
    // auto-join with its world active: the remote filter below pins it to the exact
    // original host (slot + content GUID), the slot check on the host side skips the
    // transfer when the bytes match, and a newer host generation flows through the
    // refresh path. Blocking the join while the world was live left the phone in a
    // sessionless forever-state with no pose/event sync - the live bug of 2026-10-25.
    bool eligible = g_session.launch_role!=kLaunchHost && !g_session.explicit_leave &&
                    !g_session.active && !g_session.local_member && !g_session.selected &&
                    !g_session.join_request_pending && !g_session.join_accepted &&
                    !g_session.local_has_world && !g_session.local_host &&
                    (!g_session.world_active || resume_host_slot) &&
                    now >= g_session.leave_cooldown_until &&
                    now >= g_session.join_retry_after;
    pthread_mutex_unlock(&g_session.lock);
    if (!eligible) return;

    uint32_t best_host = 0;
    uint32_t best_epoch = 0;
    uint8_t best_session[16]{};
    uint8_t best_guid[16]{};
    char best_slot[24]{};
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    for (size_t i = 0; i < kMaxPeers; ++i) {
        const RemotePresence& remote = g_remote[i];
        if (!g_peers[i].active || !remote.valid || !remote.protocol_v3 ||
            now - remote.last_presence > kPeerTimeout ||
            !(remote.flags & kSessionFlagIsHost) ||
            !bytes_nonzero(remote.session_id, sizeof(remote.session_id)) ||
            !bytes_nonzero(remote.content_guid, sizeof(remote.content_guid)) ||
            strcmp(remote.game_build, kGameBuild) != 0 ||
            strcmp(remote.mod_version, kModVersion) != 0) {
            continue;
        }
        // A host-received slot may survive an app restart while the in-memory JOIN does not.
        // Rejoin only its original world/session slot; never adopt an unrelated nearby host.
        if (resume_host_slot &&
            (strcmp(remote.slot, resume_slot) != 0 ||
             !same_content(remote.content_guid, resume_guid))) {
            continue;
        }
        if (best_host == 0 || g_peers[i].id < best_host) {
            best_host = g_peers[i].id;
            best_epoch = remote.epoch;
            memcpy(best_session, remote.session_id, sizeof(best_session));
            memcpy(best_guid, remote.content_guid, sizeof(best_guid));
            snprintf(best_slot, sizeof(best_slot), "%s", remote.slot);
        }
    }
    pthread_mutex_unlock(&g_peers_lock);
    if (best_host == 0) return;

    uint64_t nonce = 0;
    random_bytes(reinterpret_cast<uint8_t*>(&nonce), sizeof(nonce));
    if (nonce == 0) {
        nonce = (static_cast<uint64_t>(now * 1000000.0) << 16) | g_local_id;
    }
    pthread_mutex_lock(&g_session.lock);
    // Re-check under the lock: an explicit join may have landed meanwhile.
    if (!g_session.active && !g_session.local_member && !g_session.selected &&
        !g_session.join_request_pending && !g_session.join_accepted &&
        g_session.launch_role!=kLaunchHost && !g_session.explicit_leave &&
        !g_session.local_has_world && !g_session.local_host &&
        (!g_session.world_active || g_session.resume_host_slot) &&
        now >= g_session.leave_cooldown_until && now >= g_session.join_retry_after) {
        g_session.selected = true;
        g_session.selected_host = best_host;
        g_session.selected_epoch = best_epoch;
        memcpy(g_session.selected_session, best_session, sizeof(g_session.selected_session));
        memcpy(g_session.selected_guid, best_guid, sizeof(g_session.selected_guid));
        snprintf(g_session.selected_slot, sizeof(g_session.selected_slot), "%s", best_slot);
        g_session.join_nonce = nonce;
        g_session.join_request_pending = true;
        g_session.join_accepted = false;
        g_session.last_join_send = 0.0;
        g_session.join_started = now;
        g_session.reason = kJoinAccepted;
        session_set_state_locked(kSessionJoinPending, kJoinAccepted, "auto-join visible host");
        LOG_INFO("session: auto-joining host %08x (no UI yet; explicit join overrides)",
                 best_host);
    }
    pthread_mutex_unlock(&g_session.lock);
}

void process_session_tick(double now) {
    bool send_join = false;
    bool send_world_request = false;
    bool do_leave = false;
    uint32_t target_host = 0;
    uint32_t target_epoch = 0;
    uint8_t target_session[16]{};
    uint64_t nonce = 0;
    bool local_has_world = false;
    bool local_member = false;
    bool local_host = false;
    uint32_t current_host = 0;
    uint8_t current_session[16]{};

    pthread_mutex_lock(&g_session.lock);
    do_leave = g_session.explicit_leave;
    if (do_leave) {
        g_session.explicit_leave = false;
        target_host = g_session.host_id;
        target_epoch = g_session.epoch;
        memcpy(target_session, g_session.session_id, sizeof(target_session));
        g_session.active = false;
        g_session.local_member = false;
        g_session.local_host = false;
        g_session.join_accepted = false;
        g_session.selected = false;
        g_session.join_request_pending = false;
        memset(g_session.session_id, 0, sizeof(g_session.session_id));
        g_session.host_id = 0;
        g_session.presence_dirty = true;
        const bool role_reset=g_session.role_reset_pending;
        g_session.role_reset_pending=false;
        g_session.leave_cooldown_until = role_reset ? 0.0 : now + 30.0;
        if (role_reset) {
            g_session.local_has_world=false;g_session.world_active=false;
            g_session.resume_host_slot=false;g_session.content_guid_valid=false;
            g_session.slot[0]=0;g_session.epoch=0;g_session.election_started=0;
            g_session.world_request_last=0;g_session.join_retry_after=0;
        }
        g_session.host_world_gen_seen = 0;
        g_session.world_refresh_pending = false;
        session_set_state_locked(kSessionDiscovering, kJoinAccepted, "left session");
    }
    if (!do_leave && g_session.join_request_pending &&
        now - g_session.last_join_send >= kJoinRetrySeconds) {
        send_join = true;
        target_host = g_session.selected_host;
        target_epoch = g_session.selected_epoch;
        nonce = g_session.join_nonce;
        memcpy(target_session, g_session.selected_session, sizeof(target_session));
        g_session.last_join_send = now;
    }
    local_has_world = g_session.local_has_world;
    local_member = g_session.local_member;
    local_host = g_session.local_host;
    current_host = g_session.host_id;
    memcpy(current_session, g_session.session_id, sizeof(current_session));

    // World-refresh detect. While the guest is IN a world, the host's presence
    // carrying a higher offer generation (host_offer_refresh_tick republished after
    // an autosave, migrated host, or any other re-offer) means a newer world exists.
    // Guid moved from the detector to telemetry: the world identity may or may not
    // cycle per autosave, the monotonic generation counter never degrades. Never
    // touch the local world here - the flow only records the fact and the guest's
    // next natural world exit pulls a fresh copy (the handoff reset below + the
    // request loop).
    if (!do_leave && g_session.active && local_member && !local_host &&
        local_has_world && g_session.join_accepted && current_host) {
        pthread_once(&g_peers_lock_once, init_peers_lock);
        pthread_mutex_lock(&g_peers_lock);
        int host_index = peer_index_by_id(current_host);
        uint32_t host_world_gen = 0;
        if (host_index >= 0 && g_remote[host_index].valid &&
            g_remote[host_index].protocol_v3 &&
            now - g_remote[host_index].last_presence <= kPeerTimeout) {
            host_world_gen = g_remote[host_index].world_gen;
        }
        pthread_mutex_unlock(&g_peers_lock);
        if (host_world_gen != 0) {
            // g_session.lock has been held since the start of process_session_tick().
            // Re-locking this non-recursive mutex deadlocks the pump on a newer host world,
            // exactly when the guest needs to set world_refresh_pending.
            if (host_world_gen > g_session.host_world_gen_seen &&
                !g_session.world_refresh_pending) {
                g_session.world_refresh_pending = true;
                session_set_state_locked(kSessionGuest, kJoinAccepted,
                                         "host published a newer world; exit to menu to "
                                         "receive it");
                LOG_WARN("world: host %08x published a newer world (gen %u > %u); exit to "
                         "the menu and re-enter to receive it",
                         current_host, host_world_gen, g_session.host_world_gen_seen);
            }
        }
    }

    // A guest that left its world (slot loaded, then exited) with a finished or
    // failed handoff marks those as consumed, so the request loop below resumes
    // instead of sitting in the SlotReady/Failed state forever. Any pending refresh
    // transfers right now, since this is worldless and the fresh manifest replaces
    // the stale local copy (allowed: the slot carries the from_host marker).
    if (!do_leave && g_session.active && local_member && !local_host &&
        !local_has_world && g_session.join_accepted && g_session.world_refresh_pending) {
        WorldHandoffState handoff;
        pthread_mutex_lock(&g_handoff_lock);
        WorldHandoffState prior = g_handoff_state;
        pthread_mutex_unlock(&g_handoff_lock);
        if (prior == kWorldHandoffWritten || prior == kWorldHandoffFailed ||
            prior == kWorldHandoffIdle) {
            g_session.world_refresh_pending = false;
            set_handoff(kWorldHandoffIdle, "", "world exited; fetching refreshed world");
            LOG_INFO("world: fetching the host's newer world after the menu exit");
        }
    }

    if (!do_leave && g_session.active && local_member && !local_host &&
        !local_has_world && g_session.join_accepted) {
        WorldHandoffState handoff;
        pthread_mutex_lock(&g_handoff_lock);
        handoff = g_handoff_state;
        pthread_mutex_unlock(&g_handoff_lock);
        if (handoff == kWorldHandoffWritten) {
            session_set_state_locked(kSessionSlotReady, kJoinAccepted,
                                     "slot ready; load it manually from the menu");
        } else if (handoff == kWorldHandoffFailed) {
            session_set_state_locked(kSessionFailed, g_session.reason,
                                     "world transfer failed; retry remains available");
        } else if (handoff == kWorldHandoffReceiving ||
                   handoff == kWorldHandoffVerified) {
            session_set_state_locked(kSessionTransferring, kJoinAccepted, "receiving world");
        } else if (now - g_session.world_request_last >=
                   (handoff == kWorldHandoffFailed ? kWorldRequestRetryInterval
                                                   : kWorldRequestInterval)) {
            send_world_request = true;
            g_session.world_request_last = now;
        }
    }
    pthread_mutex_unlock(&g_session.lock);

    if (!local_host) {
        for (size_t i = 0; i < kWorldMaxGuests; ++i) {
            if (g_guests[i].active) release_guest(&g_guests[i]);
        }
    }

    if (do_leave) {
        if (target_host && target_host != g_local_id) {
            SessionLeavePacket leave{};
            leave.header.magic = kMagic;
            leave.header.type = kPacketSessionLeave;
            leave.protocol_version = kSessionProtocolVersion;
            leave.packet_size = sizeof(leave);
            leave.sender_id = g_local_id;
            leave.host_id = target_host;
            leave.session_epoch = target_epoch;
            memcpy(leave.session_id, target_session, sizeof(leave.session_id));
            pthread_once(&g_peers_lock_once, init_peers_lock);
            pthread_mutex_lock(&g_peers_lock);
            PeerState* peer = find_peer_by_id(target_host);
            if (peer) send_to(&leave, sizeof(leave), peer->raw);
            pthread_mutex_unlock(&g_peers_lock);
        }
        world_reset_receive();
        net_world_autoload_cancel();
        set_handoff(kWorldHandoffIdle, "", "left session");
        return;
    }

    if (send_join) {
        JoinRequestPacket request{};
        request.header.magic = kMagic;
        request.header.type = kPacketJoinRequest;
        request.protocol_version = kSessionProtocolVersion;
        request.packet_size = sizeof(request);
        request.sender_id = g_local_id;
        request.target_host_id = target_host;
        request.session_epoch = target_epoch;
        memcpy(request.session_id, target_session, sizeof(request.session_id));
        request.request_nonce = nonce;
        pthread_once(&g_peers_lock_once, init_peers_lock);
        pthread_mutex_lock(&g_peers_lock);
        PeerState* peer = find_peer_by_id(target_host);
        if (peer) send_to(&request, sizeof(request), peer->raw);
        pthread_mutex_unlock(&g_peers_lock);
    }

    if (send_world_request) {
        WorldRequestPacket request{};
        request.header.magic = kMagic;
        request.header.type = kPacketWorldRequest;
        request.id = g_local_id;
        pthread_once(&g_peers_lock_once, init_peers_lock);
        pthread_mutex_lock(&g_peers_lock);
        PeerState* peer = find_peer_by_id(current_host);
        if (peer) send_to(&request, sizeof(request), peer->raw);
        pthread_mutex_unlock(&g_peers_lock);
    }

    session_auto_join_tick(now);

    // Host migration, guests only. A host never elects (it IS the authority); a phone
    // outside any session has no host to lose. The sequence below is: notice the
    // timeout -> wait 2 s so every member converges on the same roster -> pick the
    // lowest id among members advertising the SAME world (content GUID must match,
    // otherwise the "successor" would serve a different world). Winner bumps the epoch
    // and keeps the session id: same session continuing, not a new one. Nobody
    // qualifies -> a worldless member drops the dead session (rediscovery), a loaded
    // one waits with its world (it may still become host by loading).
    if (!local_member || local_host) return;

    pthread_mutex_lock(&g_session.lock);
    bool host_timed_out = current_host == 0 ||
                          now - g_session.last_host_presence > kPeerTimeout;
    if (!host_timed_out) {
        g_session.election_started = 0.0;
        pthread_mutex_unlock(&g_session.lock);
        return;
    }
    if (g_session.election_started == 0.0) {
        g_session.election_started = now;
        session_set_state_locked(kSessionMigrating, kJoinAccepted,
                                 "host timeout; waiting for roster convergence");
        pthread_mutex_unlock(&g_session.lock);
        return;
    }
    if (now - g_session.election_started < kElectionConvergenceSeconds) {
        pthread_mutex_unlock(&g_session.lock);
        return;
    }
    pthread_mutex_unlock(&g_session.lock);

    uint32_t best_id = 0;
    uint32_t max_epoch = 0;
    pthread_mutex_lock(&g_session.lock);
    max_epoch = g_session.epoch;
    bool local_candidate = g_session.launch_role!=kLaunchGuest &&
                          g_session.local_has_world && g_session.local_member &&
                          g_session.content_guid_valid;
    pthread_mutex_unlock(&g_session.lock);
    if (local_candidate) best_id = g_local_id;
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    for (size_t i = 0; i < kMaxPeers; ++i) {
        const RemotePresence& remote = g_remote[i];
        if (!g_peers[i].active || !remote.valid || !remote.protocol_v3 ||
            now - remote.last_presence > kPeerTimeout ||
            !(remote.flags & kSessionFlagMember) ||
            !(remote.flags & kSessionFlagHasWorld) ||
            !same_session(remote.session_id, current_session)) {
            continue;
        }
        pthread_mutex_lock(&g_session.lock);
        bool guid_matches = g_session.content_guid_valid &&
                            same_content(remote.content_guid, g_session.content_guid);
        pthread_mutex_unlock(&g_session.lock);
        if (!guid_matches) continue;
        if (remote.epoch > max_epoch) max_epoch = remote.epoch;
        if (best_id == 0 || g_peers[i].id < best_id) best_id = g_peers[i].id;
    }
    pthread_mutex_unlock(&g_peers_lock);

    if (best_id == 0) {
        pthread_mutex_lock(&g_session.lock);
        if (!g_session.local_has_world) {
            // Dead session with nothing to offer: our host is gone and no member can
            // succeed it. A worldless member waiting here would never rejoin - drop back
            // to discovery so auto-join can pick a live session instead of lingering on
            // a host id that will never answer again.
            g_session.active = false;
            g_session.local_member = false;
            g_session.join_accepted = false;
            g_session.selected = false;
            g_session.join_request_pending = false;
            memset(g_session.session_id, 0, sizeof(g_session.session_id));
            g_session.host_id = 0;
            g_session.presence_dirty = true;
            g_session.election_started = 0.0;
            session_set_state_locked(kSessionDiscovering, kJoinAccepted,
                                     "session died with no successor; rediscovering");
            LOG_INFO("session: abandoned dead session, rediscovering");
        } else {
            session_set_state_locked(kSessionWaitingForWorld, kJoinAccepted,
                                     "no session member currently has the world loaded");
        }
        pthread_mutex_unlock(&g_session.lock);
        return;
    }
    if (best_id == g_local_id) {
        pthread_mutex_lock(&g_session.lock);
        if (g_session.active && g_session.local_member && g_session.local_has_world &&
            same_session(g_session.session_id, current_session)) {
            g_session.epoch = max_epoch + 1;
            if (g_session.epoch == 0) g_session.epoch = 1;
            g_session.host_id = g_local_id;
            bool changed = !g_session.local_host;
            g_session.local_host = true;
            g_session.last_host_presence = now;
            g_session.election_started = 0.0;
            session_set_state_locked(kSessionHost, kJoinAccepted, "elected host");
            g_session.presence_dirty = true;
            pthread_mutex_unlock(&g_session.lock);
            LOG_INFO("session: elected local host id=%08x epoch=%u", g_local_id, max_epoch + 1);
            if (changed) net_note_session_role(true, g_local_id);
            return;
        }
        pthread_mutex_unlock(&g_session.lock);
    }
}

void drop_stale_vehicles(double now) {
    pthread_mutex_lock(&g_peers_lock);
    for (size_t i = 0; i < kMaxVehicles; i++) {
        if (!g_vehicles[i].active) continue;
        if (now - g_vehicles[i].updated <= kVehicleTimeout) continue;
        memset(&g_vehicles[i], 0, sizeof(g_vehicles[i]));
    }
    pthread_mutex_unlock(&g_peers_lock);
}

void* pump_main(void* unused) {
    (void)unused;
    // Was 512, which silently truncated every datagram longer than that. A world chunk plus
    // its header is 1041 bytes, so the buffer has to clear that with room to spare.
    uint8_t buffer[2048];
    uint64_t iterations = 0;
    double next_report = monotonic_seconds() + 5.0;
    double next_pose_report = monotonic_seconds() + 15.0;

    while (g_pump_running) {
        g_pump_beat_ms = MonotonicMs();
        double now = monotonic_seconds();
        iterations++;

        for (;;) {
            struct sockaddr_in addr{};
            socklen_t addr_size = sizeof(addr);
            ssize_t received = recvfrom(g_socket, buffer, sizeof(buffer), 0,
                                        reinterpret_cast<struct sockaddr*>(&addr), &addr_size);
            if (received <= 0) break;
            handle_packet(buffer, static_cast<size_t>(received), addr, now);
        }

        if (now >= g_next_hello) {
            send_hello();
            send_session_presence();
            g_next_hello = now + kHelloInterval;
        } else {
            pthread_mutex_lock(&g_session.lock);
            bool dirty = g_session.presence_dirty;
            pthread_mutex_unlock(&g_session.lock);
            if (dirty) send_session_presence();
        }

        // The host's clock, once a second and only inside a loaded world - DayNightCycle.main
        // does not exist on the main menu, and a clock read from nowhere would corrupt the
        // guest's world anyway.
        if (now >= g_next_time) {
            g_next_time = now + kTimePacketInterval;
            bool publish = false;
            double value = 0.0;
            double rate = 0.0;
            pthread_mutex_lock(&g_time_lock);
            publish = g_local_time_in_game;
            value = g_local_time;
            rate = g_local_rate;
            pthread_mutex_unlock(&g_time_lock);
            if (publish && net_session_is_host()) {
                TimePacket packet{};
                packet.header.magic = kMagic;
                packet.header.type = kPacketTime;
                packet.time_passed = value;
                packet.rate = rate;

                pthread_once(&g_peers_lock_once, init_peers_lock);
                pthread_mutex_lock(&g_peers_lock);
                for (size_t i = 0; i < kMaxPeers; i++) {
                    if (!g_peers[i].active) continue;
                    if (!session_peer_eligible_locked(i, true)) continue;
                    struct sockaddr_in addr{};
                    addr.sin_family = AF_INET;
                    addr.sin_port = htons(g_peers[i].port);
                    inet_pton(AF_INET, g_peers[i].address, &addr.sin_addr);
                    send_to(&packet, sizeof(packet), addr);
                }
                pthread_mutex_unlock(&g_peers_lock);
            }
        }

        process_session_tick(now);

        for (size_t i = 0; i < kWorldMaxGuests; i++) {
            if (!g_guests[i].active) continue;
            if (now - g_guests[i].last_activity > kGuestIdleSeconds) {
                LOG_INFO("world: releasing idle guest %08x", g_guests[i].id);
                release_guest(&g_guests[i]);
                continue;
            }
            pump_guest(g_guests[i], now);
        }

        LocalState local{};
        double local_published_at = 0.0;
        pthread_mutex_lock(&g_state_lock);
        local = g_local;
        local_published_at = g_local_publish_at;
        pthread_mutex_unlock(&g_state_lock);

        if (local.valid) {
            pthread_once(&g_peers_lock_once, init_peers_lock);
            pthread_mutex_lock(&g_peers_lock);
            for (size_t i = 0; i < kMaxPeers; i++) {
                if (!g_peers[i].active) continue;
                if (!session_peer_eligible_locked(i, true)) continue;
                if (now - g_peers[i].last_sent < kStateInterval) continue;
                PoseStreamStats& stream = g_pose_stream[i];
                int send_errno = 0;
                const int sent = send_state(g_peers[i], local, &send_errno);
                if (sent == sizeof(StatePacket)) {
                    ++stream.tx_ok;
                } else {
                    ++stream.tx_failed;
                    stream.last_send_errno = send_errno;
                }
                g_peers[i].last_sent = now;
            }
            pthread_mutex_unlock(&g_peers_lock);
        }

        // Piloted vehicles ride alongside at 10 Hz. Same peers, independent timer.
        {
            static double next_vehicle_send = 0.0;
            VehicleState vehicle{};
            bool driving = false;
            pthread_mutex_lock(&g_state_lock);
            if (g_local_vehicle.active) {
                vehicle = g_local_vehicle;
                driving = true;
            }
            pthread_mutex_unlock(&g_state_lock);
            if (driving && local.valid && now >= next_vehicle_send) {
                next_vehicle_send = now + kVehicleInterval;
                pthread_once(&g_peers_lock_once, init_peers_lock);
                pthread_mutex_lock(&g_peers_lock);
                for (size_t i = 0; i < kMaxPeers; i++) {
                    if (!g_peers[i].active) continue;
                    if (!session_peer_eligible_locked(i, true)) continue;
                    send_vehicle(g_peers[i], vehicle);
                }
                pthread_mutex_unlock(&g_peers_lock);
            }
        }

        // Repeat whatever the peers have not acknowledged. After drop_stale_peers so the mask
        // it reads excludes peers that just timed out.
        drop_stale_peers(now);
        drop_stale_vehicles(now);
        service_world_events(now);
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
        geometry_service(now);
#endif

        if (now >= next_report) {
            LOG_INFO("pump alive: %llu loops, peers=%zu, local_valid=%d, rx hello=%llu "
                     "state=%llu other=%llu bytes=%llu, events tx=%llu rx=%llu dup=%llu "
                     "pending=%zu",
                     (unsigned long long)iterations, net_peer_count(), local.valid ? 1 : 0,
                     net_rx_hello(), net_rx_state(), net_rx_other(), net_rx_bytes(),
                     net_world_event_tx(), net_world_event_rx(), net_world_event_dup(),
                     net_pending_world_event_count());
            next_report = now + 5.0;
        }
        if (now >= next_pose_report) {
            pthread_mutex_lock(&g_peers_lock);
            pthread_mutex_lock(&g_session.lock);
            LOG_INFO("pose session: local=%08x host=%08x epoch=%u member=%d world=%d "
                     "role_host=%d state=%d detail=%s", g_local_id, g_session.host_id,
                     g_session.epoch, g_session.local_member, g_session.local_has_world,
                     g_session.local_host, int(g_session.state), g_session.detail);
            pthread_mutex_unlock(&g_session.lock);
            for (size_t i = 0; i < kMaxPeers; ++i) {
                if (!g_peers[i].active) continue;
                const PoseStreamStats& stream = g_pose_stream[i];
                const char* block = session_peer_block_reason_locked(i, true);
                if (!local.valid) block = "local_pose_invalid";
                LOG_INFO("pose stream: peer=%08x endpoint=%s:%u tx_ok=%llu tx_failed=%llu "
                         "send_errno=%d tx_gate=%s sample_age=%.1f presence_age=%.1f "
                         "rx_seen=%llu rx_applied=%llu rx_blocked=%llu rx_stale=%llu "
                         "rx_age=%.1f applied_age=%.1f last_rx_block=%s flags=%x pos=%.2f/%.2f/%.2f "
                         "remote_flags=%x remote_host=%08x remote_epoch=%u protocol_ok=%d "
                         "game_match=%d mod_match=%d join_granted=%d grant_epoch=%u",
                         g_peers[i].id, g_peers[i].address, g_peers[i].port,
                         stream.tx_ok, stream.tx_failed, stream.last_send_errno,
                         block ? block : "open",
                         local_published_at > 0.0 ? now - local_published_at : -1.0,
                         g_remote[i].valid ? now - g_remote[i].last_presence : -1.0,
                         stream.rx_seen, stream.rx_applied, stream.rx_blocked, stream.rx_stale,
                         stream.last_rx > 0.0 ? now - stream.last_rx : -1.0,
                         stream.last_applied > 0.0 ? now - stream.last_applied : -1.0,
                         stream.last_rx_block ? stream.last_rx_block : "none",
                         g_peers[i].flags, g_peers[i].x, g_peers[i].y, g_peers[i].z,
                         g_remote[i].flags, g_remote[i].host_id, g_remote[i].epoch,
                         g_remote[i].valid && g_remote[i].protocol_v3,
                         strcmp(g_remote[i].game_build, kGameBuild) == 0,
                         strcmp(g_remote[i].mod_version, kModVersion) == 0,
                         g_remote[i].join_accepted, g_remote[i].admitted_epoch);
            }
            pthread_mutex_unlock(&g_peers_lock);
            next_pose_report = now + 15.0;
        }
        sleep_millis(10);
    }

    LOG_INFO("lan pump stopping (id=%08x)", g_local_id);
    return nullptr;
}

}  // namespace

#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
bool net_geometry_enqueue(const base_geometry::GhostEnvelope& envelope) {
    return geometry_enqueue(envelope);
}
bool net_geometry_take(uint32_t* origin, base_geometry::GhostEnvelope* out) {
    return geometry_take(origin, out);
}
bool net_geometry_session(uint8_t out_id[16], uint32_t* epoch) {
    if (!out_id || !epoch) return false;
    GeometrySession session = geometry_session();
    if (!session.valid) return false;
    memcpy(out_id, session.id, sizeof(session.id));
    *epoch = session.epoch;
    return true;
}
#endif

// ---------------------------------------------------------------------------
// Slot auto-load request. Written by the pump when a guest slot lands complete (the only
// producer is set_handoff's Written branch), taken by the game thread at the main menu.
// One request is held: a newer one overwrites it, and it stays pending until taken, so a
// guest that is still in a world - or still booting - gets the load when the menu exists.
// ---------------------------------------------------------------------------
#include "autoload_request.inc"

// The event API is the one part of the transport the game thread calls directly, so it lives
// outside the anonymous namespace. The state it touches stays internal.
void net_post_world_event(const WorldEvent& event, bool already_applied) {
    if (!event_kind_known(event.kind)) return;
    // The build tail travels to peers as-is, but a build event without a terminated Id
    // would be refused by every receiver anyway - reject it here where the producer stands.
    if ((event.kind == kWorldEventBuild || event.kind == kWorldEventProgress ||
         event.kind == kWorldEventRemove) &&
        !memchr(event.object_id, '\0', sizeof(event.object_id))) {
        LOG_WARN("event: %s refused, unterminated object id", event_kind_name(event.kind));
        return;
    }
    queue_local_event(event, already_applied);
}

size_t net_copy_world_events(WorldEvent* out, size_t max) {
    size_t count = 0;
    pthread_mutex_lock(&g_event_lock);
    while (count < max && g_event_inbox_tail != g_event_inbox_head) {
        out[count++] = g_event_inbox[g_event_inbox_tail];
        g_event_inbox_tail = (g_event_inbox_tail + 1) % kEventInboxSize;
    }
    pthread_mutex_unlock(&g_event_lock);
    return count;
}

size_t net_pending_world_event_count() {
    size_t count = 0;
    pthread_mutex_lock(&g_event_lock);
    for (int i = 0; i < kEventRingSize; i++) {
        if (g_events.slots[i].used) count++;
    }
    pthread_mutex_unlock(&g_event_lock);
    return count;
}

unsigned long long net_world_event_rx() { return g_event_rx; }
unsigned long long net_world_event_tx() { return g_event_tx; }
unsigned long long net_world_event_dup() { return g_event_dup; }

void net_init() {
    if (g_socket >= 0) return;

    g_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_socket < 0) {
        LOG_ERROR("socket() failed: %s", strerror(errno));
        return;
    }

    int reuse = 1;
    setsockopt(g_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    int broadcast = 1;
    setsockopt(g_socket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kLanPort);
    if (bind(g_socket, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOG_ERROR("bind(%u) failed: %s", kLanPort, strerror(errno));
        close(g_socket);
        g_socket = -1;
        return;
    }

    memset(g_peers, 0, sizeof(g_peers));
    memset(g_remote, 0, sizeof(g_remote));
    memset(&g_local, 0, sizeof(g_local));
    g_local_id = make_local_id();
    pthread_mutex_lock(&g_session.lock);
    g_session.state = kSessionDiscovering;
    g_session.reason = kJoinAccepted;
    snprintf(g_session.detail, sizeof(g_session.detail), "discovering");
    pthread_mutex_unlock(&g_session.lock);

    // The socket now belongs to the pump thread, so a short receive timeout is enough to
    // keep the loop responsive without spinning at 100% CPU.
    int flags = fcntl(g_socket, F_GETFL, 0);
    fcntl(g_socket, F_SETFL, flags | O_NONBLOCK);

    struct timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 2000;
    setsockopt(g_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    g_pump_running = true;
    if (pthread_create(&g_pump, nullptr, pump_main, nullptr) != 0) {
        LOG_ERROR("pthread_create failed: %s", strerror(errno));
        g_pump_running = false;
        close(g_socket);
        g_socket = -1;
        return;
    }

    g_pump_beat_ms = MonotonicMs();
    g_watchdog_running = true;
    if (pthread_create(&g_watchdog, nullptr, WatchdogMain, nullptr) != 0) {
        LOG_ERROR("watchdog pthread_create failed: %s", strerror(errno));
        g_watchdog_running = false;
    }

    LOG_INFO("lan transport ready: id=%08x udp/%u (pump thread + watchdog)", g_local_id, kLanPort);
}

void net_publish(const LocalState& local) {
    pthread_mutex_lock(&g_state_lock);
    g_local = local;
    g_local_publish_at = monotonic_seconds();
    pthread_mutex_unlock(&g_state_lock);
}

void net_publish_vehicle(const VehicleState& vehicle) {
    pthread_mutex_lock(&g_state_lock);
    g_local_vehicle = vehicle;
    pthread_mutex_unlock(&g_state_lock);
}

size_t net_copy_vehicles(VehicleState* out, size_t max) {
    if (!out || max == 0) return 0;
    size_t written = 0;
    pthread_mutex_lock(&g_peers_lock);
    for (size_t i = 0; i < kMaxVehicles && written < max; i++) {
        if (!g_vehicles[i].active) continue;
        out[written++] = g_vehicles[i];
    }
    pthread_mutex_unlock(&g_peers_lock);
    return written;
}

size_t net_peer_count() {
    size_t count = 0;
    pthread_mutex_lock(&g_peers_lock);
    for (size_t i = 0; i < kMaxPeers; i++) {
        if (g_peers[i].active) count++;
    }
    pthread_mutex_unlock(&g_peers_lock);
    return count;
}

size_t net_copy_peers(PeerState* out, size_t max) {
    if (!out || max == 0) return 0;
    size_t written = 0;
    pthread_mutex_lock(&g_peers_lock);
    for (size_t i = 0; i < kMaxPeers && written < max; i++) {
        if (!g_peers[i].active) continue;
        out[written++] = g_peers[i];
    }
    pthread_mutex_unlock(&g_peers_lock);
    return written;
}

uint32_t net_local_id() {
    return g_local_id;
}

size_t net_copy_sessions(SessionCandidate* out, size_t max) {
    if (!out || max == 0) return 0;
    size_t count = 0;
    double now = monotonic_seconds();
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    for (size_t i = 0; i < kMaxPeers && count < max; ++i) {
        const RemotePresence& remote = g_remote[i];
        if (!g_peers[i].active || !remote.valid || !remote.protocol_v3 ||
            now - remote.last_presence > kPeerTimeout ||
            !(remote.flags & kSessionFlagIsHost) ||
            !bytes_nonzero(remote.session_id, sizeof(remote.session_id))) {
            continue;
        }
        SessionCandidate& candidate = out[count++];
        memset(&candidate, 0, sizeof(candidate));
        candidate.host_id = g_peers[i].id;
        candidate.session_epoch = remote.epoch;
        memcpy(candidate.session_id, remote.session_id, sizeof(candidate.session_id));
        memcpy(candidate.content_guid, remote.content_guid, sizeof(candidate.content_guid));
        memcpy(candidate.slot, remote.slot, sizeof(candidate.slot));
        memcpy(candidate.game_build, remote.game_build, sizeof(candidate.game_build));
        memcpy(candidate.mod_version, remote.mod_version, sizeof(candidate.mod_version));
        candidate.player_count = remote.player_count;
        candidate.max_players = remote.max_players;
        candidate.compatible = strcmp(remote.game_build, kGameBuild) == 0 &&
                               strcmp(remote.mod_version, kModVersion) == 0;
        candidate.reason = strcmp(remote.game_build, kGameBuild) != 0
                               ? kJoinGameMismatch
                               : (strcmp(remote.mod_version, kModVersion) != 0
                                      ? kJoinModMismatch
                                      : kJoinAccepted);
    }
    pthread_mutex_unlock(&g_peers_lock);
    return count;
}

SessionState net_session_state() {
    pthread_mutex_lock(&g_session.lock);
    SessionState state = g_session.state;
    pthread_mutex_unlock(&g_session.lock);
    if (state != kSessionDiscovering && state != kSessionHostVisible &&
        state != kSessionIncompatible) {
        return state;
    }
    SessionCandidate candidates[kMaxSessionCandidates]{};
    size_t count = net_copy_sessions(candidates, kMaxSessionCandidates);
    if (count == 0) return kSessionDiscovering;
    for (size_t i = 0; i < count; ++i)
        if (candidates[i].compatible) return kSessionHostVisible;
    return kSessionIncompatible;
}

SessionJoinStatus net_session_reason() {
    pthread_mutex_lock(&g_session.lock);
    SessionState state = g_session.state;
    SessionJoinStatus reason = g_session.reason;
    pthread_mutex_unlock(&g_session.lock);
    if (state != kSessionDiscovering && state != kSessionHostVisible &&
        state != kSessionIncompatible) {
        return reason;
    }
    SessionCandidate candidates[kMaxSessionCandidates]{};
    size_t count = net_copy_sessions(candidates, kMaxSessionCandidates);
    for (size_t i = 0; i < count; ++i) {
        if (candidates[i].compatible) return kJoinAccepted;
    }
    return count ? candidates[0].reason : kJoinAccepted;
}

void net_session_detail(char* out, size_t capacity) {
    if (!out || capacity == 0) return;
    pthread_mutex_lock(&g_session.lock);
    snprintf(out, capacity, "%s", g_session.detail[0] ? g_session.detail : "discovering");
    pthread_mutex_unlock(&g_session.lock);
}

bool net_session_is_host() {
    pthread_mutex_lock(&g_session.lock);
    bool host = g_session.local_host && g_session.local_member &&
                g_session.local_has_world;
    pthread_mutex_unlock(&g_session.lock);
    return host;
}

bool net_session_join(uint32_t host_id, const uint8_t session_id[16]) {
    if (!host_id || !session_id || !bytes_nonzero(session_id, 16)) return false;
    RemotePresence selected{};
    bool found = false;
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    int index = peer_index_by_id(host_id);
    if (index >= 0) {
        const RemotePresence& remote = g_remote[index];
        found = remote.valid && remote.protocol_v3 &&
                (remote.flags & kSessionFlagIsHost) &&
                same_session(remote.session_id, session_id) &&
                monotonic_seconds() - remote.last_presence <= kPeerTimeout;
        if (found) selected = remote;
    }
    pthread_mutex_unlock(&g_peers_lock);
    if (!found) return false;

    SessionJoinStatus reason = kJoinAccepted;
    if (strcmp(selected.game_build, kGameBuild) != 0) reason = kJoinGameMismatch;
    else if (strcmp(selected.mod_version, kModVersion) != 0) reason = kJoinModMismatch;
    if (reason != kJoinAccepted) {
        pthread_mutex_lock(&g_session.lock);
        session_set_state_locked(kSessionIncompatible, reason,
                                 reason == kJoinGameMismatch ? "game build mismatch"
                                                              : "mod build mismatch");
        pthread_mutex_unlock(&g_session.lock);
        return false;
    }

    uint64_t nonce = 0;
    random_bytes(reinterpret_cast<uint8_t*>(&nonce), sizeof(nonce));
    if (nonce == 0) nonce = (static_cast<uint64_t>(monotonic_seconds() * 1000000.0) << 16) |
                            static_cast<uint64_t>(g_local_id);
    pthread_mutex_lock(&g_session.lock);
    if (g_session.world_active || g_session.local_has_world || g_session.local_host ||
        g_session.join_request_pending ||
        g_session.join_accepted) {
        pthread_mutex_unlock(&g_session.lock);
        return false;
    }
    g_session.selected = true;
    g_session.selected_host = host_id;
    g_session.selected_epoch = selected.epoch;
    memcpy(g_session.selected_session, selected.session_id, sizeof(g_session.selected_session));
    memcpy(g_session.selected_guid, selected.content_guid, sizeof(g_session.selected_guid));
    snprintf(g_session.selected_slot, sizeof(g_session.selected_slot), "%s", selected.slot);
    g_session.join_nonce = nonce;
    g_session.join_request_pending = true;
    g_session.join_accepted = false;
    g_session.last_join_send = 0.0;
    g_session.join_started = monotonic_seconds();
    g_session.reason = kJoinAccepted;
    session_set_state_locked(kSessionJoinPending, kJoinAccepted, "join request pending");
    pthread_mutex_unlock(&g_session.lock);
    return true;
}

void net_session_leave() {
    pthread_mutex_lock(&g_session.lock);
    if (g_session.active || g_session.join_request_pending || g_session.selected) {
        g_session.explicit_leave = true;
    }
    pthread_mutex_unlock(&g_session.lock);
}

#include "session_world_context.inc"
#include "session_launch_role.inc"

#include "session_local_world.inc"

void net_session_set_world_active(bool active) {
    pthread_mutex_lock(&g_session.lock);
    g_session.world_active = active;
    pthread_mutex_unlock(&g_session.lock);
}

// ---------------------------------------------------------------------------
// World handoff, public surface
// ---------------------------------------------------------------------------

// The offer is copied rather than referenced. world_capture() runs on the game thread and can
// free and refill its buffers at any point, so the pump must not be reading them directly.
// Day/night clock: written by the game thread every frame, shipped by the pump while host.
void net_set_local_time(double time_passed, bool in_game, double rate) {
    pthread_mutex_lock(&g_time_lock);
    g_local_time = time_passed;
    g_local_time_in_game = in_game;
    g_local_rate = rate;
    pthread_mutex_unlock(&g_time_lock);
}

// The clock last received from the host, with the monotonic timestamp it arrived at and the
// rate the host measured for its own clock. Returns false until the first TimePacket, which
// keeps the guest from correcting against nothing.
bool net_host_time(double* time_passed, double* recv_at, double* rate) {
    pthread_mutex_lock(&g_time_lock);
    bool valid = g_host_time_valid;
    if (valid) {
        *time_passed = g_host_time;
        *recv_at = g_host_time_at;
        *rate = g_host_rate;
    }
    pthread_mutex_unlock(&g_time_lock);
    return valid;
}

void net_offer_world(const WorldPayload* payload, const char* slot) {
    // The new snapshot is built entirely on the game thread from the payload, which is
    // itself a stable capture snapshot. The pump never sees the new pointer until the
    // swap below, and in-flight guests keep the old snapshot alive through their own
    // reference, so no send path can ever touch freed or replaced buffers.
    WorldOffer* next = nullptr;
    if (payload && payload->valid && payload->file_count == kWorldFileCount) {
        // An empty slot name would tell the guest to write into the saves root itself.
        if (slot && *slot && strlen(slot) < 24) {
            size_t total = 0;
            for (int i = 0; i < kWorldFileCount; i++) total += payload->files[i].size;
            if (total > kWorldOfferLimit) {
                LOG_WARN("world: capture is %zu bytes, over the %zu byte offer limit, "
                         "not serving it",
                         total, kWorldOfferLimit);
            } else {
                next = static_cast<WorldOffer*>(calloc(1, sizeof(WorldOffer)));
                if (next) {
                    next->valid = true;
                    next->file_count = payload->file_count;
                    next->total_bytes = total;
                    snprintf(next->slot, sizeof(next->slot), "%s", slot);
                    for (int i = 0; i < kWorldFileCount; i++) {
                        snprintf(next->files[i].name, sizeof(next->files[i].name), "%s",
                                 payload->files[i].name);
                        next->files[i].size = payload->files[i].size;
                        next->files[i].checksum = payload->files[i].checksum;
                        next->files[i].data =
                            static_cast<uint8_t*>(malloc(payload->files[i].size));
                        if (!next->files[i].data) {
                            LOG_ERROR("world: out of memory offering %s",
                                      payload->files[i].name);
                            for (int k = 0; k < kWorldFileCount; k++) {
                                free(next->files[k].data);
                                next->files[k].data = nullptr;
                            }
                            free(next);
                            next = nullptr;
                            break;
                        }
                        memcpy(next->files[i].data, payload->files[i].data,
                               payload->files[i].size);
                    }
                }
            }
        } else {
            LOG_WARN("world: capture has no slot name or its name exceeds the manifest field, "
                     "not serving it");
        }
    }

    // Atomic swap. In-flight guests keep their own reference to the old snapshot and
    // finish their transfer on it; only the pointer is replaced here.
    pthread_mutex_lock(&g_offer_lock);
    WorldOffer* old = g_offer_snapshot;
    g_offer_snapshot = next;
    if (next) {
        g_offer_generation++;
        next->generation = g_offer_generation;
        next->refs = 1;  // publication reference
    }
    // We already hold g_offer_lock here: use the locked helper. offer_release() is only for
    // callers outside the offer critical section and would recursively lock this non-recursive
    // mutex, freezing UnityMain on refresh and blocking session presence in the pump.
    if (old) offer_release_locked(old);
    // The published reference keeps next alive after releasing the swap lock.
    pthread_mutex_unlock(&g_offer_lock);
    if (next) {
        LOG_INFO("world: offering local world (gen %llu), %zu bytes across %d files",
                 (unsigned long long)next->generation, next->total_bytes, next->file_count);
    } else {
        LOG_INFO("world: offer cleared, in-flight guests finish their old transfer");
    }
}  // net_offer_world

uint64_t net_world_offer_generation() {
    pthread_mutex_lock(&g_offer_lock);
    uint64_t gen = (g_offer_snapshot && g_offer_snapshot->valid)
                       ? g_offer_snapshot->generation
                       : 0;
    pthread_mutex_unlock(&g_offer_lock);
    return gen;
}

WorldHandoffState net_world_handoff_state() {
    pthread_mutex_lock(&g_handoff_lock);
    WorldHandoffState state = g_handoff_state;
    pthread_mutex_unlock(&g_handoff_lock);
    return state;
}

// Copied out under the lock rather than handing back a pointer into shared storage: the pump
// thread writes these while the game thread reads them for logging.
void net_world_handoff_slot(char* out, size_t capacity) {
    if (!out || capacity == 0) return;
    pthread_mutex_lock(&g_handoff_lock);
    snprintf(out, capacity, "%s", g_handoff_slot);
    pthread_mutex_unlock(&g_handoff_lock);
}

void net_world_handoff_detail(char* out, size_t capacity) {
    if (!out || capacity == 0) return;
    pthread_mutex_lock(&g_handoff_lock);
    snprintf(out, capacity, "%s", g_handoff_detail);
    pthread_mutex_unlock(&g_handoff_lock);
}

void net_world_handoff_clear() { set_handoff(kWorldHandoffIdle, "", ""); }
