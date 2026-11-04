#pragma once

#include <netinet/in.h>

#include <cstddef>
#include <cstdint>

#include "world.h"
#include "held_rotation.h"
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
#include "base_geometry_envelope.h"
#endif

// LAN transport for the co-op layer. A dedicated pump thread owns the socket so peer
// discovery also works while the game sits on the main menu; the game thread only
// publishes a pose sample via net_publish(), which never blocks.
constexpr uint16_t kLanPort = 47777;
constexpr size_t kMaxPeers = 7;
constexpr double kPeerTimeout = 5.0;
constexpr double kHelloInterval = 1.0;
constexpr double kStateInterval = 0.05;  // 20 Hz
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
constexpr uint16_t kSessionProtocolVersion = 23;  // invalidate stale host world on menu/load/slot changes
#else
constexpr uint16_t kSessionProtocolVersion = 7;  // complete saved cell handoff (five-entry manifest)
#endif
constexpr size_t kMaxSessionCandidates = kMaxPeers;

constexpr uint32_t kFlagSwimming = 1u << 0;
constexpr uint32_t kFlagExosuit = 1u << 1;
constexpr uint32_t kFlagSeamoth = 1u << 2;
constexpr uint32_t kFlagWalking = 1u << 3;
constexpr uint32_t kFlagInside = 1u << 4;
constexpr uint32_t kFlagPiloting = 1u << 5;
// The Animator picks its locomotion state from is_underwater / on_surface, and those are
// driven by Player.IsUnderwaterForSwimming on the local side. IsSwimming alone is not enough:
// a diver walking along the seabed is also "swimming", so the two states travel separately.
constexpr uint32_t kFlagUnderwater = 1u << 6;
constexpr uint32_t kFlagOnSurface = 1u << 7;
// Tool-in-use moment (knife swing, scanner scan...): the peer plays the use, not just the hold.
constexpr uint32_t kFlagUsingTool = 1u << 8;

// Number of Animator float parameters carried in the state packet. The controller has exactly
// thirteen today and the cap leaves room, but the payload is sized by this constant on both
// ends, so it must stay identical in LocalState, PeerState and StatePacket.
constexpr int kNetAnimFloats = 16;

struct PeerState {
    uint32_t id;
    char address[16];
    uint16_t port;
    double last_seen;
    double last_sent;
    uint32_t sequence;
    // Pump-side reorder guard: STATE arrives at 20 Hz over UDP and a late
    // duplicate must not roll pose/flags back (e.g. clear the piloting bit a
    // newer packet already set). Wrap-aware; a timed-out peer rejoins with a
    // zeroed slot, which re-arms on the first packet.
    bool seq_valid;
    float x;
    float y;
    float z;
    float yaw;
    float pitch;
    float view_yaw;
    uint32_t flags;
    // TechType of the item the peer currently holds, as the enum's integer value. Zero means bare
    // hands. Both phones run the same game build, so the numeric values agree without a lookup
    // table; the receiver turns the number into a prop on its local copy of that item type.
    uint32_t held_tech;
    HeldRotation held_rotation;
    uint8_t held_rotation_valid;
    // Equipment sync (item sync v3): the peer's Foots/Head slot TechTypes plus the suit as a
    // male_geo children bitmask (bit i = child i active, read by the sender off its own
    // hierarchy). The receiver replicates the mask instead of mirroring the local player's
    // suit, which is what dressed every avatar in the wrong suit.
    uint32_t suit_tech;
    uint32_t fins_tech;
    uint32_t head_tech;
    uint8_t environment_id[16]; // exact shared SubRoot identity; zero outdoors/unknown
    struct sockaddr_in raw;
    bool active;
    float anim_floats[kNetAnimFloats];
    int anim_float_count;
    uint8_t anim_underwater;
    uint8_t anim_cinematics;
};

