#define SNMOD_BASE_GEOMETRY_CAPTURE
#include "modloader/src/interior_build.h"
#include "modloader/src/fabricator_state.h"
#include "modloader/src/base_build_completion.h"
#include <cassert>

WorldEvent fabricator(const char* id="fabricator-one") {
    WorldEvent e{};e.kind=kWorldEventBuild;e.tech_type=interior_build::kFabricator;
    e.build_flags=kWorldBuildComplete|kWorldBuildInterior|kWorldBuildIsInside|kWorldBuildAttached;
    e.constructed_amount=1;std::snprintf(e.object_id,sizeof(e.object_id),"%s",id);
    e.class_id[0]=9;e.interior_parent_guid[0]=7;e.interior_local_pose[6]=1;
    return e;
}
int main() {
    using namespace interior_build;
    auto e=fabricator();assert(payload_valid(e));
    for(int flags=0;flags<256;++flags){auto p=e;p.build_flags=flags;assert(payload_valid(p)==(flags==e.build_flags));}
    for(float amount:{0.f,.5f,.999f,1.001f,NAN}){auto p=e;p.constructed_amount=amount;assert(!payload_valid(p));}
    auto bad=e;memset(bad.interior_parent_guid,0,16);assert(!payload_valid(bad));
    bad=e;bad.interior_local_pose[3]=NAN;assert(!payload_valid(bad));
    Registry r;assert(r.accept(e));
    bad=e;bad.interior_parent_guid[15]=1;assert(!r.accept(bad));
    bad=e;bad.interior_local_pose[1]=.1f;assert(!r.accept(bad));
    bad=e;bad.class_id[15]=1;assert(!r.accept(bad));
    bad=e;bad.tech_type=kLocker;assert(!r.accept(bad));
    auto removed=e;removed.kind=kWorldEventRemove;assert(r.accept(removed));
    assert(r.accept(removed));assert(!r.accept(e));
    assert(r.accept(fabricator("fabricator-rebuilt")));
    auto early=fabricator("remove-before-create");early.kind=kWorldEventRemove;assert(r.accept(early));
    early.kind=kWorldEventBuild;assert(!r.accept(early));
    // Exercise the production demolition guard across every busy condition,
    // including unknown/invalid item counts: never discard unfinished output.
    for(unsigned bits=0;bits<16;++bits)for(int output:{-1,0,1,4,2147483647})for(bool visual_open:{false,true}) {
        fabricator_sync::State s{bool(bits&1),bool(bits&2),bool(bits&4),bool(bits&8),output};
        s.visual_open=visual_open;
        assert(fabricator_sync::removal_allowed(s)==(bits==1&&output==0));
        assert((fabricator_sync::removal_block(s)==nullptr)==fabricator_sync::removal_allowed(s));
    }
    fabricator_sync::State after_craft{true,false,false,false,0,true};
    assert(fabricator_sync::removal_allowed(after_craft)); // nearby player keeps shell open after menu closes
    after_craft.menu_open=true;assert(!fabricator_sync::removal_allowed(after_craft));
    assert(std::strcmp(fabricator_sync::removal_block(after_craft),"menu-open")==0);
    int calls=0,sends=0;
    for(unsigned bits=0;bits<64;++bits) {
        bool requested=bits&1,already=bits&2,preview=bits&4,remote=bits&8,ready=bits&16,changed=bits&32;
        const int before=sends;
        base_geometry::observe_completion(requested,already,preview,remote,ready,
            [&]{return payload_valid(e);},[&]{++calls;return uint8_t(changed);},
            [&]{++sends;},[](const char*){assert(false);});
        assert(sends-before==int(requested&&!already&&!preview&&!remote&&ready&&changed));
    }
    assert(calls==64);
    for(float amount:{0.f,.5f,1.f,NAN})for(unsigned bits=0;bits<32;++bits) {
        bool complete=bits&1,remote=bits&2,ready=bits&4,preview=bits&8,success=bits&16;
        const int before=sends;
        base_geometry::observe_deconstruction(amount,complete,remote,ready,preview,
            [&]{return payload_valid(removed);},[&]()->void*{return success?&r:nullptr;},
            [&]{++sends;},[](const char*){assert(false);});
        assert(sends-before==int(amount==0&&!complete&&!remote&&ready&&!preview&&success));
    }
    puts("PASS: fabricator exact-base placement, strict flags, busy/output demolition guard, explicit transitions and removal/rebuild");
}
