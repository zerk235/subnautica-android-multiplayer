#include "world.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <algorithm>
#include <string>
#include <vector>

#include "log.h"

namespace {

// Slot directories live here on Android. patch.py backs saves up from exactly this path, so
// it is the one location the game and this process have both demonstrably read.
#ifndef SNMOD_WORLD_TEST_ROOT
#define SNMOD_WORLD_TEST_ROOT "/sdcard/Android/data/com.unknownworlds.subnautica/files/SavedGames"
#endif
constexpr const char* kSavesRoot = SNMOD_WORLD_TEST_ROOT;

constexpr const char* kWorldFileNames[kWorldFileCount] = {
    "gameinfo.json",
    "global-objects.bin",
    "scene-objects.bin",
    "screenshot.jpg",
    "snmod-cells.bin",
};

// A slot file is rejected rather than truncated if it grows past this. scene-objects.bin is
// 24 kB on a young save, so the cap only trips on something already wrong.
constexpr size_t kMaxFileBytes = 64u * 1024u * 1024u;

WorldPayload g_payload;

// Receive side. Only the pump thread touches these, so unlike the capture above they need no
// lock: the game thread never reads or writes them.
WorldManifest g_manifest;
uint8_t* g_recv[kWorldFileCount] = {nullptr};
uint32_t g_recv_size[kWorldFileCount] = {0};
uint32_t g_recv_filled[kWorldFileCount] = {0};
// One flag per chunk, recording that the chunk actually arrived. g_recv_filled is only a
// high-water mark: a chunk that overtakes an earlier one pushes it forward and leaves a hole,
// which a high-water mark cannot express. Anything that decides whether the transfer is done
// has to ask for the contiguous run instead, or it will call a file complete while bytes are
// still missing. The checksum would catch that, but only after declaring victory.
uint8_t* g_recv_seen[kWorldFileCount] = {nullptr};
uint32_t g_recv_chunks[kWorldFileCount] = {0};
bool g_recv_active = false;

void release_locked() {
    for (int i = 0; i < kWorldFileCount; i++) {
        free(g_payload.files[i].data);
        memset(&g_payload.files[i], 0, sizeof(g_payload.files[i]));
    }
    memset(&g_payload, 0, sizeof(g_payload));
}

void release_receive() {
    for (int i = 0; i < kWorldFileCount; i++) {
        free(g_recv[i]);
        free(g_recv_seen[i]);
        g_recv[i] = nullptr;
        g_recv_seen[i] = nullptr;
        g_recv_size[i] = 0;
        g_recv_filled[i] = 0;
        g_recv_chunks[i] = 0;
    }
    memset(&g_manifest, 0, sizeof(g_manifest));
    g_recv_active = false;
}

// Reads a whole file into a freshly allocated buffer. The size comes from fstat rather than
// lseek so a growing save cannot make the read and the allocation disagree.
bool read_whole_file(const char* path, uint8_t** out_data, uint32_t* out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        LOG_WARN("world: open %s failed", path);
        return false;
    }

    struct stat info;
    if (fstat(fd, &info) != 0 || info.st_size <= 0 ||
        static_cast<size_t>(info.st_size) > kMaxFileBytes) {
        LOG_WARN("world: %s has an unusable size", path);
        close(fd);
        return false;
    }

    size_t expected = static_cast<size_t>(info.st_size);
    uint8_t* buffer = static_cast<uint8_t*>(malloc(expected));
    if (!buffer) {
        LOG_ERROR("world: out of memory reading %s (%zu bytes)", path, expected);
        close(fd);
        return false;
    }

    size_t total = 0;
    while (total < expected) {
        ssize_t got = read(fd, buffer + total, expected - total);
        if (got <= 0) break;
        total += static_cast<size_t>(got);
    }
    close(fd);

    if (total != expected) {
        LOG_WARN("world: short read on %s (%zu of %zu)", path, total, expected);
        free(buffer);
        return false;
    }

    *out_data = buffer;
    *out_size = static_cast<uint32_t>(total);
    return true;
}