// Local player sample, filled in by the game thread on every Player.Update.
struct LocalState {
    float x;
    float y;
    float z;
    float yaw;
    float pitch;
    float view_yaw;
    uint32_t flags;
    bool valid;
    // TechType of the held item as an integer, 0 when the hands are empty. Read from
    // Inventory.GetHeld() -> Pickupable.GetTechType() on the game thread.
    uint32_t held_tech;
    HeldRotation held_rotation;
    uint8_t held_rotation_valid;
    // Worn equipment: suit as a male_geo bitmask (see read_suit_mask), Foots/Head as TechTypes,
    // 0 when the slot is empty. Read on the game thread.
    uint32_t suit_tech;
    uint32_t fins_tech;
    uint32_t head_tech;
    uint8_t environment_id[16];
    // The peer's Animator is mirrored verbatim instead of being reconstructed from a handful of
    // guessed parameter names. Whatever the controller does with these values, it does the same
    // thing on both phones, which is the only way to get the same pose without owning Unity's
    // animation graph.
    float anim_floats[kNetAnimFloats];
    int anim_float_count;
    uint8_t anim_underwater;
    uint8_t anim_cinematics;
};

void net_init();

// Called from the game thread; hands the latest pose to the pump thread.
void net_publish(const LocalState& local);

// Day/night clock, ported from the zcode session's world sync v2. The game thread reports the
// local DayNightCycle clock every frame; while this phone is the host the pump ships it to the
// peers once a second, and net_host_time hands the value received from the host back to the
// game thread, which pulls its own clock towards it.
void net_set_local_time(double time_passed, bool in_game, double rate);
bool net_host_time(double* time_passed, double* recv_at, double* rate);

// Use net_copy_peers: handing out a pointer into the peer table is unsafe because the
// pump thread mutates it concurrently.
size_t net_peer_count();

// Copies the current peer table so the game thread can read it without racing the pump.
size_t net_copy_peers(PeerState* out, size_t max);

uint32_t net_local_id();

#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
// Experimental snapshot transport. Never applies geometry to a game object.
bool net_geometry_enqueue(const base_geometry::GhostEnvelope& envelope);
bool net_geometry_take(uint32_t* origin, base_geometry::GhostEnvelope* out);
bool net_geometry_session(uint8_t out_id[16], uint32_t* epoch);
#endif

enum SessionState {
    kSessionDiscovering = 0,
    kSessionHostVisible,
    kSessionIncompatible,
    kSessionJoinPending,
    kSessionTransferring,
    kSessionSlotReady,
    kSessionConflict,
    kSessionFailed,
    kSessionWaitingForWorld,
    kSessionMigrating,
    kSessionHost,
    kSessionGuest,
};

enum SessionJoinStatus {
    kJoinAccepted = 0,
    kJoinProtocolMismatch = 1,
    kJoinGameMismatch = 2,
    kJoinModMismatch = 3,
    kJoinFull = 4,
    kJoinBusy = 5,
    kJoinStaleSession = 6,
    kJoinGuestInGame = 7,
    kJoinWorldUnavailable = 8,
};

struct SessionCandidate {
    uint32_t host_id;
    uint32_t session_epoch;
    uint8_t session_id[16];
    uint8_t content_guid[16];
    char slot[24];
    char game_build[32];
    char mod_version[41];
    uint8_t player_count;
    uint8_t max_players;
    bool compatible;
    SessionJoinStatus reason;
};

// Snapshot getters; no pointer into pump-owned state escapes.
size_t net_copy_sessions(SessionCandidate* out, size_t max);
SessionState net_session_state();
SessionJoinStatus net_session_reason();
void net_session_detail(char* out, size_t capacity);
bool net_session_is_host();

// UI-facing session API. Join targets one exact host/session and is refused while this
// device already has a loaded world. Leave is explicit; returning to the menu is not leave.
bool net_session_join(uint32_t host_id, const uint8_t session_id[16]);
void net_session_leave();

// Game-thread context setter. content_guid is the 16-byte gameinfo.json session GUID;
// pass nullptr when it is not currently available.
void net_session_set_local_world(bool has_world, const char* slot,
                                 const uint8_t content_guid[16]);
