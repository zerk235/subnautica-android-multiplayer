#pragma once
// Finite, explicitly armed one-fish experiment. Not the production creature registry.
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace creature_motion {
using Id=std::array<uint8_t,16>;
enum Kind:uint8_t { Request=1, Pose=2, End=3, Refuse=4 };
constexpr size_t kWireSize=112;
constexpr double kRunSeconds=60, kGuestDeadline=65;
inline bool controlled_gap(bool enabled,double elapsed) {
    return enabled&&std::isfinite(elapsed)&&elapsed>=20&&elapsed<25;
}
enum class Bearing { Ahead, Right, Behind, Left };
inline Bearing bearing(float dx,float dz,float view_yaw) {
    const float angle=std::remainder(std::atan2(dx,dz)*57.2957795131f-view_yaw,360.0f);
    if(std::fabs(angle)<=25)return Bearing::Ahead;
    if(std::fabs(angle)>=155)return Bearing::Behind;
    return angle>0?Bearing::Right:Bearing::Left;
}
struct Message {
    Kind kind=Request;
    uint32_t host=0,epoch=0,sender=0,target=0,sequence=0,elapsed_ms=0;
    uint64_t nonce=0;
    Id session{},fish{},prefab{};
    std::array<float,3> position{};
    std::array<float,4> rotation{0,0,0,1};
};
inline bool nonzero(const Id& id){for(auto b:id)if(b)return true;return false;}
inline bool newer(uint32_t a,uint32_t b){const uint32_t d=a-b;return d&&d<0x80000000u;}
inline bool valid(const Message& m) {
    if(m.kind<Request||m.kind>Refuse||!m.host||!m.epoch||!m.sender||!m.target||
       m.sender==m.target||!m.nonce||!nonzero(m.session)||!nonzero(m.fish)||!nonzero(m.prefab))return false;
    if(m.elapsed_ms>60000||(m.kind==Pose&&!m.sequence))return false;
    for(float v:m.position)if(!std::isfinite(v)||std::fabs(v)>100000)return false;
    float norm=0;for(float v:m.rotation){if(!std::isfinite(v))return false;norm+=v*v;}
    return norm>.99f&&norm<1.01f;
}
inline void put32(uint8_t* p,uint32_t v){for(int i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
inline uint32_t get32(const uint8_t* p){uint32_t v=0;for(int i=0;i<4;++i)v|=uint32_t(p[i])<<(8*i);return v;}
inline bool encode(const Message& m,std::vector<uint8_t>* out) {
    if(!out||!valid(m))return false;
    out->assign(kWireSize,0);auto* p=out->data();p[0]=m.kind;
    uint32_t values[]={m.host,m.epoch,m.sender,m.target,m.sequence,m.elapsed_ms};
    for(int i=0;i<6;++i)put32(p+4+i*4,values[i]);
    put32(p+28,uint32_t(m.nonce));put32(p+32,uint32_t(m.nonce>>32));
    std::memcpy(p+36,m.session.data(),16);std::memcpy(p+52,m.fish.data(),16);std::memcpy(p+68,m.prefab.data(),16);
    for(int i=0;i<7;++i){float v=i<3?m.position[i]:m.rotation[i-3];uint32_t b;std::memcpy(&b,&v,4);put32(p+84+4*i,b);}
    return true;
}
inline bool decode(const uint8_t* p,size_t size,Message* out) {
    if(!p||!out||size!=kWireSize||p[1]||p[2]||p[3])return false;
    Message m;m.kind=Kind(p[0]);m.host=get32(p+4);m.epoch=get32(p+8);m.sender=get32(p+12);m.target=get32(p+16);
    m.sequence=get32(p+20);m.elapsed_ms=get32(p+24);m.nonce=uint64_t(get32(p+28))|(uint64_t(get32(p+32))<<32);
    std::memcpy(m.session.data(),p+36,16);std::memcpy(m.fish.data(),p+52,16);std::memcpy(m.prefab.data(),p+68,16);
    for(int i=0;i<7;++i){uint32_t b=get32(p+84+4*i);float v;std::memcpy(&v,&b,4);if(i<3)m.position[i]=v;else m.rotation[i-3]=v;}
    if(!valid(m))return false;*out=m;return true;
}
inline bool same_run(const Message& a,const Message& b) {
    return a.host==b.host&&a.epoch==b.epoch&&a.nonce==b.nonce&&a.session==b.session&&a.fish==b.fish&&a.prefab==b.prefab;
}
inline bool permitted(const Message& m,uint32_t local,uint32_t host,const Id& session,uint32_t epoch) {
    if(!valid(m)||m.target!=local||m.host!=host||m.session!=session||m.epoch!=epoch)return false;
    return local==host ? m.kind==Request&&m.sender!=host : m.kind!=Request&&m.sender==host;
}
// Reception time interpolation; never extrapolates or re-enables native simulation.
class Receiver {
    Message run_,previous_,latest_,resume_from_;
    bool has_=false;
    bool resuming_=false;
    double previous_at_=0,latest_at_=0,resume_started_=0;
    static bool blend(const Message& a,const Message& b,float t,Message* out) {
        Message result=b;
        for(int i=0;i<3;++i)result.position[i]=a.position[i]+t*(b.position[i]-a.position[i]);
        float dot=0;for(int i=0;i<4;++i)dot+=a.rotation[i]*b.rotation[i];
        float norm=0;for(int i=0;i<4;++i){result.rotation[i]=(1-t)*a.rotation[i]+t*(dot<0?-b.rotation[i]:b.rotation[i]);norm+=result.rotation[i]*result.rotation[i];}
        if(norm<1e-8f)return false;
        for(auto& v:result.rotation)v/=std::sqrt(norm);
        *out=result;return true;
    }
public:
    void reset(const Message& run){*this=Receiver{};run_=run;}
    bool accept(const Message& pose,double now) {
        if(!valid(pose)||pose.kind!=Pose||!same_run(run_,pose)||pose.sender!=run_.host||
           pose.target!=run_.sender||!std::isfinite(now)||(has_&&(now<latest_at_||!newer(pose.sequence,latest_.sequence))))return false;
        if(has_&&now-latest_at_>.5){
            if(!sample(now,&resume_from_))return false;
            resume_started_=now;resuming_=true;
        }
        previous_=has_?latest_:pose;previous_at_=has_?latest_at_:now;
        latest_=pose;latest_at_=now;has_=true;return true;
    }
    bool sample(double now,Message* out) const {
        if(!has_||!out||!std::isfinite(now))return false;
        double span=latest_at_-previous_at_;
        float t=span>0?float(std::clamp((now-.12-previous_at_)/span,0.0,1.0)):1;
        if(!blend(previous_,latest_,t,out))return false;
        if(resuming_&&now-resume_started_<.35){
            float resume_t=float(std::clamp((now-resume_started_)/.35,0.0,1.0));
            return blend(resume_from_,*out,resume_t,out);
        }
        return true;
    }
    double age(double now)const{return has_?now-latest_at_:1e9;}
    bool has_pose()const{return has_;}
};
} // namespace creature_motion
