#include "modloader/src/avatar_motion.h"

#include <cassert>
#include <cstdio>
#include <limits>

namespace {

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001f;
}

void check_motion() {
    AvatarMotion motion{};
    PeerState peer{};
    AvatarPose pose{0.0f, 0.0f, 0.0f, 359.0f, 179.0f, 355.0f};
    auto receive = [&](double now) {
        ++peer.sequence;
        peer.pose_received_at = now;
        return motion.retarget(peer, pose, now);
    };
    assert(receive(10.0));
    assert(motion.sample(10.0).x == 0.0f);

    pose = {0.9f, 0.3f, -0.6f, 1.0f, 181.0f, 5.0f};
    assert(!receive(10.05));
    const auto middle = motion.sample(10.05 + AvatarMotion::kBlendSeconds / 2);
    assert(near(middle.x, 0.45f) && near(middle.y, 0.15f) && near(middle.z, -0.3f));
    assert(near(middle.yaw, 0.0f) && near(std::fabs(middle.view_yaw), 180.0f));
    assert(near(middle.pitch, 0.0f));

    // An early packet retargets from the current pose, including during a turn.
    const double early = 10.09;
    const auto before_retarget = motion.sample(early);
    pose.x = 1.1f;
    assert(!receive(early));
    const auto after_retarget = motion.sample(early);
    assert(near(before_retarget.x, after_retarget.x) && near(before_retarget.yaw, after_retarget.yaw));
    assert(motion.sample(10.11).x > after_retarget.x);

    // A duplicate or a new packet with an identical pose leaves the original
    // blend deadline in place, including when the peer comes to a stop.
    assert(!motion.retarget(peer, pose, 10.12));
    assert(!receive(10.14));
    assert(motion.sample(10.17) == pose);
    assert(motion.sample(10.35) == pose);

    // A short packet gap can hold position, and recovery still starts smoothly.
    pose.x = 1.7f;
    assert(!receive(10.36));
    assert(near(motion.sample(10.36).x, 1.1f));
    assert(motion.sample(10.5) == pose);

    peer.flags = kFlagInside;
    pose.x = 2.1f;
    assert(receive(10.51) && motion.sample(10.51) == pose);
    peer.environment_id[0] = 1;
    pose.x = 2.4f;
    assert(receive(10.56) && motion.sample(10.56) == pose);
    peer.flags |= kFlagPiloting;
    pose.x = 2.7f;
    assert(receive(10.61) && motion.sample(10.61) == pose);
    peer.flags &= ~kFlagPiloting;
    assert(receive(10.66) && motion.sample(10.66) == pose);
    pose.x = 40.0f;
    assert(receive(10.71) && motion.sample(10.71) == pose);
    pose.x = 40.4f;
    assert(receive(11.2) && motion.sample(11.2) == pose);

    pose.x = std::numeric_limits<float>::quiet_NaN();
    assert(!pose.valid());
}

} // namespace

int main() {
    check_motion();
    puts("avatar motion: continuous retargeting, angle wrapping, packet gaps and snap boundaries passed");
}
