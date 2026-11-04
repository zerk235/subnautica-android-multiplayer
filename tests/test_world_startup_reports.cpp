#include <cassert>
#include <cstdio>
#include <vector>
std::vector<bool> stages;
unsigned failures=0;
bool log_save_subsystem(const char*,bool stage) {
    stages.push_back(stage);
    if (failures) { --failures; return false; }
    return true;
}
#include "modloader/src/world_startup_reports.inc"
int main() {
    WorldStartupReports state; int player_a=0,player_b=0;
    failures=2;
    assert(!tick_world_startup_reports(&state,&player_a,0)); // API not bound
    assert(state.successful==0 && stages.size()==1 && stages.back());
    assert(!tick_world_startup_reports(&state,&player_a,1) && stages.size()==1);
    assert(!tick_world_startup_reports(&state,&player_a,5)); // slot not ready
    assert(state.successful==0 && stages.back());
    assert(tick_world_startup_reports(&state,&player_a,10)); // real capture
    assert(state.successful==1 && stages.back());
    assert(tick_world_startup_reports(&state,&player_a,15)); // second report
    assert(state.successful==2 && !stages.back());
    assert(!tick_world_startup_reports(&state,&player_a,100) && stages.size()==4);
    assert(tick_world_startup_reports(&state,&player_b,101) && stages.back());
    assert(state.successful==1 && state.player==&player_b);
    assert(tick_world_startup_reports(&state,&player_b,102,"slot-old",1) && stages.back());
    assert(tick_world_startup_reports(&state,&player_b,107,"slot-old",1) && !stages.back());
    assert(!tick_world_startup_reports(&state,&player_b,200,"slot-old",1));
    assert(tick_world_startup_reports(&state,&player_b,201,"slot-new",2) && stages.back());
    assert(tick_world_startup_reports(&state,&player_b,206,"slot-new",2) && !stages.back());
    // Same player AND same slot, but a fresh native load must capture again.
    assert(tick_world_startup_reports(&state,&player_b,207,"slot-new",3) && stages.back());
    assert(state.successful==1 && state.context_generation==3);
    assert(!tick_world_startup_reports(&state,nullptr,200));
    assert(!tick_world_startup_reports(nullptr,&player_a,200));
    puts("actual startup scheduler: failed API/slot attempts retain capture, bounded retry, success stops capture, new world resets");
}
