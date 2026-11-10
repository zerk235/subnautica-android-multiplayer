#pragma once

namespace player_marker {
enum class Command { Ignore, On, Hud, Off, Probe, Refused };
inline bool space(char c) { return c==' '||c=='\t'||c=='\r'||c=='\n'; }
inline bool equals(const char* data,unsigned size,const char* word) {
    unsigned n=0;while(word[n])++n;
    if(size!=n)return false;
    for(unsigned i=0;i<n;++i)if(data[i]!=word[i])return false;
    return true;
}
inline Command parse(const char* data,unsigned size) {
    if(!data||size>32)return Command::Ignore;
    while(size&&space(*data)){++data;--size;}
    while(size&&space(data[size-1]))--size;
    if(equals(data,size,"on"))return Command::On;
    if(equals(data,size,"hud"))return Command::Hud;
    if(equals(data,size,"off"))return Command::Off;
    if(equals(data,size,"probe"))return Command::Probe;
    if(equals(data,size,"ping")||equals(data,size,"nitrox")||
       equals(data,size,"beacon")||equals(data,size,"cleanup"))return Command::Refused;
    return Command::Ignore;
}
struct Control {
    bool text_enabled=false,hud_enabled=true,probe_pending=false;
    void apply(Command command) {
        if(command==Command::On){text_enabled=true;hud_enabled=false;}
        else if(command==Command::Hud){hud_enabled=true;text_enabled=false;}
        else if(command==Command::Off){text_enabled=false;hud_enabled=false;probe_pending=false;}
        else if(command==Command::Probe)probe_pending=true;
    }
    bool active()const{return text_enabled||hud_enabled||probe_pending;}
};
struct PoseLease {
    unsigned peer=0,sequence=0,context=0;
    bool seen=false;
    double last=0;
    bool update(unsigned id,unsigned seq,unsigned world,double now,double timeout) {
        if(!seen||id!=peer||world!=context||seq!=sequence) {
            seen=true;peer=id;sequence=seq;context=world;last=now;
        }
        return now>=last&&now-last<=timeout;
    }
    void clear(){seen=false;}
};
} // namespace player_marker
