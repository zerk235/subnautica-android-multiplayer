#pragma once
#include "net.h"
#include <cmath>
#include <cstring>

// STATE and VEHICLE share the sender's sequence counter. This lets an explicit
// exit in a newer STATE release a seat without waiting for the vehicle timeout,
// while an older/reordered on-foot STATE cannot unlock a newly occupied seat.
inline bool vehicle_sequence_newer(std::uint32_t value,std::uint32_t previous) {
    return static_cast<std::int32_t>(value-previous)>0;
}
inline bool vehicle_pose_fresh(const VehicleState& pose,double now,double timeout) {
    const double age=now-pose.updated;
    return pose.active&&pose.seq_valid&&pose.pilot&&std::isfinite(age)&&age>=0&&age<=timeout;
}

inline bool is_seamoth_vehicle(const char class_id[16]) {
    return std::memcmp(class_id, "SeaMoth", sizeof("SeaMoth")) == 0;
}

inline std::uint32_t remote_vehicle_pilot(const char* class_id, const std::uint8_t guid[16], std::uint32_t local_id,
    const VehicleState* vehicles,std::size_t vehicle_count,const PeerState* peers,std::size_t peer_count,
    double now,double timeout=kPeerTimeout) {
    if(!class_id||!guid||!vehicles||!peers||!std::isfinite(now))return 0;
    const std::uint8_t zero[16]{};
    if(!std::memcmp(guid,zero,16))return 0;
    std::uint32_t occupied_by=0;
    for(std::size_t i=0;i<vehicle_count;++i) {
        const auto& pose=vehicles[i];
        if(!vehicle_pose_fresh(pose,now,timeout)||pose.pilot==local_id||
           std::memcmp(pose.guid,guid,16)||std::strncmp(class_id,pose.classId,sizeof(pose.classId))!=0)continue;
        const PeerState* pilot=nullptr;
        for(std::size_t p=0;p<peer_count;++p) {
            const double age=now-peers[p].last_seen;
            if(peers[p].id==pose.pilot&&peers[p].active&&std::isfinite(age)&&age>=0&&age<=timeout) {
                pilot=&peers[p];break;
            }
        }
        if(!pilot)continue;
        if(pilot->seq_valid&&vehicle_sequence_newer(pilot->sequence,pose.sequence)&&
           !(pilot->flags&kFlagPiloting))continue;
        bool switched=false;
        for(std::size_t other=0;other<vehicle_count;++other) {
            const auto& next=vehicles[other];
            if(next.pilot==pose.pilot&&vehicle_pose_fresh(next,now,timeout)&&
               vehicle_sequence_newer(next.sequence,pose.sequence)) {switched=true;break;}
        }
        if(switched)continue;
        if(!occupied_by||pose.pilot<occupied_by)occupied_by=pose.pilot;
    }
    return occupied_by;
}

inline bool is_cyclops_vehicle(const char class_id[16]) {
    return std::memcmp(class_id, "Cyclops", sizeof("Cyclops")) == 0;
}

inline std::uint32_t remote_seamoth_pilot(const std::uint8_t guid[16], std::uint32_t local_id,
    const VehicleState* vehicles, std::size_t vehicle_count, const PeerState* peers,
    std::size_t peer_count, double now, double timeout=kPeerTimeout) {
    return remote_vehicle_pilot("SeaMoth", guid, local_id, vehicles, vehicle_count,
                                peers, peer_count, now, timeout);
}
