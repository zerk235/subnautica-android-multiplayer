#include "modloader/src/creature_motion.h"
#include <cassert>
#include <limits>
using namespace creature_motion;
int main(){
    Message request;request.sender=2;request.target=request.host=1;request.epoch=9;request.nonce=0x1234567812345678ULL;
    request.session[0]=1;request.fish[15]=2;request.prefab[3]=3;
    std::vector<uint8_t> wire;assert(encode(request,&wire)&&wire.size()==112);
    Message decoded;assert(decode(wire.data(),wire.size(),&decoded)&&same_run(request,decoded));
    assert(decoded.nonce==request.nonce&&decoded.sender==2&&decoded.target==1);
    for(size_t n=0;n<wire.size();++n)assert(!decode(wire.data(),n,&decoded));
    wire.push_back(0);assert(!decode(wire.data(),wire.size(),&decoded));wire.pop_back();
    wire[1]=1;assert(!decode(wire.data(),wire.size(),&decoded));wire[1]=0;
    assert(permitted(request,1,1,request.session,9));
    assert(!permitted(request,2,1,request.session,9));
    assert(!permitted(request,1,1,request.session,10));
    auto bad=request;bad.session[2]=1;assert(!permitted(bad,1,1,request.session,9));
    bad=request;bad.nonce=0;assert(!encode(bad,&wire));
    Message pose=request;pose.kind=Pose;pose.sender=1;pose.target=2;pose.sequence=0xfffffffeu;
    assert(permitted(pose,2,1,request.session,9));assert(!permitted(pose,2,3,request.session,9));
    Receiver rx;rx.reset(request);Message sample;
    assert(!rx.sample(1,&sample));assert(rx.accept(pose,10));assert(!rx.accept(pose,10.1));
    pose.sequence=0xffffffffu;pose.position[0]=10;pose.rotation={0,0,0,-1};assert(rx.accept(pose,10.2));
    assert(rx.sample(10.22,&sample));assert(std::fabs(sample.position[0]-5)<.001);assert(std::fabs(sample.rotation[3]-1)<.001);
    assert(rx.sample(100,&sample)&&sample.position[0]==10); // Silence freezes the last host pose, no extrapolation.
    pose.sequence=1;pose.position[0]=20;assert(rx.accept(pose,10.3));
    auto stale=pose;stale.sequence=0xffffffffu;assert(!rx.accept(stale,10.4));
    bad=pose;bad.nonce++;assert(!rx.accept(bad,10.4));
    bad=pose;bad.fish[0]++;assert(!rx.accept(bad,10.4));
    bad=pose;bad.prefab[0]++;assert(!rx.accept(bad,10.4));
    bad=pose;bad.sender=3;assert(!rx.accept(bad,10.4));
    bad=pose;bad.position[0]=std::numeric_limits<float>::quiet_NaN();assert(!rx.accept(bad,10.4));
    bad=pose;bad.rotation={0,0,0,0};assert(!encode(bad,&wire));
    bad=pose;bad.elapsed_ms=60001;assert(!encode(bad,&wire));
    assert(encode(pose,&wire));put32(wire.data()+84,0x7f800000u);assert(!decode(wire.data(),wire.size(),&decoded));
    request.nonce++;rx.reset(request);assert(!rx.sample(11,&sample)&&!rx.accept(pose,11));
    assert(!controlled_gap(true,19.999)&&controlled_gap(true,20)&&controlled_gap(true,24.999)&&!controlled_gap(true,25));
    assert(!controlled_gap(false,22)&&!controlled_gap(true,std::numeric_limits<double>::quiet_NaN()));
    assert(bearing(0,10,0)==Bearing::Ahead&&bearing(10,0,0)==Bearing::Right);
    assert(bearing(0,-10,0)==Bearing::Behind&&bearing(-10,0,0)==Bearing::Left);
    assert(bearing(10,0,90)==Bearing::Ahead&&bearing(0,10,350)==Bearing::Ahead);
    Receiver gap;request.nonce=123;gap.reset(request);pose.nonce=123;pose.sequence=1;pose.position={0,0,0};
    pose.rotation={0,.6f,0,.8f};assert(encode(pose,&wire)&&decode(wire.data(),wire.size(),&decoded));
    assert(decoded.rotation==pose.rotation&&gap.accept(pose,0));
    pose.sequence=2;pose.position[0]=2;assert(gap.accept(pose,.1));
    assert(gap.sample(5,&sample)&&sample.position[0]==2); // Five seconds of silence never extrapolates.
    pose.sequence=3;pose.position[0]=30;assert(gap.accept(pose,5.1));
    assert(gap.sample(5.1,&sample)&&sample.position[0]==2); // First returning packet does not teleport.
    assert(gap.sample(5.275,&sample)&&sample.position[0]>14&&sample.position[0]<17);
    assert(gap.sample(5.46,&sample)&&sample.position[0]==30);
    pose.sequence=4;assert(!gap.accept(pose,5)); // Local receive clock cannot roll backwards.

    Message live = pose;
    live.kind = LivePose;
    live.target = 0;
    live.nonce = 1;
    live.sequence = 100;
    assert(encode(live, &wire) && decode(wire.data(), wire.size(), &decoded));
    assert(permitted(decoded, 2, 1, live.session, live.epoch));
    assert(permitted(decoded, 3, 1, live.session, live.epoch));
    assert(!permitted(decoded, 1, 1, live.session, live.epoch));
    bad = live; bad.target = 2; assert(!valid(bad));
    bad = live; bad.sender = 2; assert(!valid(bad));
    bad = live; bad.elapsed_ms = 1; assert(!valid(bad));
    Receiver runtime;
    runtime.reset(live);
    assert(runtime.accept(decoded, 1));
    bad = live; bad.sequence--; assert(!runtime.accept(bad, 2));
    bad = live; bad.sequence++; bad.fish[0]++; assert(!runtime.accept(bad, 2));
    bad = live; bad.sequence++; bad.kind = Pose; bad.target = 2; assert(!runtime.accept(bad, 2));
    assert(runtime.sample(10, &sample) && sample.sequence == live.sequence);

    Receiver playback;
    playback.reset(live);
    auto moving = live;
    for (int i = 0; i < 4; ++i) {
        moving.sequence++;
        moving.position[0] = i * .05f;
        assert(playback.accept(moving, i * .05));
    }
    assert(playback.sample(.15, &sample));
    assert(std::fabs(sample.position[0] - .065f) < .001f && sample.sequence == moving.sequence);
    assert(playback.sample(.28, &sample) && std::fabs(sample.position[0] - .195f) < .001f);
    assert(playback.sample(.5, &sample) && std::fabs(sample.position[0] - .215f) < .001f);
    moving.sequence++;
    moving.position[0] = 3;
    assert(playback.accept(moving, .5));
    assert(playback.sample(.5, &sample) && std::fabs(sample.position[0] - .215f) < .001f);
    assert(playback.sample(.69, &sample) && sample.position[0] == 3);
    moving.sequence++;
    moving.position[0] = 12;
    assert(playback.accept(moving, .7));
    assert(playback.sample(.86, &sample) && std::fabs(sample.position[0] - 12.2f) < .001f);

    auto bladderfish = live;
    bladderfish.species = Species::Bladderfish;
    assert(tech_for_species(bladderfish.species) == 2515);
    assert(species_for_tech(2515) == Species::Bladderfish);
    assert(encode(bladderfish, &wire) && wire[1] == 2);
    assert(decode(wire.data(), wire.size(), &decoded) && same_run(decoded, bladderfish));
    bladderfish.sequence++;
    assert(!runtime.accept(bladderfish, 2)); // Same GUID with a different species is not the same stream.
    bad = request; bad.species = Species::Bladderfish; assert(!valid(bad));
    wire[1] = 3; assert(!decode(wire.data(), wire.size(), &decoded));
    // Slow frames and reordered UDP cannot roll a pending pose backwards.
    Mailbox inbox;
    assert(inbox.push(live));
    bad = live; bad.sequence += 2; bad.position[0] = 9; assert(inbox.push(bad));
    bad.sequence--; bad.position[0] = 8; assert(inbox.push(bad));
    assert(!inbox.take(false, &sample));
    assert(inbox.take(true, &sample) && sample.sequence == live.sequence + 2 && sample.position[0] == 9);
    assert(!inbox.take(true, &sample));
    SequenceGate sequences;
    live.sequence = 0xffffffffu;
    assert(sequences.accept(live) && !sequences.accept(live));
    bad = live; bad.sequence = 1; assert(sequences.accept(bad));
    assert(!sequences.accept(live)); // Stream-out must not reset the floor.
    bad.sequence = 2; bad.prefab[0]++; assert(!sequences.accept(bad));
    sequences = {};
    assert(sequences.accept(live)); // New world/session starts a fresh stream.
    for (unsigned i = 0; i < kControlQueueCapacity; ++i) {
        auto control = request; control.nonce = i + 1;
        assert(inbox.push(control));
    }
    for (unsigned i = 0; i < kLiveQueueCapacity; ++i) {
        bad = live; bad.fish[0] = uint8_t(i + 1); assert(inbox.push(bad));
    }
    bad = live; bad.fish[0] = 99; assert(!inbox.push(bad));
    bad = request; bad.nonce = 99; assert(!inbox.push(bad));
    for (size_t i = 0; i < kControlQueueCapacity; ++i) assert(inbox.take(false, &sample));
    assert(!inbox.take(false, &sample));
    // A full live mailbox must still replace a pending fish with its newest pose.
    bad = live; bad.fish[0] = 40; bad.sequence = 2; assert(inbox.push(bad));
    for (size_t i = 0; i < kLiveQueueCapacity; ++i) {
        assert(inbox.take(true, &sample));
        if (sample.fish[0] == 40) assert(sample.sequence == 2);
    }
    assert(!inbox.take(true, &sample));

    // Forty unique fish all get a turn under the per-tick budget. Reordering
    // the interest list cannot reset deadlines or send a duplicate burst.
    std::array<Id, 41> ids{};
    for (size_t i=0;i<ids.size();++i) ids[i][0]=uint8_t(i+1);
    LiveSchedule schedule;schedule.refresh(ids.data(),ids.size());
    std::array<unsigned,40> counts{};size_t index=99;
    for(int tick=0;tick<5;++tick)
        for(size_t i=0;i<kLiveSendPerTick;++i) {
            assert(schedule.take(tick*.02,&index)&&index<40);
            ++counts[index];
        }
    for(auto value:counts)assert(value==1);
    assert(!schedule.take(.09,&index));
    std::reverse(ids.begin(),ids.begin()+40);
    schedule.refresh(ids.data(),40);
    assert(!schedule.take(.09,&index));
    assert(schedule.take(.1,&index)&&ids[index][0]<=8);
    // A long stall sends a current sample once per fish, without backlog catchup.
    counts={};
    for(size_t i=0;i<40;++i) {assert(schedule.take(20,&index));++counts[index];}
    assert(!schedule.take(20,&index));
    for(auto value:counts)assert(value==1);
    assert(!schedule.take(std::numeric_limits<double>::quiet_NaN(),&index));
    assert(!schedule.take(-1,&index));
    schedule.refresh(nullptr,40);assert(!schedule.take(30,&index));
    ids[0]={};ids[1]=ids[2];schedule.refresh(ids.data(),3);
    assert(schedule.take(30,&index)&&index==1);
    assert(!schedule.take(30,&index)); // Empty and duplicate identities ignored.
}
