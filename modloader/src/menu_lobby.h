#pragma once

#include <cstdint>

namespace menu_lobby {

constexpr uint8_t kReady = 1;
constexpr uint8_t kStarted = 2;

struct Presence {
    uint32_t room;
    uint32_t host;
    uint32_t guest;
    uint8_t flags;
    uint8_t reserved[3];
};

static_assert(sizeof(Presence) == 16, "lobby advertisement wire size");

inline bool valid(const Presence& value, uint32_t sender) {
    if (value.reserved[0] || value.reserved[1] || value.reserved[2]) return false;
    if (!value.room) return !value.host && !value.guest && !value.flags;
    if (!sender || !value.host || value.guest == value.host ||
        (value.flags & ~(kReady | kStarted))) return false;
    if (sender == value.host) {
        return !(value.flags & kStarted) || (value.guest && (value.flags & kReady));
    }
    return value.guest == sender && !(value.flags & kStarted);
}

inline bool same_room(const Presence& a, const Presence& b) {
    return a.room && a.room == b.room && a.host == b.host;
}

inline bool can_start(const Presence& host, const Presence& guest) {
    return same_room(host, guest) && host.guest && host.guest == guest.guest &&
        host.flags == kReady && guest.flags == kReady;
}

inline bool can_join_world(const Presence& host, const Presence& guest, uint32_t sender) {
    return same_room(host, guest) && host.guest && host.guest == sender &&
        guest.guest == sender && (host.flags & kStarted);
}

}  // namespace menu_lobby
