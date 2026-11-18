#include "modloader/src/fish_pickup.h"
#include <cassert>
#include <limits>
namespace lc=locker_contents;namespace fp=fish_pickup;
int main(){
    fp::Identity id;id.session[0]=1;id.fish[2]=2;id.prefab[5]=3;id.epoch=0x12345678;
    auto key=fp::key(id);fp::Identity parsed;assert(fp::identity(key,&parsed)&&parsed.epoch==id.epoch&&parsed.fish==id.fish);
    assert(fp::matches(parsed,id.session,id.epoch)&&!fp::matches(parsed,id.session,id.epoch+1));
    for(size_t n=0;n<key.size();++n)assert(!fp::identity(key.substr(0,n),&parsed));
    auto bad_key=key;bad_key[40]='G';assert(!fp::identity(bad_key,&parsed));
    auto other=id;other.fish[3]=4;
    assert(fp::item_matches(fp::guid(id.fish),id.fish)&&fp::item_matches(fp::hex(id.fish),id.fish));
    assert(!fp::item_matches(fp::guid(other.fish),id.fish));
    lc::Message q;q.kind=lc::Request;q.recipient=1;q.request=9;q.revision=1;q.locker=key;
    q.item={fp::guid(id.fish),fp::kBoomerang,{}};assert(fp::valid(q));
    q.item.tech=creature_motion::kBladderfishTech;assert(fp::valid(q));
    q.item.tech=creature_motion::kHoopfishTech;assert(fp::valid(q));
    auto bad=q;bad.item.data={1};assert(!fp::valid(bad));
    bad=q;bad.item.tech++;assert(!fp::valid(bad));
    bad=q;bad.item.id=fp::guid(other.fish);assert(!fp::valid(bad));
    bad=q;bad.items.push_back(q.item);assert(!fp::valid(bad));
    bad=q;bad.request=0;assert(!fp::valid(bad));
    assert(fp::actor_in_reach(25,.5)&&!fp::actor_in_reach(31,.1)&&!fp::actor_in_reach(1,.8));
    assert(!fp::actor_in_reach(std::numeric_limits<float>::quiet_NaN(),0));
    // The old pivot can be off the aim ray even when the fish's body intersects it.
    const std::array<float,3> forward{0,0,1};
    const std::array<float,3> body{.50f,.2f,.3f};
    const float body_hit=fp::aim_box_distance({.48f,0,2},forward,body);
    assert(body_hit>1.6f&&body_hit<1.7f);
    assert(!fp::aim_box_distance({.7f,0,2},forward,body));
    assert(!fp::aim_box_distance({0,0,4},forward,body));
    assert(!fp::aim_box_distance({0,0,-2},forward,body));
    assert(fp::aim_box_distance({0,0,2},{0,0,-1},body)==0);
    assert(fp::keep_reticle_target(1.15,1,2,4.25f,.25f));
    assert(!fp::keep_reticle_target(1.21,1,2,4.25f,.25f));
    assert(!fp::keep_reticle_target(1.15,1,-2,4.25f,.25f));
    assert(!fp::keep_reticle_target(1.15,1,2,4.5f,.5f));
    assert(!fp::keep_reticle_target(1.15,1,3.7f,14,.25f));
    lc::Authority host;auto item=q.item;item.data={7,8,9};assert(host.initialize({item}));
    auto canonical=q;canonical.item=item;
    auto grant=host.begin(2,canonical);assert(grant.kind==lc::Grant&&fp::valid(grant));
    assert(host.begin(2,canonical).kind==lc::Grant); // Same request is replayed, not reserved twice.
    auto rival=canonical;rival.request++;
    assert(host.begin(3,rival).kind==lc::Reject); // Host arbitrates competing catches.
    fp::Pending guest;guest.used=true;guest.request=q;
    assert(guest.may_restore(grant));guest.loading=true;assert(!guest.may_restore(grant));
    guest.loading=false;guest.applied=true;assert(!guest.may_restore(grant));
    auto applied=q;applied.kind=lc::Applied;assert(fp::valid(applied));
    auto done=host.finish(2,applied,true);assert(done.kind==lc::Done&&host.items.empty());
    assert(host.finish(2,applied,true).kind==lc::Done&&host.revision==2);
    assert(host.begin(2,canonical).kind==lc::Done); // Lost Done cannot issue a second fish.
    assert(host.begin(3,rival).kind==lc::Reject);
    lc::Authority full;assert(full.initialize({item}));assert(full.begin(2,canonical).kind==lc::Grant);
    auto cancel=q;cancel.kind=lc::Cancel;assert(full.finish(2,cancel,false).kind==lc::Reject);
    assert(full.items.size()==1&&full.revision==1); // Full inventory leaves host's fish intact.
    assert(full.finish(2,applied,true).kind==lc::Reject); // Late Applied after Cancel is fenced.
    canonical.request=10;assert(full.begin(2,canonical).kind==lc::Grant);
    auto tampered=canonical;tampered.item.data={99};
    assert(full.finish(2,tampered,true).item.data==item.data); // Immutable granted snapshot.
    std::vector<uint8_t> bytes;assert(lc::encode(grant,&bytes));lc::Message decoded;
    assert(lc::decode(bytes,&decoded)&&fp::valid(decoded)&&decoded.item.data==item.data&&
           decoded.item.tech==creature_motion::kHoopfishTech);
    auto reset=q;reset.kind=lc::Snapshot;reset.recipient=0;reset.revision=3;reset.request=100;
    assert(fp::valid(reset));reset.request=uint64_t(UINT32_MAX)+1;assert(!fp::valid(reset));
    assert(fp::pose_after_floor(100,100)&&fp::pose_after_floor(101,100)&&!fp::pose_after_floor(99,100));
    assert(fp::pose_after_floor(1,UINT32_MAX)&&!fp::pose_after_floor(UINT32_MAX,1));
    lc::Authority dropped;dropped.revision=3;assert(dropped.initialize({item}));
    assert(dropped.begin(2,q).kind==lc::Reject); // Previous lifecycle cannot catch the dropped fish.
    auto again=q;again.request=90;again.revision=3;again.item=item;
    assert(dropped.begin(2,again).kind==lc::Grant);
    assert(dropped.finish(2,again,true).revision==4&&dropped.items.empty());
    assert(dropped.finish(2,again,true).revision==4); // Recapture remains idempotent.
}
