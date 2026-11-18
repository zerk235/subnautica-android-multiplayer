#pragma once

#include <cmath>
#include <cstring>

#include "net.h"
#include "pose_interpolation.h"

struct AvatarPose {
    float x, y, z;
    float yaw, view_yaw, pitch;

    bool operator==(const AvatarPose& other) const {
        return x == other.x && y == other.y && z == other.z &&
               yaw == other.yaw && view_yaw == other.view_yaw && pitch == other.pitch;
    }

    bool valid() const {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
               std::isfinite(yaw) && std::isfinite(view_yaw) && std::isfinite(pitch);
    }

    AvatarPose blend_to(const AvatarPose& target, float t) const {
        return {
            x + t * (target.x - x), y + t * (target.y - y), z + t * (target.z - z),
            blend_angle(yaw, target.yaw, t),
            blend_angle(view_yaw, target.view_yaw, t),
            blend_angle(pitch, target.pitch, t),
        };
    }

private:
    static float blend_angle(float from, float to, float t) {
        const float start = std::remainder(from, 360.0f);
        return std::remainder(start + t * std::remainder(to - start, 360.0f), 360.0f);
    }
};

struct AvatarMotion {
    static constexpr double kBlendSeconds = 1.5 * kStateInterval;
    static constexpr double kSnapGapSeconds = 0.35;
    static constexpr float kSnapDistance = 6.0f;

    PoseInterpolation<AvatarPose> blend;
    double received_at;
    uint32_t sequence, flags;
    uint8_t environment_id[16];
    bool initialized;

    AvatarPose sample(double now) const {
        return blend.sample(now, kBlendSeconds);
    }

    // The network already rejects out-of-order packets. Return true for a
    // discontinuity so the renderer can also reset its animation speed sample.
    bool retarget(const PeerState& peer, const AvatarPose& pose, double now) {
        if (initialized && peer.sequence == sequence && peer.pose_received_at == received_at)
            return false;

        const bool snap = !initialized || peer.pose_received_at - received_at > kSnapGapSeconds ||
            ((peer.flags ^ flags) & (kFlagInside | kFlagPiloting)) != 0 ||
            std::memcmp(environment_id, peer.environment_id, sizeof(environment_id)) != 0 ||
            pose_jump(blend.target, pose, kSnapDistance);

        if (snap || !(pose == blend.target)) {
            // Start from the visible trajectory so early and late packets never
            // pull the avatar backwards to a previous sample.
            blend.retarget(pose, now, kBlendSeconds, snap);
        }
        initialized = true;
        received_at = peer.pose_received_at;
        sequence = peer.sequence;
        flags = peer.flags;
        std::memcpy(environment_id, peer.environment_id, sizeof(environment_id));
        return snap;
    }
};
