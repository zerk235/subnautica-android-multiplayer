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
}
