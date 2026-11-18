#pragma once

#include <algorithm>

template<class Pose>
struct PoseInterpolation {
    Pose from, target;
    double started;

    Pose sample(double now, double duration) const {
        const float t = static_cast<float>(std::clamp((now - started) / duration, 0.0, 1.0));
        if (t <= 0.0f) return from;
        if (t >= 1.0f) return target;
        return from.blend_to(target, t);
    }

    void retarget(const Pose& pose, double now, double duration, bool snap) {
        from = snap ? pose : sample(now, duration);
        target = pose;
        started = now;
    }

    bool finished(double now, double duration) const {
        return from == target || now >= started + duration;
    }
};

template<class Pose>
bool pose_jump(const Pose& from, const Pose& to, float distance) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float dz = to.z - from.z;
    return dx*dx + dy*dy + dz*dz >= distance*distance;
}