bool net_session_invalidate_host_world();
// Immediate gameplay-state gate, independent of when the save subsystem exposes the slot.
// This prevents a UI caller from requesting a join during the first frames of a loaded world.
void net_session_set_world_active(bool active);

// ---------------------------------------------------------------------------
// Vehicle sync
//
// Whoever pilots a Seamoth/Prawn publishes its transform; both phones share the save, so the
// vehicle exists on both ends under the same UniqueIdentifier guid and only its pose travels.
// The receiver teleports its local copy and parks it kinematic while driven, then restores it
// on exit/timeout. The pilot's avatar hides (it sits inside).
// ---------------------------------------------------------------------------
struct VehicleState {
    uint32_t pilot;  // sender id (who drives)
    uint8_t guid[16];
    char classId[16];  // prefab class id ("seamoth"/"cyclops"/"prawn"); clones when guid misses
    float x, y, z;
    float yaw, pitch, roll;  // euler degrees, Unity order
    uint32_t sequence;
    // Same reorder guard as PeerState: a stale pose must not teleport the copy
    // backwards. Reset whenever the slot is assigned to a new pilot/guid.
    bool seq_valid;
    double updated;  // pump monotonic time, receipt side
    bool active;
};

// Called from the game thread every frame; active while this phone pilots, inactive once
// otherwise (a single inactive publish on exit is enough, the field stays put).
void net_publish_vehicle(const VehicleState& vehicle);

// Copies received vehicle states for the game thread (same lock discipline as peers).
size_t net_copy_vehicles(VehicleState* out, size_t max);

// ---------------------------------------------------------------------------
// World handoff
//
// The current session host is the source of truth. A menu-only member receives the host's
// slot after joining; the player loads it manually. No save is swapped into a loaded world.
// ---------------------------------------------------------------------------

// Called from the game thread once world_capture has staged the local world. The bytes are
// copied into pump-owned storage: the capture buffer belongs to the game thread and the
// pump must never read it while a later capture could be replacing it.
void net_offer_world(const WorldPayload* payload, const char* slot);

// Current published offer's generation for telemetry logs (0 when nothing is offered).
uint64_t net_world_offer_generation();

enum WorldHandoffState {
    kWorldHandoffIdle = 0,
    kWorldHandoffReceiving,
    kWorldHandoffVerified,
    kWorldHandoffWritten,
    kWorldHandoffFailed,
};

// Polled from the game thread. The pump thread does all the work and only publishes the
// outcome here.
WorldHandoffState net_world_handoff_state();
void net_world_handoff_slot(char* out, size_t capacity);
void net_world_handoff_detail(char* out, size_t capacity);
void net_world_handoff_clear();

// ---------------------------------------------------------------------------
// Slot auto-load request
//
// Raised by the pump when a guest slot is fully written (or confirmed identical on disk),
// consumed by the game thread at the main menu to start the game's own load flow for that
// slot. One request is held: a newer one overwrites it, and it stays pending until taken,
// so the game thread can wait for the menu to exist instead of dropping the request.
// ---------------------------------------------------------------------------

// Copies out the pending request without taking it. False when nothing is pending.
bool net_world_autoload_peek(char* slot_out, size_t capacity, uint32_t* generation_out,
                             double* raised_at_out);
// Discards only this exact request; a newer generation remains queued.
bool net_world_autoload_pop_if(const char* slot, uint32_t generation);
// Consumes this exact request only for a joined, worldless guest in SlotReady.
bool net_world_autoload_claim(const char* slot, uint32_t generation);

// ---------------------------------------------------------------------------
// Reliable world events
//
// Poses are sent at 20 Hz and a dropped sample is replaced by the next one, so they need no
// guarantee. A pickup is not like that: losing it leaves the item in one player's hands and
// on the floor for everybody else. Events therefore carry a sequence number, are repeated
// until every peer acknowledges them, and are de-duplicated on arrival.
// ---------------------------------------------------------------------------

