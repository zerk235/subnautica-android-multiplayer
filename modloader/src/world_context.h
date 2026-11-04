#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
struct WorldContextWatch {
    bool initialized=false,loading=false;
    char slot[24]{};
};
inline bool world_context_transition(WorldContextWatch* state,const char* slot,bool loading) {
    if(!state || !slot || std::strlen(slot)>=sizeof(state->slot))return false;
    const bool changed=state->initialized &&
        ((loading&&!state->loading) || (slot[0] && state->slot[0] && std::strcmp(slot,state->slot)));
    state->initialized=true;state->loading=loading;
    std::snprintf(state->slot,sizeof(state->slot),"%s",slot);
    return changed;
}
