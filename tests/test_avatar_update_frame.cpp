#include "modloader/src/avatar_update_frame.h"
#include <cassert>
#include <cstdio>

int main() {
    bool updating = false;
    int updates = 0, errors = 0, nested = 0;
    auto report = [&] { assert(!updating); ++errors; };
    auto next_frame = [&] {
        run_avatar_update_frame(updating, [&] {
            assert(updating);
            ++updates;
            run_avatar_update_frame(updating, [&] { ++nested; }, report);
        }, report);
        assert(!updating);
    };
    next_frame();
    assert(updates == 1 && nested == 0 && errors == 0);

    // Reproduce a managed tool/Animator failure after the body pose was written.
    // The HUD/network continues separately; the next frame must still move the
    // body instead of being permanently skipped by the re-entry flag.
    int body_position = 10;
    run_avatar_update_frame(updating, [&] {
        body_position = 20;
        throw 1;
    }, report);
    assert(body_position == 20 && !updating && errors == 1);
    run_avatar_update_frame(updating, [&] { body_position = 30; }, report);
    assert(body_position == 30 && !updating);

    // Repeated failures and an early return also release the guard. A nested
    // rejected call must never release the outer frame's active guard.
    for (int i = 0; i < 3; ++i) {
        run_avatar_update_frame(updating, [&] {
            run_avatar_update_frame(updating, [&] { ++nested; }, report);
            assert(updating && nested == 0);
            throw i;
        }, report);
        next_frame();
    }
    run_avatar_update_frame(updating, [] { return; }, report);
    assert(!updating && updates == 4 && errors == 4 && nested == 0);
    puts("avatar update: exception recovery, later movement, repeated failures and re-entry passed");
}
