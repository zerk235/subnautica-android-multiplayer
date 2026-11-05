#pragma once

#include <cstddef>
#include <cstdint>

// Complete phone-to-phone saved world handoff. Four native root files plus
// a deterministic transport archive of saved CellsCache entity files; these
// files are restored unchanged before the guest starts the exact-slot load.
// Native ZIPs remain opaque and are never decompressed by this transfer code.

// Four native root files plus a bounded archive of saved entity cells. The archive
// is transport-only: the guest restores the original CellsCache files before load.
constexpr int kWorldFileCount = 5;
constexpr size_t kWorldMaxTransferBytes = 96u * 1024u * 1024u;

// Chunk size for the world transfer. Both sides must agree on this.
constexpr uint32_t kWorldChunkPayload = 1024;

// Number of chunks needed to cover a file of the given size.
inline uint32_t chunks_for(uint32_t size) {
    return (size + kWorldChunkPayload - 1) / kWorldChunkPayload;
}

// A single slot file staged in memory. data is malloc'd and owned by the payload.
struct WorldFile {
    char name[32];
    uint32_t size;
    uint32_t checksum;
    uint8_t* data;
};

struct WorldPayload {
    bool valid;
    int file_count;
    size_t total_bytes;
    uint8_t content_guid[16];
    bool content_guid_valid;
    WorldFile files[kWorldFileCount];
};

// Reads the identity files of a save slot into memory. Passing an empty or null slot clears
// any previous capture and returns 0. Safe to call repeatedly; the previous capture is
// released first.
size_t world_capture(const char* slot);

// The staged payload, or null when nothing has been captured. Owned by this subsystem and
// invalidated by the next world_capture or world_release.
const WorldPayload* world_payload();

void world_release();

// Captures a slot straight from the saves directory into caller memory. Pure file I/O, no
// Unity: this is the only capture path the pump thread may call, which lets the transport
// rebuild a servable world at the moment a guest asks, instead of depending on a game-thread
// publication that may have been cleared between save ticks. Same gates as world_capture -
// plain slot name, all native files plus saved cells, usable sizes, gameinfo session GUID - and all-or-nothing:
// on failure *out is left zeroed. The caller owns the payload and releases it with
// world_payload_free().
bool world_disk_capture(const char* slot, WorldPayload* out);

// Probes without allocating: true when the slot directory holds all native files plus saved cells with usable
// sizes and gameinfo.json still parses to a session GUID. Answers "could this world be
// served right now" for availability checks a thousand times cheaper than a full read.
bool world_disk_ready(const char* slot);

// Frees a payload returned by world_disk_capture.
void world_payload_free(WorldPayload* out);

// FNV-1a. Only has to catch a truncated or misordered transfer, so it stays cheap enough to
// run over every chunk as it arrives.
uint32_t world_checksum(const void* data, size_t size);

// ---------------------------------------------------------------------------
// Receiving side, used by the guest. The transport moves opaque bytes; everything about
// interpreting them as a world lives here.
// ---------------------------------------------------------------------------

// What the guest was told to expect: the manifest the host sent.
struct WorldManifest {
    char slot[24];
    int file_count;
    uint32_t total_bytes;
    struct {
        char name[32];
        uint32_t size;
        uint32_t checksum;
    } files[kWorldFileCount];
};

// Allocates the receive buffers for a manifest. Frees anything held before, so a retried
// transfer starts clean. Returns false on a manifest that cannot be a world at all.
bool world_begin_receive(const WorldManifest& manifest);

// Stores one chunk at the given file/offset. Out-of-order and duplicate chunks are accepted;
// the checksum at the end is what decides whether the result is usable. Returns false only
// when the arguments are out of range.
bool world_receive_chunk(int file_index, uint32_t offset, const void* data, size_t size);

// True once every byte in the manifest has arrived. Also re-checks each file's checksum and
// logs a mismatch per file rather than a single opaque failure.
bool world_receive_verify(const WorldManifest& manifest);
bool world_receive_content_guid(uint8_t out[16]);

// Writes the received files into a new slot directory under the saves root. Each file is
// written to a temporary name and renamed into place, so an interrupted transfer can never
// leave a slot that looks complete but is not. Refuses to touch a slot that already exists.
bool world_write_slot(const WorldManifest& manifest,bool preserve_local=false);

enum WorldSlotCheck {
    kWorldSlotMissing = 0,
    kWorldSlotMatches,
    kWorldSlotConflict,
    kWorldSlotCheckError,
};

// Checks every file on disk against the manifest (exact size and checksum). Existing but
// different/incomplete slots are conflicts; they must never be overwritten.
WorldSlotCheck world_slot_exists(const char* slot, const WorldManifest& manifest);

// True when the slot was previously received from a peer: world_write_slot leaves a small
// marker file behind. Conflict policy uses this - a peer-received slot may be overwritten
// by a newer peer copy (the refresh path), while a locally built slot stays protected.
bool world_slot_is_host_received(const char* slot);

// The gameinfo.json values the menu's own load path passes to LoadGameAsync. They are read
// back from the slot itself instead of assumed, so a load behaves exactly like pressing the
// load button on that slot.
struct WorldLoadFields {
    bool valid;
    char session[40];  // the 36-character session GUID as text
    int change_set;
    int game_mode;     // GameMode enum value: Survival 0, Freedom 1, Hardcore 2, Creative 3
};

// Reads the load fields of a slot's gameinfo.json. Pure file I/O, safe on any thread.
// False when the slot has no readable gameinfo.json or a required field is missing;
// *out is zeroed either way.
bool world_read_load_fields(const char* slot, WorldLoadFields* out);

// The receive buffers, for the transport to read a manifest back out of.
const WorldManifest* world_receive_manifest();
uint8_t* world_receive_file_data(int file_index, uint32_t size);
uint32_t world_receive_received(int file_index);
// Chunks forming an unbroken run from the start of the file. This is the honest answer to "how
// much of this file has arrived", unlike the high-water mark.
uint32_t world_receive_contiguous(int file_index);
void world_reset_receive();
