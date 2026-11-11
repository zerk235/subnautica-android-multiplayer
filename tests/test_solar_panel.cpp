#define SNMOD_BASE_GEOMETRY_CAPTURE
#include "modloader/src/interior_build.h"
#include "modloader/src/base_build_completion.h"
#include <cassert>

WorldEvent panel(const char* id="solar-one") {
    WorldEvent e{};e.kind=kWorldEventBuild;e.tech_type=interior_build::kSolarPanel;
    e.build_flags=kWorldBuildComplete|kWorldBuildInterior|kWorldBuildAttached;
    e.constructed_amount=1;std::snprintf(e.object_id,sizeof(e.object_id),"%s",id);
    e.class_id[0]=3;e.interior_parent_guid[0]=7;e.interior_local_pose[6]=1;
    return e;
}
int main() {
    using namespace interior_build;
    auto e=panel();assert(payload_valid(e));
    // Outdoor base-mounted panels need not carry the locker's inside flag.
    for(int flags=0;flags<256;++flags) {
        auto p=e;p.build_flags=flags;
        const int required=kWorldBuildComplete|kWorldBuildInterior;
        const int optional=kWorldBuildIsInside|kWorldBuildAttached;
        assert(payload_valid(p)==((flags&required)==required&&!(flags&~(required|optional))));
    }
    auto bad=e;bad.tech_type=1518;assert(!payload_valid(bad));
    bad=e;memset(bad.interior_parent_guid,0,16);assert(!payload_valid(bad));
    bad=e;bad.interior_local_pose[0]=NAN;assert(!payload_valid(bad));
    Registry r;assert(r.accept(e));
    bad=e;bad.interior_parent_guid[1]=1;assert(!r.accept(bad));
    bad=e;bad.interior_local_pose[2]+=.1f;assert(!r.accept(bad));
    bad=e;bad.build_flags^=kWorldBuildIsInside;assert(!r.accept(bad));
    bad=e;bad.tech_type=kLocker;bad.build_flags|=kWorldBuildIsInside;assert(!r.accept(bad));
    auto removed=e;removed.kind=kWorldEventRemove;assert(r.accept(removed));
    assert(r.accept(removed));assert(!r.accept(e)); // late repair cannot resurrect
    assert(r.accept(panel("solar-rebuilt"))); // same pose, new identity
    auto early=panel("remove-first");early.kind=kWorldEventRemove;assert(r.accept(early));
    early.kind=kWorldEventBuild;assert(!r.accept(early));
    // The native transition observers, also used for lockers, exclude hover,
    // cancellation, failed completion, partial demolition and remote echo.
    int published=0,calls=0;
    for(unsigned b=0;b<64;++b) {
        bool requested=b&1,already=b&2,preview=b&4,remote=b&8,ready=b&16,changed=b&32;
        int before=published;
        base_geometry::observe_completion(requested,already,preview,remote,ready,
            [&]{return payload_valid(e);},[&]{++calls;return uint8_t(changed);},
            [&]{++published;},[](const char*){assert(false);});
        assert(published-before==int(requested&&!already&&!preview&&!remote&&ready&&changed));
    }
    assert(calls==64);
    for(float amount:{0.f,.2f,1.f,NAN})for(unsigned b=0;b<32;++b) {
        bool complete=b&1,remote=b&2,ready=b&4,preview=b&8,success=b&16;
        int before=published;
        base_geometry::observe_deconstruction(amount,complete,remote,ready,preview,
            [&]{return payload_valid(removed);},[&]()->void*{return success?&r:nullptr;},
            [&]{++published;},[](const char*){assert(false);});
        assert(published-before==int(amount==0&&!complete&&!remote&&ready&&!preview&&success));
    }
    puts("PASS: solar exact-base placement, outdoor flags, identity, removal/rebuild, preview and remote-echo guards");
}
