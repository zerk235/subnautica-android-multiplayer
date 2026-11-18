#pragma once

#include "net.h"

// ArmsController's surface rule uses a band of one metre around the local water level.
inline bool avatar_near_water_surface(float player_y, float water_level) {
    return player_y > water_level - 1.0f && player_y < water_level + 1.0f;
}

struct AvatarLocomotion {
    bool fast_swim;
    bool underwater;
    bool on_surface;
};

inline AvatarLocomotion read_peer_locomotion(const PeerState& peer) {
    const bool swimming = (peer.flags & kFlagSwimming) != 0;
    return {
        swimming && (peer.flags & kFlagFastSwimming) != 0,
        peer.anim_float_count > 0 ? peer.anim_underwater != 0
                                  : (peer.flags & kFlagUnderwater) != 0,
        // Older peers set this bit for walking outdoors as well.
        swimming && (peer.flags & kFlagOnSurface) != 0,
    };
}
