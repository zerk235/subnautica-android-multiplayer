#include "experiments/creature_sync/replica_model.h"
#include <cassert>
#include <limits>
#include <cstdio>
using namespace creature_sync_draft;
int main() {
    Context context; context.session[0] = 1; context.host = 10; context.epoch = 7;
    Identity fish{"10000000-0000-0000-0000-000000000001", "20000000-0000-0000-0000-000000000001", kBoomerang};
    GuestRegistry guest;
    guest.configure(Role::Guest, context, false);
    assert(guest.simulation_for(kBoomerang, true, true) == Simulation::Native);
    guest.configure(Role::Guest, {}, true); // Host not discovered yet: never run competing AI.
    assert(guest.simulation_for(kBoomerang, true, true) == Simulation::Suppressed);
    assert(!guest.lifecycle(10, {}, fish, 1, Lifecycle::Announce));
    guest.configure(Role::Guest, context, true);
    assert(guest.simulation_for(kBoomerang, true, true) == Simulation::Suppressed);
    assert(guest.simulation_for(kBoomerang, false, true) == Simulation::Native); // Aquarium/inventory excluded.
    assert(guest.simulation_for(2504, true, true) == Simulation::Native); // Peeper not implemented by this draft.
    assert(!guest.lifecycle(11, context, fish, 1, Lifecycle::Announce));
    auto old_context = context; old_context.epoch--;
    assert(!guest.lifecycle(10, old_context, fish, 1, Lifecycle::Announce));
    assert(guest.lifecycle(10, context, fish, 1, Lifecycle::Announce));
    Pose p; p.lifecycle_revision = 1; p.sequence = 0xfffffffeu;
    assert(!guest.pose(10, context, fish.id, p));
    assert(!guest.bind(fish.id, false)); // Cannot display until Unity adapter confirms AI/physics suppression.
    assert(guest.bind(fish.id, true));
    assert(guest.pose(10, context, fish.id, p));
    assert(!guest.pose(10, context, fish.id, p));
    p.sequence = 1; assert(guest.pose(10, context, fish.id, p)); // Sequence wraps safely.
    p.sequence = 0; assert(!guest.pose(10, context, fish.id, p));
    p.sequence = 2; p.position[0] = std::numeric_limits<float>::quiet_NaN();
    assert(!guest.pose(10, context, fish.id, p));
    p.position[0] = 0; p.rotation = {0, 0, 0, 0}; assert(!guest.pose(10, context, fish.id, p));
    p.rotation = {0, 0, 0, 1}; assert(!guest.pose(11, context, fish.id, p));
    assert(guest.lifecycle(10, context, fish, 2, Lifecycle::Suspend));
    p.lifecycle_revision = 2; assert(!guest.pose(10, context, fish.id, p));
    assert(!guest.bind(fish.id, true));
    assert(guest.simulation_for(kBoomerang, true, true) == Simulation::Suppressed); // Silence/suspend isn't host migration.
    assert(guest.lifecycle(10, context, fish, 3, Lifecycle::Announce));
    p.lifecycle_revision = 2; assert(!guest.pose(10, context, fish.id, p));
    p.lifecycle_revision = 3; assert(guest.pose(10, context, fish.id, p));
    guest.unbind(fish.id); assert(!guest.pose(10, context, fish.id, p)); // Stream-out is not death.
    assert(guest.bind(fish.id, true)); assert(!guest.pose(10, context, fish.id, p));
    p.sequence++; assert(guest.pose(10, context, fish.id, p));
    assert(guest.lifecycle(10, context, fish, 4, Lifecycle::Remove));
    assert(!guest.lifecycle(10, context, fish, 5, Lifecycle::Announce));
    assert(!guest.bind(fish.id, true)); assert(!guest.pose(10, context, fish.id, p));
    guest.configure(Role::Guest, context, true); assert(guest.size() == 1); // Same context retains tombstones.
    auto changed = fish; changed.prefab[0] = '3'; assert(!guest.lifecycle(10, context, changed, 6, Lifecycle::Remove));
    GuestRegistry capacity; capacity.configure(Role::Guest, context, true);
    for (size_t i = 0; i < kMaxRecords; ++i) {
        char id[37]; std::snprintf(id, sizeof(id), "30000000-0000-0000-0000-%012llx", (unsigned long long)i);
        auto other = fish; other.id = id;
        assert(capacity.lifecycle(10, context, other, 2, Lifecycle::Remove));
        assert(!capacity.lifecycle(10, context, other, 1, Lifecycle::Announce));
    }
    assert(!capacity.lifecycle(10, context, fish, 1, Lifecycle::Announce)); // No tombstone eviction.
    context.epoch++;
    guest.configure(Role::Guest, context, true); assert(guest.size() == 0);
    assert(!guest.lifecycle(10, old_context, fish, 1, Lifecycle::Announce));
    guest.configure(Role::Host, context, true);
    assert(guest.simulation_for(kBoomerang, true, true) == Simulation::Native);
    guest.configure(Role::Offline, {}, true);
    assert(guest.simulation_for(kBoomerang, true, true) == Simulation::Native);
    std::puts("PASS creature replica model: authority, AI policy, lifecycle, identity, stale poses, capacity");
}