// The first bytes of a file, as hex. Enough to tell a real protobuf payload from an HTML
// error page or a zeroed buffer without dumping a binary blob into the log.
void log_prefix(const char* label, const uint8_t* data, uint32_t size) {
    char hex[64] = {0};
    size_t used = 0;
    uint32_t shown = size < 12u ? size : 12u;
    for (uint32_t i = 0; i < shown && used + 3 < sizeof(hex); i++) {
        used += static_cast<size_t>(snprintf(hex + used, sizeof(hex) - used, "%02x ", data[i]));
    }
    LOG_INFO("world: %s first bytes: %s", label, hex);
}

#include "world_cells.inc"

int hex_value(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_session_guid(const uint8_t* data, size_t size, uint8_t out[16]) {
    static const char key[] = "\"session\"";
    if (!data || !out || size < sizeof(key) - 1) return false;
    for (size_t i = 0; i + sizeof(key) - 1 <= size; ++i) {
        if (memcmp(data + i, key, sizeof(key) - 1) != 0) continue;
        size_t p = i + sizeof(key) - 1;
        while (p < size && (data[p] == ' ' || data[p] == '\t' || data[p] == '\r' ||
                            data[p] == '\n')) ++p;
        if (p >= size || data[p++] != ':') continue;
        while (p < size && (data[p] == ' ' || data[p] == '\t' || data[p] == '\r' ||
                            data[p] == '\n')) ++p;
        if (p >= size || data[p++] != '"') continue;
        uint8_t parsed[16]{};
        int nibble = 0;
        bool valid = true;
        for (int n = 0; n < 36; ++n) {
            uint8_t c = p < size ? data[p++] : 0;
            bool hyphen = n == 8 || n == 13 || n == 18 || n == 23;
            if (hyphen) {
                if (c != '-') valid = false;
                continue;
            }
            int value = hex_value(c);
            if (value < 0) {
                valid = false;
                continue;
            }
            if ((nibble & 1) == 0) parsed[nibble / 2] = static_cast<uint8_t>(value << 4);
            else parsed[nibble / 2] |= static_cast<uint8_t>(value);
            ++nibble;
        }
        if (!valid || nibble != 32 || p >= size || data[p] != '"') continue;
        memcpy(out, parsed, sizeof(parsed));
        return true;
    }
    return false;
}

bool slot_name_is_plain(const char* slot);

bool slot_name_is_plain(const char* slot) {
    if (!slot || !*slot) return false;
    for (const char* c = slot; *c; c++) {
        bool allowed = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                       (*c >= '0' && *c <= '9') || *c == '-' || *c == '_';
        if (!allowed) return false;
    }
    return true;
}

}  // namespace

uint32_t world_checksum(const void* data, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

// Scans machine-written gameinfo.json for one "key":value pair. Only the values the menu
// load path needs are read back; the file is small and written by one serializer, so a
// targeted scan is honest here. Numeric and quoted values both work - JsonUtility writes
// enums as integers, but accepting the name too costs nothing.
bool world_json_find_value(const uint8_t* data, size_t size, const char* key,
                           char* out, size_t capacity, bool* out_was_number, int* out_number) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    size_t key_len = strlen(pattern);
    if (size < key_len) return false;
    for (size_t i = 0; i + key_len <= size; ++i) {
        if (memcmp(data + i, pattern, key_len) != 0) continue;
        size_t p = i + key_len;
        while (p < size && (data[p] == ' ' || data[p] == '\t' || data[p] == '\r' ||
                            data[p] == '\n')) ++p;
        if (p >= size || data[p++] != ':') continue;
        while (p < size && (data[p] == ' ' || data[p] == '\t' || data[p] == '\r' ||
                            data[p] == '\n')) ++p;
        if (p >= size) return false;

        if (data[p] == '"') {
            ++p;
            size_t start = p;
            while (p < size && data[p] != '"') ++p;
            if (p >= size) return false;
            size_t len = p - start;
            if (len + 1 > capacity) return false;
            memcpy(out, data + start, len);
            out[len] = '\0';
            if (out_was_number) *out_was_number = false;
            return true;
        }
        if (data[p] == '-' || (data[p] >= '0' && data[p] <= '9')) {
            bool negative = data[p] == '-';
            size_t start = p;
            if (negative) ++p;
            while (p < size && data[p] >= '0' && data[p] <= '9') ++p;
            if (p == start || (p - start) > 9) return false;
            int value = 0;
            for (size_t d = negative ? start + 1 : start; d < p; ++d) {
                value = value * 10 + (data[d] - '0');
            }
            if (out) snprintf(out, capacity, "%d", negative ? -value : value);
            if (out_was_number) *out_was_number = true;
            if (out_number) *out_number = negative ? -value : value;
            return true;
        }
        return false;
    }
    return false;
}

