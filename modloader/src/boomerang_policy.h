#pragma once
#include <cmath>

namespace boomerang_sync {
constexpr float kRange = 32;
constexpr float kReleaseRange = 48;
constexpr double kHideAfter = 2;

// Missing authority data is a visibility decision, never an AI handover.
inline bool hidden(bool has_pose, double age) {
    return !has_pose || !std::isfinite(age) || age < 0 || age > kHideAfter;
}
inline bool outside_domain(float distance_squared) {
    return std::isfinite(distance_squared) && distance_squared > kReleaseRange * kReleaseRange;
}
// Network discovery/admission may temporarily disappear while the world remains
// loaded. Only explicit world/role/context changes restore native simulation.
inline bool restore_context(bool world_changed, bool role_changed, bool diagnostic) {
    return world_changed || role_changed || diagnostic;
}
}
