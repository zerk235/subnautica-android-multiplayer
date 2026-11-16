#pragma once
#include "creature_motion.h"
#include "locker_contents.h"
#include <string>

namespace fish_pickup {
using Id=creature_motion::Id;
constexpr int kBoomerang=creature_motion::kBoomerangTech;
constexpr float kUseDistance=3.6f,kAuthorityDistance=5.5f;
constexpr size_t kMaxRecords=512;
struct Identity {Id session{},fish{},prefab{};uint32_t epoch=0;};
inline std::string hex(const Id& id) {
    static const char digits[]="0123456789abcdef";std::string s;
    for(auto b:id){s+=digits[b>>4];s+=digits[b&15];}return s;
}
inline bool read_hex(const char* text,size_t n,uint8_t* out) {
    if(!text||n%2)return false;
    auto digit=[](char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
    for(size_t i=0;i<n;i+=2){int a=digit(text[i]),b=digit(text[i+1]);if(a<0||b<0)return false;out[i/2]=uint8_t(a*16+b);}return true;
}
inline std::string key(const Identity& id) {
    uint8_t epoch[4];creature_motion::put32(epoch,id.epoch);Id e{};std::copy(epoch,epoch+4,e.begin());
    return "fp1:"+hex(id.session)+":"+hex(e).substr(0,8)+":"+hex(id.fish)+":"+hex(id.prefab);
}
inline bool identity(const std::string& key,Identity* out) {
    if(!out||key.size()!=111||key.substr(0,4)!="fp1:"||key[36]!=':'||key[45]!=':'||key[78]!=':')return false;
    Identity id;uint8_t epoch[4];
    if(!read_hex(key.data()+4,32,id.session.data())||!read_hex(key.data()+37,8,epoch)||
       !read_hex(key.data()+46,32,id.fish.data())||!read_hex(key.data()+79,32,id.prefab.data()))return false;
    id.epoch=creature_motion::get32(epoch);
    if(!id.epoch||!creature_motion::nonzero(id.session)||!creature_motion::nonzero(id.fish)||!creature_motion::nonzero(id.prefab))return false;
    *out=id;return true;
}
inline bool matches(const Identity& id,const Id& session,uint32_t epoch) {return id.session==session&&id.epoch==epoch;}
inline std::string guid(const Id& id) {
    auto s=hex(id);return s.substr(0,8)+"-"+s.substr(8,4)+"-"+s.substr(12,4)+"-"+s.substr(16,4)+"-"+s.substr(20);
}
inline bool item_matches(const std::string& text,const Id& id) {
    return text==hex(id)||text==guid(id);
}
inline bool valid(const locker_contents::Message& m) {
    Identity id;if(!identity(m.locker,&id)||m.deposit||!creature_motion::supported_tech(m.item.tech)||
        !item_matches(m.item.id,id.fish)||!m.items.empty()||!m.revision)return false;
    if(m.kind==locker_contents::Snapshot)return !m.recipient&&m.request&&m.request<=UINT32_MAX&&m.item.data.empty();
    if(!m.request)return false;
    switch(m.kind) {
        case locker_contents::Request:case locker_contents::Applied:case locker_contents::Cancel:
            return m.recipient&&!m.item.data.size();
        case locker_contents::Grant:return m.recipient&&!m.item.data.empty()&&m.item.data.size()<=locker_contents::kMaxItemBytes;
        case locker_contents::Done:case locker_contents::Reject:return m.item.data.size()<=locker_contents::kMaxItemBytes;
        default:return false;
    }
}
inline bool actor_in_reach(float distance_squared,double sample_age) {
    return std::isfinite(distance_squared)&&distance_squared>=0&&distance_squared<=kAuthorityDistance*kAuthorityDistance&&
        std::isfinite(sample_age)&&sample_age>=0&&sample_age<=.75;
}
inline bool pose_after_floor(uint32_t sequence,uint32_t floor) {
    return sequence==floor||creature_motion::newer(sequence,floor);
}
// Transaction messages are immutable after grant. Duplicate grants must resend
// Applied rather than create a second inventory object.
struct Pending {
    bool used=false,applied=false,cancelling=false,loading=false,committed=false;
    locker_contents::Message request;
    bool owns(const locker_contents::Message& m)const {
        return used&&request.request==m.request&&request.locker==m.locker&&request.item.tech==m.item.tech;
    }
    bool may_restore(const locker_contents::Message& grant)const {
        return owns(grant)&&!applied&&!cancelling&&!loading&&grant.kind==locker_contents::Grant&&valid(grant);
    }
};
} // namespace fish_pickup