bool world_read_load_fields(const char* slot, WorldLoadFields* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!slot_name_is_plain(slot)) return false;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s/gameinfo.json", kSavesRoot, slot);
    uint8_t* data = nullptr;
    uint32_t size = 0;
    if (!read_whole_file(path, &data, &size)) return false;

    char mode_text[32] = {0};
    bool mode_is_number = false;
    int mode_number = 0;
    bool ok = world_json_find_value(data, size, "session", out->session, sizeof(out->session),
                                    nullptr, nullptr) &&
              strlen(out->session) == 36 &&
              world_json_find_value(data, size, "changeSet", nullptr, 0, nullptr,
                                    &out->change_set) &&
              world_json_find_value(data, size, "gameMode", mode_text, sizeof(mode_text),
                                    &mode_is_number, &mode_number);
    free(data);
    if (!ok) return false;

    // GameMode: dump.cs 36101 - Survival 0, Freedom 1, Hardcore 2, Creative 3.
    if (mode_is_number) {
        out->game_mode = mode_number;
    } else if (strcmp(mode_text, "Survival") == 0) {
        out->game_mode = 0;
    } else if (strcmp(mode_text, "Freedom") == 0) {
        out->game_mode = 1;
    } else if (strcmp(mode_text, "Hardcore") == 0) {
        out->game_mode = 2;
    } else if (strcmp(mode_text, "Creative") == 0) {
        out->game_mode = 3;
    } else {
        LOG_WARN("world: gameinfo.json gameMode '%s' is not a known value", mode_text);
        return false;
    }

    out->valid = true;
    return true;
}

size_t world_capture(const char* slot) {
    release_locked();
    if (!world_disk_capture(slot,&g_payload)) return 0;
    return g_payload.total_bytes;
}

// The pump-thread capture. world_capture() stages into the shared g_payload because the
// game thread drives it; this variant fills caller memory so the transport can rebuild an
// offer at the moment a guest asks for one. A torn file fails naturally here: a short read
// is rejected outright, which is the honest reaction to catching the game mid-autosave.
bool world_disk_capture(const char* slot, WorldPayload* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!slot_name_is_plain(slot)) {
        LOG_WARN("world: slot '%s' is not servable from disk", slot ? slot : "(null)");
        return false;
    }
    if (strlen(slot) >= 24) {
        LOG_WARN("world: slot name is too long for the transfer manifest");
        return false;
    }

    std::vector<WorldDiskStamp> before,after;
    if(!world_disk_stamps(slot,&before)) return false;
    char path[512];
    size_t total = 0;
    bool ok = true;
    for (int i = 0; i < kWorldFileCount && ok; i++) {
        snprintf(path, sizeof(path), "%s/%s/%s", kSavesRoot, slot, kWorldFileNames[i]);
        uint8_t* data = nullptr;
        uint32_t size = 0;
        if (!(i==4 ? world_capture_cells(slot,&data,&size) : read_whole_file(path, &data, &size))) {
            ok = false;
            break;
        }
        WorldFile* file = &out->files[i];
        snprintf(file->name, sizeof(file->name), "%s", kWorldFileNames[i]);
        file->size = size;
        file->checksum = world_checksum(data, size);
        file->data = data;
        total += size;
    }

    if (ok) ok = total<=kWorldMaxTransferBytes && world_disk_stamps(slot,&after) && before==after;
    if (ok)
        ok = parse_session_guid(out->files[0].data, out->files[0].size, out->content_guid);

    if (!ok) {
        for (int i = 0; i < kWorldFileCount; i++) {
            free(out->files[i].data);
            memset(&out->files[i], 0, sizeof(out->files[i]));
        }
        memset(out, 0, sizeof(*out));
        return false;
    }

    out->valid = true;
    out->file_count = kWorldFileCount;
    out->total_bytes = total;
    out->content_guid_valid = true;
    LOG_INFO("world: disk capture of %s ready, %zu bytes across %d files", slot, total,
             kWorldFileCount);
    return true;
}

