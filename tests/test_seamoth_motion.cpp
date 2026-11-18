#include "modloader/src/seamoth_motion.h"

#include <cassert>
#include <cstdio>
#include <limits>

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001f;
}

int main() {
    SeamothMotion motion{};
    VehicleState state{};
    auto receive = [&](double time) {
        state.updated = time;
        assert(vehicle_transform_valid(state));
        motion.retarget(state, time);
    };

    state.yaw = 179.0f;
    receive(10.0);
    assert(motion.finished(10.0) && motion.sample(10.0).x == 0.0f);

    state.x = 1.0f;
    state.y = -0.3f;
    state.z = 0.4f;
    state.yaw = 181.0f;
    receive(10.10);
    const auto halfway = motion.sample(10.10 + SeamothMotion::kBlendSeconds / 2);
    assert(near(halfway.x, 0.5f) && near(halfway.y, -0.15f) && near(halfway.z, 0.2f));
    assert(near(std::fabs(halfway.rotation.y), 1.0f) && near(halfway.rotation.w, 0.0f));

    const auto before = motion.sample(10.19);
    state.x = 1.7f;
    receive(10.19);
    const auto after = motion.sample(10.19);
    assert(after == before);
    receive(10.28); // unchanged position should not move the deadline
    assert(motion.finished(10.35));
    assert(motion.sample(10.4) == SeamothPose::from_vehicle(state));
    assert(motion.sample(10.5) == SeamothPose::from_vehicle(state));

    // Switching Euler representations around vertical pitch should follow the
    // short physical rotation, even as both yaw and roll jump by 180 degrees.
    SeamothMotion diving{};
    VehicleState down{};
    down.pitch = 89.0f;
    down.updated = 20.0;
    diving.retarget(down, 20.0);
    down.yaw = down.roll = 180.0f;
    down.updated = 20.1;
    diving.retarget(down, 20.1);
    const auto vertical = diving.sample(20.1 + SeamothMotion::kBlendSeconds / 2).rotation;
    assert(near(std::fabs(vertical.x), std::sqrt(0.5f)) &&
           near(std::fabs(vertical.w), std::sqrt(0.5f)));
    assert(near(vertical.y, 0.0f) && near(vertical.z, 0.0f));

    state.x = 25.0f;
    receive(10.51); // teleport
    assert(motion.finished(10.51) && motion.sample(10.51).x == 25.0f);
    state.x = 26.0f;
    receive(11.1); // first fresh packet after a long gap
    assert(motion.finished(11.1) && motion.sample(11.1).x == 26.0f);
    state.pitch = std::numeric_limits<float>::quiet_NaN();
    assert(!vehicle_transform_valid(state));

    puts("Seamoth motion: pose continuity, full rotation, gaps, and teleport snap passed");
}
