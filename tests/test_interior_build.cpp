#define SNMOD_BASE_GEOMETRY_CAPTURE
#include "modloader/src/interior_build.h"
#include "modloader/src/base_build_completion.h"
#include <cassert>
#include <limits>

WorldEvent locker(const char* id="locker-one") {
    WorldEvent e{};e.kind=kWorldEventBuild;e.tech_type=1504;
    e.build_flags=kWorldBuildComplete|kWorldBuildInterior|kWorldBuildIsInside|kWorldBuildAttached;
    e.constructed_amount=1;std::snprintf(e.object_id,sizeof(e.object_id),"%s",id);
    e.class_id[0]=3;e.interior_parent_guid[0]=7;e.interior_local_pose[6]=1;
    return e;
}
int main() {
    using namespace interior_build;
    auto e=locker();assert(payload_valid(e));
    for(int tech:{0,1503,1505,5510,5500}){auto invalid=e;invalid.tech_type=tech;assert(!payload_valid(invalid));}
    for(int i=0;i<8;++i){auto invalid=e;invalid.build_flags^=uint8_t(1u<<i);assert(!payload_valid(invalid));}
    for(float amount:{0.0f,.5f,.999f,1.001f,NAN}){auto invalid=e;invalid.constructed_amount=amount;assert(!payload_valid(invalid));}
    auto invalid=e;memset(invalid.interior_parent_guid,0,16);assert(!payload_valid(invalid));
    invalid=e;memset(invalid.class_id,0,16);assert(!payload_valid(invalid));
    invalid=e;memset(invalid.object_id,'x',sizeof(invalid.object_id));assert(!payload_valid(invalid));
    for(int i=0;i<7;++i){invalid=e;invalid.interior_local_pose[i]=NAN;assert(!payload_valid(invalid));}
    invalid=e;invalid.interior_local_pose[0]=1001;assert(!payload_valid(invalid));
    invalid=e;invalid.interior_local_pose[6]=0;assert(!payload_valid(invalid));
    invalid=e;invalid.kind=kWorldEventProgress;assert(!payload_valid(invalid));

    Registry registry;assert(registry.accept(e));
    auto moved=e;moved.interior_local_pose[0]=.1f;assert(!registry.accept(moved));
    auto wrong_parent=e;wrong_parent.interior_parent_guid[15]=1;assert(!registry.accept(wrong_parent));
    auto opposite_quat=e;opposite_quat.interior_local_pose[6]=-1;assert(registry.accept(opposite_quat));
    auto removed=e;removed.kind=kWorldEventRemove;assert(registry.accept(removed));
    assert(!registry.accept(e)); // delayed completion/repair cannot resurrect it
    assert(registry.accept(removed)); // duplicate demolition is idempotent
    auto rebuilt=locker("locker-two");assert(registry.accept(rebuilt)); // same pose, new ID
    assert(registry.find(e.object_id)->removed&&!registry.find(rebuilt.object_id)->removed);
    auto* original=registry.find(e.object_id);
    original->removal_applied=true;
    original->next_send=123.0;
    assert(!registry.find("not-registered"));
    // Building and dismantling many distinct lockers must not exhaust a
    // lifetime quota, even when very few lockers remain in the world.
    for(int i=2;i<2048;++i){
        char id[40];std::snprintf(id,sizeof(id),"locker-%d",i);
        auto created=locker(id);assert(registry.accept(created));
        if(i%2==0){
            auto demolished=created;demolished.kind=kWorldEventRemove;
            assert(registry.accept(demolished));
            assert(!registry.accept(created)); // no resurrection after growth
        }
    }
    assert(registry.records.size()==2048);
    assert(registry.find(e.object_id)==original); // pending references stay valid
    assert(original->removal_applied&&original->next_send==123.0);
    assert(registry.accept(removed));
    assert(!registry.accept(e)); // oldest tombstone is still present
    assert(registry.accept(rebuilt)); // oldest live record still works
    auto remove_first=locker("remove-before-build");remove_first.kind=kWorldEventRemove;
    assert(registry.accept(remove_first));
    remove_first.kind=kWorldEventBuild;assert(!registry.accept(remove_first));
    moved=removed;moved.interior_local_pose[0]=.1f;assert(!registry.accept(moved));
    registry=Registry{};
    assert(registry.records.empty()&&registry.accept(e));

    // Production hooks use these observers: hover/cancel, partial deconstruction,
    // remote replay and failed native transitions must not publish locker events.
    Registry observed;int calls=0,sends=0;
    for(unsigned bits=0;bits<64;++bits) {
        bool requested=bits&1,already=bits&2,preview=bits&4,remote=bits&8,ready=bits&16,changed=bits&32;
        int before=sends;
        auto result=base_geometry::observe_completion(requested,already,preview,remote,ready,
            [&]{return payload_valid(e);},[&]{++calls;return uint8_t(changed);},
            [&]{++sends;assert(observed.accept(e));},[](const char*){assert(false);});
        assert(result==uint8_t(changed));
        assert(sends-before==int(requested&&!already&&!preview&&!remote&&ready&&changed));
    }
    assert(calls==64);
    for(float amount:{0.0f,.5f,1.0f,NAN})for(unsigned bits=0;bits<32;++bits) {
        bool complete=bits&1,remote=bits&2,ready=bits&4,preview=bits&8,success=bits&16;
        int before=sends;
        base_geometry::observe_deconstruction(amount,complete,remote,ready,preview,
            [&]{return payload_valid(removed);},[&]()->void*{++calls;return success?&observed:nullptr;},
            [&]{++sends;assert(observed.accept(removed));},[](const char*){assert(false);});
        assert(sends-before==int(amount==0&&!complete&&!remote&&ready&&!preview&&success));
    }
    assert(!observed.accept(e));
    puts("PASS: locker exact parent/pose, malformed payload, explicit transitions, 2048 identities, stable references, retained tombstones and session reset");
}