// Availability probe without a transfer-sized read. Four open/fstat pairs plus one small
// read for the GUID: ticked from the pump at JOIN/request time, so it must stay cheap.
bool world_disk_ready(const char* slot) {
    if (!slot_name_is_plain(slot)) return false;

    std::vector<WorldDiskStamp> stamps;
    if(!world_disk_stamps(slot,&stamps)) return false;
    char path[512];
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof(path), "%s/%s/%s", kSavesRoot, slot, kWorldFileNames[i]);
        int fd = open(path, O_RDONLY);
        if (fd < 0) return false;
        struct stat info;
        bool usable = fstat(fd, &info) == 0 && info.st_size > 0 &&
                      static_cast<size_t>(info.st_size) <= kMaxFileBytes;
        close(fd);
        if (!usable) return false;
    }

    // The size probe alone would pass a half-written gameinfo.json; the session GUID is the
    // cheap fingerprint that says the file actually reached the end of its write.
    uint8_t* data = nullptr;
    uint32_t size = 0;
    snprintf(path, sizeof(path), "%s/%s/gameinfo.json", kSavesRoot, slot);
    bool has_guid = read_whole_file(path, &data, &size);
    if (has_guid) {
        uint8_t guid[16];
        has_guid = parse_session_guid(data, size, guid);
        free(data);
    }
    return has_guid;
}

// Symmetric with release_locked(): frees a caller-owned continuation of the WorldPayload
// ownership model. Only callers that received ownership (world_disk_capture) use this.
void world_payload_free(WorldPayload* out) {
    if (!out) return;
    for (int i = 0; i < kWorldFileCount; i++) {
        free(out->files[i].data);
        memset(&out->files[i], 0, sizeof(out->files[i]));
    }
    memset(out, 0, sizeof(*out));
}

const WorldPayload* world_payload() { return g_payload.valid ? &g_payload : nullptr; }

void world_release() { release_locked(); }

// ---------------------------------------------------------------------------
// Receiving side
// ---------------------------------------------------------------------------

bool world_begin_receive(const WorldManifest& manifest) {
    release_receive();

    if (!slot_name_is_plain(manifest.slot)) {
        LOG_WARN("world: manifest has an invalid slot name");
        return false;
    }
    if (manifest.file_count != kWorldFileCount) {
        LOG_WARN("world: manifest declares %d files, expected %d", manifest.file_count,
                 kWorldFileCount);
        return false;
    }

    size_t total = 0;
    bool seen[kWorldFileCount] = {};
    for (int i = 0; i < kWorldFileCount; i++) {
        if (manifest.files[i].size == 0 || manifest.files[i].size > kMaxFileBytes) {
            LOG_WARN("world: manifest file %d (%s) has an unusable size %u", i,
                     manifest.files[i].name, manifest.files[i].size);
            return false;
        }
        // The name arrives over the network and ends up as a file name, so it is checked
        // against the fixed set rather than trusted.
        int known = -1;
        for (int k = 0; k < kWorldFileCount; k++) {
            if (strcmp(manifest.files[i].name, kWorldFileNames[k]) == 0) known = k;
        }
        if (known < 0 || seen[known]) {
            LOG_WARN("world: manifest file %d has unexpected name '%s'", i,
                     manifest.files[i].name);
            return false;
        }
        seen[known] = true;
        if ((known<4 && manifest.files[i].size>8u*1024u*1024u) ||
            total>kWorldMaxTransferBytes-manifest.files[i].size) return false;
        total += manifest.files[i].size;
    }

    g_manifest = manifest;
    g_manifest.total_bytes = static_cast<uint32_t>(total);
    for (int i = 0; i < kWorldFileCount; i++) {
        g_recv[i] = static_cast<uint8_t*>(malloc(manifest.files[i].size));
        if (!g_recv[i]) {
            LOG_ERROR("world: out of memory receiving %s (%u bytes)", manifest.files[i].name,
                      manifest.files[i].size);
            release_receive();
            return false;
        }
        g_recv_size[i] = manifest.files[i].size;

        g_recv_chunks[i] = chunks_for(manifest.files[i].size);
        g_recv_seen[i] = static_cast<uint8_t*>(calloc(g_recv_chunks[i], 1));
        if (!g_recv_seen[i]) {
            LOG_ERROR("world: out of memory tracking chunks for %s", manifest.files[i].name);
            release_receive();
            return false;
        }
    }

    g_recv_active = true;
    LOG_INFO("world: receive buffers ready for slot %s, %u bytes across %d files",
             g_manifest.slot, g_manifest.total_bytes, kWorldFileCount);
    return true;
}

