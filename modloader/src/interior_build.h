#pragma once
#include "net.h"
#include <cmath>
#include <cstring>
#include <cstdio>

namespace interior_build {
constexpr int kLocker=1504;
inline bool nonzero(const uint8_t* p) {uint8_t bits=0;for(int i=0;i<16;++i)bits|=p[i];return bits!=0;}
inline bool pose_valid(const float* p) {
    for(int i=0;i<7;++i)if(!std::isfinite(p[i]))return false;
    for(int i=0;i<3;++i)if(std::fabs(p[i])>1000)return false;
    const float n=p[3]*p[3]+p[4]*p[4]+p[5]*p[5]+p[6]*p[6];
    return n>0.99f&&n<1.01f;
}
inline bool payload_valid(const WorldEvent& e) {
    const uint8_t allowed=kWorldBuildInterior|kWorldBuildComplete|kWorldBuildIsInside|kWorldBuildAttached;
    return e.tech_type==kLocker && (e.kind==kWorldEventBuild||e.kind==kWorldEventRemove) &&
        e.build_flags==allowed && e.constructed_amount==1.0f && e.object_id[0] &&
        std::memchr(e.object_id,0,sizeof(e.object_id)) && nonzero(e.class_id) &&
        nonzero(e.interior_parent_guid) && pose_valid(e.interior_local_pose);
}
inline bool same_placement(const WorldEvent& a,const WorldEvent& b) {
    if(a.tech_type!=b.tech_type || std::memcmp(a.class_id,b.class_id,16) ||
       std::memcmp(a.interior_parent_guid,b.interior_parent_guid,16))return false;
    float distance=0,dot=0;
    for(int i=0;i<3;++i){const float d=a.interior_local_pose[i]-b.interior_local_pose[i];distance+=d*d;}
    for(int i=3;i<7;++i)dot+=a.interior_local_pose[i]*b.interior_local_pose[i];
    return std::isfinite(distance)&&std::isfinite(dot)&&distance<=0.0004f&&std::fabs(dot)>=0.9999f;
}
struct Record {
    bool used=false,removed=false,removal_applied=false,publish=false;
    WorldEvent state{};
    double next_send=0,expires=0;
};
struct Registry {
    Record records[64]{};
    Record* find(const char* id,bool allocate=false) {
        Record* empty=nullptr;
        for(auto& r:records){if(r.used&&!std::strcmp(r.state.object_id,id))return &r;if(!r.used&&!empty)empty=&r;}
        if(!allocate||!empty)return nullptr;
        empty->used=true;std::snprintf(empty->state.object_id,sizeof(empty->state.object_id),"%s",id);
        return empty;
    }
    bool accept(const WorldEvent& e) {
        if(!payload_valid(e))return false;
        auto* r=find(e.object_id);
        if(r&&r->state.tech_type && !same_placement(r->state,e))return false;
        if(r&&r->removed&&e.kind!=kWorldEventRemove)return false;
        if(!r)r=find(e.object_id,true);
        if(!r)return false;
        r->state=e;
        if(e.kind==kWorldEventRemove)r->removed=true;
        return true;
    }
};
}
