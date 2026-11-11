#include "modloader/src/vehicle_occupancy.h"
#include <cassert>
#include <cstdio>
#include <limits>

int main() {
    const std::uint8_t guid[16]{1},other_guid[16]{2},zero[16]{};
    VehicleState vehicles[3]{};
    PeerState peers[2]{};
    auto& v=vehicles[0];
    v.active=v.seq_valid=true;v.pilot=20;v.updated=100;v.sequence=100;
    std::memcpy(v.guid,guid,16);std::memcpy(v.classId,"SeaMoth",8);
    auto& p=peers[0];
    p.id=20;p.active=p.seq_valid=true;p.last_seen=100;p.sequence=101;p.flags=kFlagPiloting;
    auto occupant=[&](double now=100) {return remote_seamoth_pilot(guid,10,vehicles,3,peers,2,now);};
    assert(occupant()==20);
    assert(!remote_seamoth_pilot(other_guid,10,vehicles,3,peers,2,100));
    assert(!remote_seamoth_pilot(guid,20,vehicles,3,peers,2,100));
    assert(!remote_seamoth_pilot(zero,10,vehicles,3,peers,2,100));

    // On-foot state before entry must not release a newer piloting packet.
    p.flags=0;p.sequence=99;assert(occupant()==20);
    p.sequence=102;assert(!occupant()); // explicit newer exit opens the seat
    p.flags=kFlagPiloting;assert(occupant()==20); // a new pilot can use it again
    p.active=false;assert(!occupant());p.active=true;
    assert(occupant(105)==20);assert(!occupant(105.01));
    v.updated=std::numeric_limits<double>::quiet_NaN();assert(!occupant());
    v.updated=101;assert(!occupant());v.updated=100;

    // A pilot moving to a different vehicle releases the old one even when both
    // snapshots still exist; receipt order must not revive the old claim.
    vehicles[1]=v;vehicles[1].guid[0]=2;vehicles[1].sequence=110;
    vehicles[0].updated=100.1;assert(!occupant(100.1));
    vehicles[1].active=false;assert(occupant(100.1)==20);
    vehicles[0].updated=100;

    v.sequence=0xfffffffeu;p.sequence=1;p.flags=0;
    assert(!occupant()); // wrapped STATE sequence is still a newer exit
    p.sequence=0xfffffffdu;assert(occupant()==20);
    vehicles[1]=v;vehicles[1].guid[0]=2;vehicles[1].sequence=1;
    assert(!occupant()); // wrapped newer vehicle supersedes the old vehicle
    vehicles[1].active=false;
    p.flags=kFlagPiloting;
    std::memset(v.classId,'X',sizeof(v.classId));assert(!occupant());
    std::memcpy(v.classId,"Exosuit",8);assert(!occupant());
    std::memcpy(v.classId,"SeaMoth",8);v.seq_valid=false;assert(!occupant());
    puts("vehicle occupancy: occupied/free, exit/reentry, disconnect, expiry, vehicle switch, reorder and wrap passed");
}