bool world_receive_chunk(int file_index, uint32_t offset, const void* data, size_t size) {
    if (!g_recv_active || file_index < 0 || file_index >= kWorldFileCount) return false;
    if (size == 0) return false;
    // One byte past the end is the only way a chunk can be wrong here: a chunk that starts
    // inside the file and runs over the end is a desync, not something to clamp.
    if (static_cast<size_t>(offset) + size > g_recv_size[file_index]) {
        LOG_WARN("world: chunk for file %d at %u (+%zu) runs past the %u byte file",
                 file_index, offset, size, g_recv_size[file_index]);
        return false;
    }

    memcpy(g_recv[file_index] + offset, data, size);

    // Chunks are always whole-chunk aligned, so the chunk index is exact and a repeat is
    // simply re-marked. Recording it is what lets world_receive_contiguous tell a hole apart
    // from a full file.
    uint32_t chunk = offset / kWorldChunkPayload;
    if (chunk < g_recv_chunks[file_index]) g_recv_seen[file_index][chunk] = 1;

    uint32_t end = offset + static_cast<uint32_t>(size);
    if (end > g_recv_filled[file_index]) g_recv_filled[file_index] = end;
    return true;
}

// How many chunks form an unbroken run from the start of the file. This, not g_recv_filled, is
// what "how much of this file do I actually have" means.
uint32_t world_receive_contiguous(int file_index) {
    if (!g_recv_active || file_index < 0 || file_index >= kWorldFileCount) return 0;
    uint32_t n = 0;
    while (n < g_recv_chunks[file_index] && g_recv_seen[file_index][n]) n++;
    return n;
}

bool world_receive_verify(const WorldManifest& manifest) {
    if (!g_recv_active) return false;

    bool all_ok = true;
    int bad = 0;
    for (int i = 0; i < kWorldFileCount; i++) {
        // Completeness is judged on the contiguous run. g_recv_filled reaches the file size
        // as soon as the last chunk lands, even with a hole in the middle.
        uint32_t have = world_receive_contiguous(i);
        if (have != g_recv_chunks[i]) {
            LOG_WARN("world: %s incomplete, %u of %u chunks contiguous (%u byte high-water mark)",
                     manifest.files[i].name, have, g_recv_chunks[i], g_recv_filled[i]);
            all_ok = false;
            bad++;
            continue;
        }
        uint32_t actual = world_checksum(g_recv[i], g_recv_size[i]);
        if (actual != manifest.files[i].checksum) {
            LOG_ERROR("world: %s checksum mismatch, got %08x expected %08x",
                      manifest.files[i].name, actual, manifest.files[i].checksum);
            all_ok = false;
            bad++;
            continue;
        }
        LOG_INFO("world: verified %-18s %6u bytes  fnv1a=%08x", manifest.files[i].name,
                 g_recv_size[i], actual);
    }

    if (!all_ok) {
        LOG_ERROR("world: transfer rejected, %d of %d files bad", bad, kWorldFileCount);
        release_receive();
        return false;
    }
    return true;
}

bool world_receive_content_guid(uint8_t out[16]) {
    if (!g_recv_active || !out) return false;
    for (int i = 0; i < kWorldFileCount; ++i) {
        if (strcmp(g_manifest.files[i].name, "gameinfo.json") != 0) continue;
        return parse_session_guid(g_recv[i], g_recv_size[i], out);
    }
    return false;
}

