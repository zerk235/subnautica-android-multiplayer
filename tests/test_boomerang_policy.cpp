#include "modloader/src/boomerang_policy.h"
#include <cassert>
#include <limits>

int main() {
    using namespace boomerang_sync;
    assert(hidden(false, 0)); // Loaded locally, no host packet: invisible, AI off.
    assert(!hidden(true, 0) && !hidden(true, 2));
    assert(hidden(true, 2.001) && hidden(true, 3600));
    assert(hidden(true, std::numeric_limits<double>::quiet_NaN()));
    assert(hidden(true, -1));
    assert(!outside_domain(48 * 48));
    assert(outside_domain(49 * 49));
    assert(!restore_context(false, false, false)); // Silence is not authority migration.
    assert(restore_context(true, false, false));
    assert(restore_context(false, true, false));
    assert(restore_context(false, false, true));
}
