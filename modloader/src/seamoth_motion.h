#pragma once

#include "net.h"
#include "pose_interpolation.h"

inline bool vehicle_transform_valid(const VehicleState& state) {
    return std::isfinite(state.x) && std::isfinite(state.y) && std::isfinite(state.z) &&
           std::isfinite(state.yaw) && std::isfinite(state.pitch) && std::isfinite(state.roll);
}

struct SeamothPose {
    float x, y, z;
    HeldRotation rotation;

    static SeamothPose from_vehicle(const VehicleState& state) {
        constexpr float kHalfRadians = 0.00872664626f;
        const float pitch = std::remainder(state.pitch, 360.0f) * kHalfRadians;
        const float yaw = std::remainder(state.yaw, 360.0f) * kHalfRadians;
        const float roll = std::remainder(state.roll, 360.0f) * kHalfRadians;
        const HeldRotation around_x{std::sin(pitch), 0, 0, std::cos(pitch)};
        const HeldRotation around_y{0, std::sin(yaw), 0, std::cos(yaw)};
        const HeldRotation around_z{0, 0, std::sin(roll), std::cos(roll)};
        // The wire carries Unity Euler angles: rotations are applied Z, X, Y.
        return {state.x, state.y, state.z,
                held_rotation_mul(around_y, held_rotation_mul(around_x, around_z))};
    }

    bool operator==(const SeamothPose& other) const {
        return x == other.x && y == other.y && z == other.z &&
               rotation.x == other.rotation.x && rotation.y == other.rotation.y &&
               rotation.z == other.rotation.z && rotation.w == other.rotation.w;
    }

    SeamothPose blend_to(const SeamothPose& target, float t) const {
        const auto a = rotation;
        auto b = target.rotation;
        if (a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w < 0.0f) {
            b = {-b.x, -b.y, -b.z, -b.w};
        }
        HeldRotation blended{
            a.x + t * (b.x - a.x), a.y + t * (b.y - a.y),
            a.z + t * (b.z - a.z), a.w + t * (b.w - a.w),
        };
        HeldRotation normalized = target.rotation;
        held_rotation_normalize(blended, &normalized);
        return {
            x + t * (target.x - x), y + t * (target.y - y), z + t * (target.z - z),
            normalized,
        };
    }
};

// Each driven vehicle slot belongs to exactly one pilot and GUID.
struct SeamothMotion {
    static constexpr double kBlendSeconds = 1.5 * kVehicleInterval;
    static constexpr double kSnapGapSeconds = 0.45;
    static constexpr float kSnapDistance = 6.0f;

    PoseInterpolation<SeamothPose> blend;
    double received_at;
    bool initialized;
    bool settled;

    void retarget(const VehicleState& state, double now) {
        const auto pose = SeamothPose::from_vehicle(state);
        const bool snap = !initialized || state.updated - received_at > kSnapGapSeconds ||
                          pose_jump(blend.target, pose, kSnapDistance);
        if (snap || !(pose == blend.target)) {
            blend.retarget(pose, now, kBlendSeconds, snap);
            settled = false;
        }
        initialized = true;
        received_at = state.updated;
    }

    SeamothPose sample(double now) const { return blend.sample(now, kBlendSeconds); }
    bool finished(double now) const { return blend.finished(now, kBlendSeconds); }
};