bool world_slot_is_host_received(const char* slot) {
    if (!slot_name_is_plain(slot)) return false;
    char path[560];
    snprintf(path, sizeof(path), "%s/%s/from_host.txt", kSavesRoot, slot);
    struct stat info;
    if (stat(path, &info) != 0) return false;
    if (!S_ISREG(info.st_mode)) return false;
    // A marker of junk size would mean someone dropped something else at this path;
    // peer-written markers are 14 bytes.
    return info.st_size > 0 && info.st_size < 256;
}

WorldSlotCheck world_slot_exists(const char* slot, const WorldManifest& manifest) {
    if (!slot_name_is_plain(slot) || manifest.file_count != kWorldFileCount) {
        LOG_ERROR("world: slot check rejected invalid name/manifest");
        return kWorldSlotCheckError;
    }
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/%s", kSavesRoot, slot);
    struct stat existing;
    if (stat(dir, &existing) != 0) {
        if (errno == ENOENT) return kWorldSlotMissing;
        LOG_ERROR("world: stat slot %s failed: %s", slot, strerror(errno));
        return kWorldSlotCheckError;
    }
    if (!S_ISDIR(existing.st_mode)) {
        LOG_ERROR("world: slot path %s exists but is not a directory", slot);
        return kWorldSlotConflict;
    }

    bool seen[kWorldFileCount] = {};
    for (int i = 0; i < kWorldFileCount; ++i) {
        int known = -1;
        for (int j = 0; j < kWorldFileCount; ++j) {
            if (strcmp(manifest.files[i].name, kWorldFileNames[j]) == 0) {
                known = j;
                break;
            }
        }
        if (known < 0 || seen[known]) {
            LOG_ERROR("world: slot %s manifest has unknown/duplicate file '%s'", slot,
                      manifest.files[i].name);
            return kWorldSlotCheckError;
        }
        seen[known] = true;
        char path[560];
        snprintf(path, sizeof(path), "%s/%s/%s", kSavesRoot, slot, manifest.files[i].name);
        uint8_t* data = nullptr;
        uint32_t size = 0;
        if (!(known==4 ? world_capture_cells(slot,&data,&size) : read_whole_file(path, &data, &size))) {
            LOG_ERROR("world: existing slot %s cannot be verified (%s)", slot,
                      manifest.files[i].name);
            return kWorldSlotConflict;
        }
        uint32_t checksum = world_checksum(data, size);
        free(data);
        if (size != manifest.files[i].size || checksum != manifest.files[i].checksum) {
            LOG_ERROR("world: existing slot %s conflicts at %s (size %u/%u checksum %08x/%08x)",
                      slot, manifest.files[i].name, size, manifest.files[i].size, checksum,
                      manifest.files[i].checksum);
            return kWorldSlotConflict;
        }
    }
    return kWorldSlotMatches;
}

// Removes a staging directory this process created. Best effort: a leaked staging dir
// is cosmetic, not fatal, and the game never looks at the dot-prefixed name.
static bool remove_tree_best_effort(const char* path);
static void remove_dir_best_effort(const char* path) { remove_tree_best_effort(path); }

// Remove a private backup tree created during an authorized host-slot refresh.
// Use lstat and unlink symlinks rather than traversing them.
static bool remove_tree_best_effort(const char* path) {
    struct stat info{};
    if (lstat(path, &info) != 0) return errno == ENOENT;
    if (!S_ISDIR(info.st_mode)) return unlink(path) == 0;
    DIR* dir = opendir(path);
    if (!dir) return false;
    bool ok = true;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[600];
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (!remove_tree_best_effort(child)) ok = false;
    }
    closedir(dir);
    return ok && rmdir(path) == 0;
}

