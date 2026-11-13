#pragma once

namespace bioreactor_sync {
// Loaded fuel is a real item: a remote demolition must never destroy it, and an
// unreadable container is not proof that the reactor is empty.
struct State {
    bool present=false;
    int fuel=0;  // ItemsContainer count; negative means the container could not be read
};
inline const char* removal_block(const State& s) {
    if(!s.present)return "no-bi-reactor";
    if(s.fuel<0)return "fuel-unreadable";
    if(s.fuel>0)return "fuel-loaded";
    return nullptr;
}
inline bool removal_allowed(const State& s) {
    return removal_block(s)==nullptr;
}
}
