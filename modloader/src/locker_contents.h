#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// No Unity pointers cross the wire. A host reservation serializes transfers of
// one exact item; an unanswered grant is NEVER released by a timeout.
namespace locker_contents {
constexpr size_t kMaxItemBytes=32768, kMaxMessage=65536, kMaxItems=64;
enum Kind:uint8_t {Query=1,Snapshot,Fetch,ItemState,Request,Grant,Applied,Done,Reject,Cancel};
struct Item {std::string id; int32_t tech=0; std::vector<uint8_t> data;};
struct Message {
    Kind kind=Query;
    uint32_t recipient=0;
    uint64_t request=0,revision=0;
    bool deposit=false;
    std::string locker;
    Item item;
    std::vector<Item> items;
};
inline bool id_valid(const std::string& s) {
    if(s.empty()||s.size()>127)return false;
    for(unsigned char c:s)if(c<33||c>126)return false;
    return true;
}
inline void u32(std::vector<uint8_t>& b,uint32_t x){for(int i=0;i<4;++i)b.push_back(uint8_t(x>>(i*8)));}
inline void u64(std::vector<uint8_t>& b,uint64_t x){u32(b,uint32_t(x));u32(b,uint32_t(x>>32));}
inline uint32_t get32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
inline uint64_t get64(const uint8_t* p){return get32(p)|(uint64_t(get32(p+4))<<32);}
inline uint32_t checksum(const uint8_t* p,size_t n){uint32_t h=2166136261u;while(n--){h^=*p++;h*=16777619u;}return h;}
inline bool put_item(std::vector<uint8_t>& b,const Item& item,bool empty=false) {
    if((!empty&&!id_valid(item.id))||item.id.size()>127||item.data.size()>kMaxItemBytes)return false;
    b.push_back(uint8_t(item.id.size()));b.insert(b.end(),item.id.begin(),item.id.end());
    u32(b,uint32_t(item.tech));u32(b,uint32_t(item.data.size()));
    b.insert(b.end(),item.data.begin(),item.data.end());return b.size()<=kMaxMessage;
}
inline bool encode(const Message& m,std::vector<uint8_t>* out) {
    if(!out||m.kind<Query||m.kind>Cancel||!id_valid(m.locker)||m.items.size()>kMaxItems)return false;
    std::vector<uint8_t> b{1,uint8_t(m.kind),uint8_t(m.deposit),uint8_t(m.locker.size())};
    u32(b,m.recipient);u64(b,m.request);u64(b,m.revision);
    b.insert(b.end(),m.locker.begin(),m.locker.end());
    if(!put_item(b,m.item,true))return false;
    u32(b,uint32_t(m.items.size()));
    for(const auto& i:m.items)if(!put_item(b,i))return false;
    *out=std::move(b);return true;
}
struct Reader {
    const uint8_t* p;size_t left;
    bool take(size_t n,const uint8_t** out){if(n>left)return false;*out=p;p+=n;left-=n;return true;}
    bool item(Item* out,bool empty=false) {
        const uint8_t *a,*b,*c;
        if(!take(1,&a)||!take(*a,&b)||!take(8,&c))return false;
        out->id.assign(reinterpret_cast<const char*>(b),*a);out->tech=int32_t(get32(c));
        size_t n=get32(c+4);if(n>kMaxItemBytes||!take(n,&b))return false;
        out->data.assign(b,b+n);return (empty&&out->id.empty())||id_valid(out->id);
    }
};
inline bool decode(const std::vector<uint8_t>& b,Message* out) {
    if(!out||b.size()<24||b.size()>kMaxMessage||b[0]!=1||b[1]<Query||b[1]>Cancel||b[2]>1)return false;
    Message m;m.kind=Kind(b[1]);m.deposit=b[2];m.recipient=get32(b.data()+4);
    m.request=get64(b.data()+8);m.revision=get64(b.data()+16);
    Reader r{b.data()+24,b.size()-24};const uint8_t* p;
    if(!r.take(b[3],&p))return false;m.locker.assign(reinterpret_cast<const char*>(p),b[3]);
    if(!id_valid(m.locker)||!r.item(&m.item,true)||!r.take(4,&p))return false;
    size_t n=get32(p);if(n>kMaxItems)return false;
    for(size_t i=0;i<n;++i){Item item;if(!r.item(&item))return false;m.items.push_back(std::move(item));}
    if(r.left)return false;*out=std::move(m);return true;
}
struct Key {
    uint32_t peer=0;uint64_t request=0;
    bool operator<(const Key& x)const{return peer<x.peer||(peer==x.peer&&request<x.request);}
    bool operator==(const Key& x)const{return peer==x.peer&&request==x.request;}
};
struct Reservation {Key key;Message request;};
struct Authority {
    uint64_t revision=1;
    std::map<std::string,Item> items;
    bool reserved=false;
    Reservation pending;
    std::map<Key,Message> replies;
    bool initialize(const std::vector<Item>& baseline) {
        if(baseline.size()>kMaxItems)return false;
        std::map<std::string,Item> next;
        for(const auto& i:baseline)if(!id_valid(i.id)||i.tech<=0||!next.emplace(i.id,i).second)return false;
        items=std::move(next);return true;
    }
    Message reply(const Message& q,Kind kind,uint32_t peer) const {
        Message r=q;r.kind=kind;r.recipient=peer;r.revision=revision;r.items.clear();return r;
    }
    Message begin(uint32_t peer,const Message& q) {
        Key key{peer,q.request};auto old=replies.find(key);
        if(old!=replies.end())return old->second;
        if(reserved&&pending.key==key) return reply(pending.request,Grant,peer);
        auto existing=items.find(q.item.id);
        const bool allowed=q.kind==Request&&peer&&q.request&&q.revision==revision&&!reserved&&
            id_valid(q.item.id)&&q.item.tech>0&&revision!=UINT64_MAX&&
            (q.deposit?(existing==items.end()&&items.size()<kMaxItems&&!q.item.data.empty()):
             (existing!=items.end()&&existing->second.tech==q.item.tech));
        if(!allowed){auto r=reply(q,Reject,peer);replies.emplace(key,r);return r;}
        pending={key,q};reserved=true;return reply(q,Grant,peer);
    }
    Message finish(uint32_t peer,const Message& q,bool applied) {
        Key key{peer,q.request};auto old=replies.find(key);
        if(old!=replies.end())return old->second;
        if(!reserved||!(pending.key==key))return reply(q,Reject,peer);
        // Never trust a second payload: commit only the immutable granted request.
        auto request=pending.request;
        auto r=reply(request,applied?Done:Reject,peer);
        if(applied){if(request.deposit)items.emplace(request.item.id,request.item);else items.erase(request.item.id);r.revision=++revision;}
        reserved=false;pending={};replies.emplace(key,r);return r;
    }
};

// Carried inside the ordered WorldEvent stream. Bounded, strict fragments;
// backpressure is handled by the caller so a large item cannot evict history.
constexpr size_t kFrameHeader=24;
inline bool fragment(const std::vector<uint8_t>& blob,uint64_t id,size_t index,size_t capacity,std::vector<uint8_t>* out) {
    if(!out||!id||blob.empty()||blob.size()>kMaxMessage||capacity<=kFrameHeader)return false;
    size_t stride=capacity-kFrameHeader,offset=index*stride;if(offset>=blob.size())return false;
    std::vector<uint8_t> b;u32(b,0x31434c53);u64(b,id);u32(b,uint32_t(blob.size()));
    u32(b,checksum(blob.data(),blob.size()));u32(b,uint32_t(offset));
    b.insert(b.end(),blob.begin()+offset,blob.begin()+std::min(blob.size(),offset+stride));*out=std::move(b);return true;
}
struct Assembly {
    uint64_t id=0;uint32_t hash=0,total=0;std::vector<uint8_t> bytes;
    bool append(const uint8_t* frame,size_t length,size_t capacity,std::vector<uint8_t>* result) {
        if(!frame||length<kFrameHeader||length>capacity||capacity<=kFrameHeader||get32(frame)!=0x31434c53)return false;
        uint64_t next=get64(frame+4);uint32_t n=get32(frame+12),h=get32(frame+16),off=get32(frame+20);
        if(!next||!n||n>kMaxMessage||off>=n||off%(capacity-kFrameHeader)||length!=kFrameHeader+std::min(size_t(n-off),capacity-kFrameHeader))return false;
        if(!off){id=next;total=n;hash=h;bytes.clear();}
        if(id!=next||total!=n||hash!=h||off!=bytes.size())return false;
        bytes.insert(bytes.end(),frame+kFrameHeader,frame+length);
        if(bytes.size()==total){if(checksum(bytes.data(),bytes.size())!=hash){*this={};return false;}*result=std::move(bytes);*this={};}
        return true;
    }
};
} // namespace locker_contents