bool world_write_slot(const WorldManifest& manifest,bool preserve_local) {
    if (!g_recv_active || manifest.file_count!=g_manifest.file_count ||
        strcmp(manifest.slot,g_manifest.slot)) return false;
    for(int i=0;i<kWorldFileCount;++i) if(strcmp(manifest.files[i].name,g_manifest.files[i].name) ||
        manifest.files[i].size!=g_manifest.files[i].size || manifest.files[i].checksum!=g_manifest.files[i].checksum) return false;
    if(!world_receive_verify(manifest)) return false;
    if(mkdir(kSavesRoot,0770)!=0 && errno!=EEXIST) return false;
    if (!slot_name_is_plain(manifest.slot)) {
        LOG_ERROR("world: refusing to write slot '%s', name is not plain", manifest.slot);
        return false;
    }

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/%s", kSavesRoot, manifest.slot);

    // A local slot is never overwritten. A peer-received slot may be refreshed by the
    // host-authorized conflict path (net.cpp checks this same marker before calling us).
    char backup[560];
    snprintf(backup, sizeof(backup), "%s/.snmod_backup_%s", kSavesRoot, manifest.slot);
    struct stat existing;
    bool existing_slot = stat(dir, &existing) == 0;
    if (!existing_slot && errno != ENOENT) {
        LOG_ERROR("world: stat slot %s failed: %s", manifest.slot, strerror(errno));
        return false;
    }
    if (!existing_slot) {
        // Recover from an interruption between moving the old directory aside and
        // committing the fully staged replacement.
        struct stat old_backup{};
        if (stat(backup, &old_backup) == 0) {
            if (rename(backup, dir) != 0) {
                LOG_ERROR("world: restoring interrupted slot replacement %s failed: %s",
                          manifest.slot, strerror(errno));
                return false;
            }
            existing = old_backup;
            existing_slot = true;
            LOG_WARN("world: restored previous host-received slot %s after interrupted refresh",
                     manifest.slot);
        }
    }
    const bool replace_host_received = existing_slot && S_ISDIR(existing.st_mode) &&
                                       (world_slot_is_host_received(manifest.slot) || preserve_local);
    if (existing_slot && !replace_host_received) {
        LOG_ERROR("world: slot %s already exists without host-received overwrite authority",
                  manifest.slot);
        return false;
    }

    // Transactional write: everything lands in a staging directory first and the final
    // name appears only through one atomic rename of the whole directory. A failure at
    // any point (ENOSPC, disconnect, crash) leaves the saves root without a partial slot
    // - the previous behaviour created the final directory up front, so a failure after
    // the first file produced a visible half slot that the game could pick up.
    //
    // The staging name embeds the (plain-validated) slot name, so a stale directory left
    // by an interrupted run is unambiguous and safe to sweep here: this process owns the
    // prefix, the game never creates it.
    char staging[560];
    snprintf(staging, sizeof(staging), "%s/.snmod_stage_%s", kSavesRoot, manifest.slot);
    bool moved_old_slot = false;

    // Sweep leftovers from a previous interrupted attempt. Only our own dot-prefix is
    // removed, and slot is plain-validated above, so this cannot escape the saves root.
    DIR* saves = opendir(kSavesRoot);
    if (saves) {
        struct dirent* entry;
        while ((entry = readdir(saves)) != nullptr) {
            if (strncmp(entry->d_name, ".snmod_stage_", 13) != 0) continue;
            char stale[560];
            snprintf(stale, sizeof(stale), "%s/%s", kSavesRoot, entry->d_name);
            LOG_WARN("world: removing stale staging dir %s", entry->d_name);
            remove_dir_best_effort(stale);
        }
        closedir(saves);
    }

    if (mkdir(staging, 0770) != 0 && errno != EEXIST) {
        LOG_ERROR("world: mkdir staging %s failed: %s", staging, strerror(errno));
        return false;
    }

    for (int i = 0; i < kWorldFileCount; i++) {
        if(!strcmp(manifest.files[i].name,"snmod-cells.bin")) {
            if(!world_extract_cells(staging,g_recv[i],g_recv_size[i])) goto fail;
            continue;
        }
        char staged_path[560];
        char temp_path[600];
        snprintf(staged_path, sizeof(staged_path), "%s/%s", staging, manifest.files[i].name);
        snprintf(temp_path, sizeof(temp_path), "%s.part", staged_path);

        int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0660);
        if (fd < 0) {
            LOG_ERROR("world: open %s failed: %s", temp_path, strerror(errno));
            goto fail;
        }

        uint32_t written = 0;
        while (written < g_recv_size[i]) {
            ssize_t put = write(fd, g_recv[i] + written, g_recv_size[i] - written);
            if (put <= 0) {
                LOG_ERROR("world: write %s failed after %u bytes: %s", temp_path, written,
                          strerror(errno));
                close(fd);
                goto fail;
            }
            written += static_cast<uint32_t>(put);
        }
        // The data has to be on the platter before the name appears, or a crash can leave a
        // complete-looking slot backed by a half-written file.
        if (fsync(fd) != 0) { close(fd); goto fail; }
        close(fd);

        // Per-file rename inside staging: a file is either complete or not there at all.
        if (rename(temp_path, staged_path) != 0) {
            LOG_ERROR("world: rename %s failed: %s", temp_path, strerror(errno));
            goto fail;
        }
        LOG_INFO("world: staged %s/%s (%u bytes)", manifest.slot, manifest.files[i].name,
                 g_recv_size[i]);
    }

    // Marker is part of the staged transaction, before the final slot is visible.
    {
        char marker[640];snprintf(marker,sizeof(marker),"%s/from_host.txt",staging);
        int fd=open(marker,O_WRONLY|O_CREAT|O_EXCL,0660);if(fd<0)goto fail;
        bool ok=write(fd,"host transfer\n",14)==14 && fsync(fd)==0;close(fd);
        if(!ok)goto fail;
    }

    // Commit a new slot with one rename. For an authorized refresh, retain the old
    // complete peer-received slot as a hidden rollback copy until the staged version
    // has been committed.
    if (replace_host_received) {
        // Keep only one recoverable prior copy. This path is private to the mod and
        // hidden from the game's plain slot names.
        struct stat prior{};
        if(lstat(backup,&prior)==0) {
            char marker[640];snprintf(marker,sizeof(marker),"%s/from_host.txt",backup);
            struct stat marker_info{};
            const bool peer_backup=lstat(marker,&marker_info)==0 && S_ISREG(marker_info.st_mode) &&
                marker_info.st_size>0 && marker_info.st_size<256;
            if(!peer_backup) {
                // A role switch can replace a former host's native save. Its prior
                // directory must survive all later ordinary guest refreshes too.
                char kept[640];bool moved=false;
                static unsigned counter=0;
                for(unsigned attempt=0;attempt<32;++attempt) {
                    snprintf(kept,sizeof(kept),"%s/.snmod_kept_%s_%lld_%d_%u",kSavesRoot,manifest.slot,
                        static_cast<long long>(time(nullptr)),int(getpid()),++counter);
                    struct stat occupied{};
                    if(lstat(kept,&occupied)==0)continue;
                    if(errno!=ENOENT)break;
                    if(rename(backup,kept)==0){moved=true;break;}
                }
                if(!moved){LOG_ERROR("world: could not preserve the previous native save backup");goto fail;}
                LOG_INFO("world: native save backup retained at %s",kept);
            } else if(!remove_tree_best_effort(backup)) {
                LOG_ERROR("world: cannot clear prior refresh backup for %s",manifest.slot);goto fail;
            }
        }
        if (rename(dir, backup) != 0) {
            LOG_ERROR("world: backing up old host-received slot %s failed: %s", manifest.slot,
                      strerror(errno));
            goto fail;
        }
        moved_old_slot = true;
    }
    if (rename(staging, dir) != 0) {
        LOG_ERROR("world: commit rename %s -> %s failed: %s", staging, dir, strerror(errno));
        if (moved_old_slot && rename(backup, dir) != 0) {
            LOG_ERROR("world: rollback restoring old slot %s failed: %s", manifest.slot,
                      strerror(errno));
        }
        goto fail;
    }
    if (moved_old_slot) {
        LOG_INFO("world: refreshed host-received slot %s; previous version retained at %s",
                 manifest.slot, backup);
    }

    release_receive();
    return true;

fail:
    // Remove only the staging directory this call created. The final slot name was never
    // touched, so a failed transfer cannot damage anything the game already sees.
    remove_dir_best_effort(staging);
    return false;
}

const WorldManifest* world_receive_manifest() { return g_recv_active ? &g_manifest : nullptr; }

uint8_t* world_receive_file_data(int file_index, uint32_t size) {
    if (!g_recv_active || file_index < 0 || file_index >= kWorldFileCount) return nullptr;
    if (g_recv_size[file_index] != size) return nullptr;
    return g_recv[file_index];
}

uint32_t world_receive_received(int file_index) {
    if (file_index < 0 || file_index >= kWorldFileCount) return 0;
    return g_recv_filled[file_index];
}

void world_reset_receive() { release_receive(); }
