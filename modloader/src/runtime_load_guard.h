#pragma once
#include <cmath>
#include <cstdint>

struct RuntimeLoadGuard {
    static constexpr double kWarmupSeconds = 2.0;
    bool sampled = false, ready = false;
    uint32_t generation = 0;
    double stable_since = -1, last_time = -1;
    bool update(bool world_exists, bool manager_exists, bool loading,
                uint32_t current_generation, double now) {
        if (!std::isfinite(now) || now < 0) { stable_since = -1; ready = false; return false; }
        if (!sampled || generation != current_generation || (last_time >= 0 && now < last_time)) {
            stable_since = -1; ready = false;
        }
        sampled = true; generation = current_generation; last_time = now;
        if (!world_exists || !manager_exists || loading) {
            stable_since = -1; ready = false; return false;
        }
        if (stable_since < 0) stable_since = now;
        ready = now - stable_since >= kWarmupSeconds;
        return ready;
    }
};

// Deadlines count time when the game can actually apply the work, not loading.
struct PausedWorkDeadline {
    double since = -1;
    void pause(double now) { if (since < 0) since = now; }
    void resume(double now, double* deadline) {
        if (since >= 0 && deadline && now > since) *deadline += now - since;
        since = -1;
    }
};
