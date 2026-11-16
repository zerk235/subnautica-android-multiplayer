#pragma once
// Pose wire format and guest interpolation shared by runtime sync and the diagnostic.
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

namespace creature_motion {
using Id=std::array<uint8_t,16>;
enum class Species : uint8_t { Boomerang, Peeper, Bladderfish, Unknown = 255 };
constexpr std::array kLiveSpecies{Species::Boomerang, Species::Peeper, Species::Bladderfish};
constexpr int kBoomerangTech = 2510;
constexpr int kPeeperTech = 2504;
constexpr int kBladderfishTech = 2515;
constexpr int tech_for_species(Species species) {
    switch (species) {
        case Species::Boomerang: return kBoomerangTech;
        case Species::Peeper: return kPeeperTech;
        case Species::Bladderfish: return kBladderfishTech;
        default: return 0;
    }
}
constexpr Species species_for_tech(int tech) {
    for (auto species : kLiveSpecies)
        if (tech_for_species(species) == tech) return species;
    return Species::Unknown;
}
constexpr bool supported_tech(int tech) {
    return species_for_tech(tech) != Species::Unknown;
}
enum Kind:uint8_t { Request=1, Pose=2, End=3, Refuse=4, LivePose=5 };
constexpr size_t kLiveFishLimit = 40;
constexpr size_t kLiveSendPerTick = 8;
constexpr size_t kLiveQueueCapacity = 64;
constexpr size_t kControlQueueCapacity = 32;
constexpr double kLivePoseInterval = .1;
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
    Species species=Species::Boomerang;
    uint32_t host=0,epoch=0,sender=0,target=0,sequence=0,elapsed_ms=0;
    uint64_t nonce=0;
    Id session{},fish{},prefab{};
    std::array<float,3> position{};
    std::array<float,4> rotation{0,0,0,1};
};
inline bool nonzero(const Id& id){for(auto b:id)if(b)return true;return false;}
// Preserve per-fish deadlines when distance sorting changes. A slow frame never
// catches up by flooding old poses; each selected fish becomes due once again.
class LiveSchedule {
    struct Slot { Id id{}; size_t index=0; double due=0; };
    std::array<Slot,kLiveFishLimit> slots_{};
    size_t count_=0,cursor_=0;
public:
    void refresh(const Id* ids,size_t count) {
        const auto previous=slots_;const size_t old_count=count_;
        count_=cursor_=0;
        if(!ids)return;
        for(size_t i=0;i<std::min(count,kLiveFishLimit);++i) {
            if(!nonzero(ids[i]))continue;
            bool duplicate=false;
            for(size_t n=0;n<count_;++n)if(slots_[n].id==ids[i])duplicate=true;
            if(duplicate)continue;
            Slot slot;slot.id=ids[i];slot.index=i;
            for(size_t n=0;n<old_count;++n)if(previous[n].id==slot.id)slot.due=previous[n].due;
            slots_[count_++]=slot;
        }
    }
    bool take(double now,size_t* index) {
        if(!index||!std::isfinite(now)||now<0)return false;
        for(size_t n=0;n<count_;++n) {
            const size_t at=(cursor_+n)%count_;
            auto& slot=slots_[at];
            if(now+1e-9<slot.due)continue;
            slot.due=now+kLivePoseInterval;*index=slot.index;
            cursor_=(at+1)%count_;return true;
        }
        return false;
    }
};
inline bool newer(uint32_t a,uint32_t b){const uint32_t d=a-b;return d&&d<0x80000000u;}
inline bool valid(const Message& m) {
    if(m.kind<Request||m.kind>LivePose||!m.host||!m.epoch||!m.sender||
       !m.nonce||!nonzero(m.session)||!nonzero(m.fish)||!nonzero(m.prefab))return false;
    if (m.kind == LivePose ? !tech_for_species(m.species) : m.species != Species::Boomerang)
        return false;
    if (m.kind == LivePose) {
        if (m.target || m.sender != m.host || m.nonce != 1 || !m.sequence || m.elapsed_ms)
            return false;
    } else if (!m.target || m.sender == m.target) {
        return false;
    }
    if(m.elapsed_ms>60000||(m.kind==Pose&&!m.sequence))return false;
    for(float v:m.position)if(!std::isfinite(v)||std::fabs(v)>100000)return false;
    float norm=0;for(float v:m.rotation){if(!std::isfinite(v))return false;norm+=v*v;}
    return norm>.99f&&norm<1.01f;
}
inline void put32(uint8_t* p,uint32_t v){for(int i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
inline uint32_t get32(const uint8_t* p){uint32_t v=0;for(int i=0;i<4;++i)v|=uint32_t(p[i])<<(8*i);return v;}
inline bool encode(const Message& m,std::vector<uint8_t>* out) {
    if(!out||!valid(m))return false;
    out->assign(kWireSize,0);auto* p=out->data();p[0]=m.kind;p[1]=uint8_t(m.species);
    uint32_t values[]={m.host,m.epoch,m.sender,m.target,m.sequence,m.elapsed_ms};
    for(int i=0;i<6;++i)put32(p+4+i*4,values[i]);
    put32(p+28,uint32_t(m.nonce));put32(p+32,uint32_t(m.nonce>>32));
    std::memcpy(p+36,m.session.data(),16);std::memcpy(p+52,m.fish.data(),16);std::memcpy(p+68,m.prefab.data(),16);
    for(int i=0;i<7;++i){float v=i<3?m.position[i]:m.rotation[i-3];uint32_t b;std::memcpy(&b,&v,4);put32(p+84+4*i,b);}
    return true;
}
inline bool decode(const uint8_t* p,size_t size,Message* out) {
    if(!p||!out||size!=kWireSize||p[2]||p[3])return false;
    Message m;m.kind=Kind(p[0]);m.species=Species(p[1]);m.host=get32(p+4);m.epoch=get32(p+8);m.sender=get32(p+12);m.target=get32(p+16);
    m.sequence=get32(p+20);m.elapsed_ms=get32(p+24);m.nonce=uint64_t(get32(p+28))|(uint64_t(get32(p+32))<<32);
    std::memcpy(m.session.data(),p+36,16);std::memcpy(m.fish.data(),p+52,16);std::memcpy(m.prefab.data(),p+68,16);
    for(int i=0;i<7;++i){uint32_t b=get32(p+84+4*i);float v;std::memcpy(&v,&b,4);if(i<3)m.position[i]=v;else m.rotation[i-3]=v;}
    if(!valid(m))return false;*out=m;return true;
}
inline bool same_run(const Message& a,const Message& b) {
    return a.host==b.host&&a.epoch==b.epoch&&a.nonce==b.nonce&&a.session==b.session&&a.fish==b.fish&&a.prefab==b.prefab&&a.species==b.species;
}
// Caller provides synchronization. Control and live traffic cannot consume
// each other's budget; a slow Unity frame keeps only the newest pose per fish.
class Mailbox {
    std::deque<Message> live_, control_;
public:
    bool push(const Message& message) {
        if (!valid(message)) return false;
        const bool live = message.kind == LivePose;
        auto& queue = live ? live_ : control_;
        if (live || message.kind == Pose) {
            for (auto& old : queue) {
                if (old.kind != message.kind || old.target != message.target || !same_run(old, message)) continue;
                if (newer(message.sequence, old.sequence)) old = message;
                return true;
            }
        }
        if (queue.size() >= (live ? kLiveQueueCapacity : kControlQueueCapacity)) return false;
        queue.push_back(message);
        return true;
    }
    bool take(bool live, Message* out) {
        if (!out) return false;
        auto& queue = live ? live_ : control_;
        if (queue.empty()) return false;
        *out = queue.front(); queue.pop_front(); return true;
    }
};
// Survives local stream-out/rebind. Reset only on session/world change so an
// old UDP pose cannot become the first pose of a newly rebound local instance.
class SequenceGate {
    struct Seen { Id prefab; Species species; uint32_t sequence; };
    std::map<Id, Seen> seen_;
public:
    bool accept(const Message& pose) {
        if (!valid(pose) || pose.kind != LivePose) return false;
        auto it = seen_.find(pose.fish);
        if (it == seen_.end()) {
            if (seen_.size() >= 4096) return false;
            seen_.emplace(pose.fish, Seen{pose.prefab, pose.species, pose.sequence});
            return true;
        }
        if (it->second.prefab != pose.prefab || it->second.species != pose.species ||
            !newer(pose.sequence, it->second.sequence)) return false;
        it->second.sequence = pose.sequence;
        return true;
    }
};
inline bool permitted(const Message& m,uint32_t local,uint32_t host,const Id& session,uint32_t epoch) {
    if(!valid(m)||!local||m.host!=host||m.session!=session||m.epoch!=epoch)return false;
    if (m.kind == LivePose) return local != host;
    if (m.target != local) return false;
    return local==host ? m.kind==Request&&m.sender!=host : m.kind!=Request&&m.sender==host;
}
// Live fish render from a short receive history. Brief gaps can use bounded
// position prediction. The diagnostic keeps its original interpolation policy.
class Receiver {
    struct TimedPose { Message pose; double at=0; };
    static constexpr double kLiveDelay = .085;
    static constexpr double kLivePrediction = .065;
    static constexpr float kMaxPredictedDistance = .20f;
    Message run_,resume_from_;
    std::array<TimedPose,4> history_{};
    size_t count_=0;
    bool resuming_=false;
    double resume_started_=0;
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
        const bool live = run_.kind == LivePose;
        const bool expected = live ? pose.kind == LivePose
            : pose.kind == Pose && pose.target == run_.sender;
        const auto& latest = history_[0];
        if(!valid(pose)||!expected||!same_run(run_,pose)||pose.sender!=run_.host||
           !std::isfinite(now)||(count_&&(now<latest.at||!newer(pose.sequence,latest.pose.sequence))))return false;
        if(count_&&now-latest.at>(live ? .25 : .5)){
            if(!sample(now,&resume_from_))return false;
            resume_started_=now;resuming_=true;
            if(live) count_=0;
        }
        if (count_ && now == latest.at) {
            history_[0].pose = pose;
        } else {
            count_=std::min(count_+1,history_.size());
            for(size_t i=count_-1;i>0;--i) history_[i]=history_[i-1];
            history_[0]={pose,now};
        }
        return true;
    }
    bool sample(double now,Message* out) const {
        if(!count_||!out||!std::isfinite(now))return false;
        const bool live = run_.kind == LivePose;
        const double render_at = now - (live ? kLiveDelay : .12);
        const auto& latest = history_[0];
        *out = latest.pose;
        if (count_ > 1) {
            if (render_at < latest.at) {
                size_t older = 1;
                while (live && older + 1 < count_ && render_at < history_[older].at) ++older;
                const auto& a = history_[older];
                const auto& b = history_[older - 1];
                const float t = float(std::clamp((render_at - a.at) / (b.at - a.at), 0.0, 1.0));
                if (!blend(a.pose, b.pose, t, out)) return false;
                out->sequence = latest.pose.sequence;
            } else if (live) {
                const auto& previous = history_[1];
                const double span = latest.at - previous.at;
                if (span >= .02 && span <= .25) {
                    const float fraction = float(std::min(render_at - latest.at, kLivePrediction) / span);
                    std::array<float,3> step{};
                    float distance_squared = 0;
                    for (size_t i=0;i<step.size();++i) {
                        step[i]=(latest.pose.position[i]-previous.pose.position[i])*fraction;
                        distance_squared+=step[i]*step[i];
                    }
                    const float scale = distance_squared > kMaxPredictedDistance*kMaxPredictedDistance
                        ? kMaxPredictedDistance/std::sqrt(distance_squared) : 1;
                    for (size_t i=0;i<step.size();++i) out->position[i]+=step[i]*scale;
                }
            }
        }
        const double resume_duration = live ? .18 : .35;
        if(resuming_&&now-resume_started_<resume_duration){
            float resume_t=float(std::clamp((now-resume_started_)/resume_duration,0.0,1.0));
            return blend(resume_from_,*out,resume_t,out);
        }
        return true;
    }
    double age(double now)const{return count_?now-history_[0].at:1e9;}
    bool has_pose()const{return count_!=0;}
};
} // namespace creature_motion
