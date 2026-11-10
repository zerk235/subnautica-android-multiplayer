#include "modloader/src/managed_root.h"
#include "modloader/src/runtime_load_guard.h"
#include <cassert>
#include <map>
#include <limits>
namespace {
using Handle = ManagedRoot::Handle;
static_assert(sizeof(Handle) == sizeof(void*));
std::map<Handle, void*> roots;
Handle next = Handle{1} << 40; // Actual ARM64 handles carry high address bits.
bool fail = false;
unsigned releases = 0;
Handle retain(void* value) { if (fail) return 0; roots[++next] = value; return next; }
void* resolve(Handle id) { auto it = roots.find(id); return it == roots.end() ? nullptr : it->second; }
void release(Handle id) { assert(roots.erase(id) == 1); ++releases; }
}
int main() {
    int request = 1, prefab = 2, moved = 3;
    {
        ManagedRoot root;
        assert(root.retain(&request, retain, resolve, release));
        assert(roots.size() == 1 && root.get() == &request);
        assert(roots.begin()->first > 0xffffffffULL);
        // Resolve the runtime's target instead of relying on a stored raw address.
        roots.begin()->second = &moved;
        assert(root.get() == &moved);
        fail = true;
        assert(!root.retain(&prefab, retain, resolve, release));
        assert(root.get() == &moved && roots.size() == 1);
        fail = false;
        assert(root.retain(&prefab, retain, resolve, release));
        assert(root.get() == &prefab && roots.size() == 1 && releases == 1);
        root.clear(); root.clear(); assert(!root.get() && roots.empty() && releases == 2);
        assert(root.retain(&request, retain, resolve, release));
    }
    assert(roots.empty() && releases == 3); // Scope/context teardown releases once.
    RuntimeLoadGuard guard;
    assert(!guard.update(true, true, true, 1, 100));
    assert(!guard.update(true, true, true, 1, 1000)); // Long load never satisfies warmup.
    assert(!guard.update(true, true, false, 1, 1001));
    assert(!guard.update(true, true, false, 1, 1002.9));
    assert(guard.update(true, true, false, 1, 1003));
    assert(!guard.update(true, true, true, 1, 1004)); // Same-slot reload resets readiness.
    assert(!guard.update(true, true, false, 1, 1005));
    assert(guard.update(true, true, false, 1, 1007));
    assert(!guard.update(true, true, false, 2, 1008)); // New world generation resets it too.
    assert(!guard.update(true, false, false, 2, 1011));
    assert(!guard.update(false, true, false, 2, 1012));
    assert(!guard.update(true, true, false, 2, std::numeric_limits<double>::quiet_NaN()));
    assert(!guard.update(true, true, false, 2, 2000));
    assert(guard.update(true, true, false, 2, 2002));
    assert(!guard.update(true, true, false, 2, 100)); // Clock rollback.
    PausedWorkDeadline pause;
    double deadline = 45;
    pause.pause(1); pause.pause(10); pause.resume(101, &deadline);
    assert(deadline == 145); // One hundred seconds of loading do not expire a job.
    pause.resume(102, &deadline); assert(deadline == 145);
}
