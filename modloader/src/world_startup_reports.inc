#include <cstdint>
#include <cstring>
#include <cstdio>
// Installing the Player.Update hook can race API resolution. An early failed
// report must not consume the only capture attempt for this player/world.
struct WorldStartupReports {
    void* player=nullptr;
    uint32_t context_generation=0;
    char slot[24]{};
    unsigned successful=0;
    double next_attempt=0;
};
bool tick_world_startup_reports(WorldStartupReports* state,void* player,double now,
                               const char* slot="",uint32_t generation=0) {
    if (!state || !player) return false;
    if(!slot || strlen(slot)>=sizeof(state->slot)) return false;
    if (state->player!=player || state->context_generation!=generation || strcmp(state->slot,slot)) {
        *state=WorldStartupReports{};
        state->player=player;
        state->context_generation=generation;
        snprintf(state->slot,sizeof(state->slot),"%s",slot);
    }
    if (state->successful>=2 || now<state->next_attempt) return false;
    state->next_attempt=now+5.0;
    bool capture=state->successful==0;
    if (!log_save_subsystem("world",capture)) return false;
    ++state->successful;
    return true;
}