enum WorldEventKind : uint8_t {
    kWorldEventPickup = 1,
    kWorldEventDrop = 2,
    // Build sync (v1, containment): full snapshots of an outdoor Constructable. Both are
    // replayable states, not deltas - a Progress that overtakes a lost Build still creates
    // the object, and every snapshot is re-sent by the periodic repair scan.
    kWorldEventBuild = 3,
    kWorldEventProgress = 4,
    // Deconstruction: the piece with this Id no longer exists on the sender. Carries the
    // last known snapshot for validation; the receiver runs SetState(false, false) on the
    // real class so the game's own deconstruction path rebuilds the base geometry.
    kWorldEventRemove = 5,
};

// build_flags bits for the build kinds. complete/isInside/forceUpright/rotatable/attached
// mirror the Constructable(able) fields they name (dump.cs 11228/11474); anchored marks an
// event whose v2 anchor tail is meaningful - the receiver may then run the base-module
// spawn path instead of the plain outdoor one.
constexpr uint8_t kWorldBuildComplete = 0x01;
constexpr uint8_t kWorldBuildIsInside = 0x02;
constexpr uint8_t kWorldBuildForceUpright = 0x04;
constexpr uint8_t kWorldBuildRotatable = 0x08;
constexpr uint8_t kWorldBuildAttached = 0x10;
constexpr uint8_t kWorldBuildAnchored = 0x20;
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
constexpr uint8_t kWorldBuildHasGeometry = 0x40;
#endif

// Room for the builder's UniqueIdentifier.Id string (a 36-character GUID with room for the
// longer ids the game can hand out). Build events carry it; pickup/drop events leave it
// empty and address the object by guid alone.
constexpr int kWorldEventObjectIdSize = 128;

// guid is the object's UniqueIdentifier. It is the only stable handle that both phones agree on:
// managed pointers differ per process and a name collides between every instance of a prefab.
// For pickup/drop only the kind/guid/pose/velocity fields are meaningful; build kinds add
// object_id/class_id/tech_type/constructed_amount/build_flags, and anchored base pieces
// (ConstructableBase modules) add the v2 anchor tail: the serialized anchors the game itself
// keeps on ConstructableBase and resolves on load (dump.cs 11474). The struct is the game-side
// carrier, the wire copy is WorldEventPacket in net.cpp - keep the two field-for-field.
struct WorldEvent {
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
    // Throw velocity, in world space, taken from the rigidbody on the phone that dropped it.
    // Without it the receiving phone's copy has to be placed and released, and an object that
    // was thrown simply drops from wherever it was released instead of carrying on.
    float vx;
    float vy;
    float vz;
    char object_id[kWorldEventObjectIdSize];
    // --- v2 anchor tail, meaningful when kWorldBuildAnchored is set ---
    int32_t proto_version;             // ConstructableBase.protoVersion (0xD4)
    int32_t face_linked_module_type;   // ConstructableBase.faceLinkedModuleType (0xD8, TechType)
    float face_linked_position[3];     // ConstructableBase.faceLinkedModulePosition (0xDC)
    int32_t face_cell[3];              // Base.Face.cell (Int3, dump.cs 5267)
    uint32_t face_direction;           // Base.Face.direction
    uint8_t face_has_value;            // Nullable<Base.Face>.hasValue (moduleFace, 0xE8)
    uint8_t reserved8[3];
#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)
    uint32_t geometry_hash_low;
    uint32_t geometry_hash_high;
#endif
};

// Queues an event for every peer. Call from the game thread. The event is repeated until it is
// acknowledged, so the caller does not wait for the network.
// already_applied says the local game already performed the action: the host picks items up
// locally and must not have its own hook run a second time when the broadcast comes back.
void net_post_world_event(const WorldEvent& event, bool already_applied);

// Drains events received from peers. Call from the game thread - the actual Pickupable calls
// have to happen on the thread that owns Unity.
size_t net_copy_world_events(WorldEvent* out, size_t max);

// Events still waiting for an acknowledgement, for the log.
size_t net_pending_world_event_count();

// Number of events received, applied or refused. A rising refused count means peers disagree
// about an object, which is the failure mode worth spotting in the field.
unsigned long long net_world_event_rx();
unsigned long long net_world_event_tx();
unsigned long long net_world_event_dup();
