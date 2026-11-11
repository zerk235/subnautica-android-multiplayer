#pragma once

namespace fabricator_sync {
// Incomplete initialization and uncollected output are both reasons to defer
// remote demolition. Never clear the native crafter just to apply a removal.
struct State {
    bool initialized=false,menu_open=false,in_progress=false,picking_up=false;
    int num_crafted=0;
    bool visual_open=false; // GhostCrafter._opened stays true while the player is nearby.
};
inline const char* removal_block(const State& s) {
    if(!s.initialized)return "not-initialized";
    if(s.menu_open)return "menu-open";
    if(s.in_progress)return "crafting";
    if(s.picking_up)return "pickup-in-progress";
    if(s.num_crafted<0)return "invalid-output-count";
    if(s.num_crafted>0)return "uncollected-output";
    return nullptr;
}
inline bool removal_allowed(const State& s) {
    return removal_block(s)==nullptr;
}
}
