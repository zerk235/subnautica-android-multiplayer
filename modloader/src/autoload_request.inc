// The pump and game thread exchange one exact slot/generation. A stale consumer
// must never discard a newer transfer that arrived after its peek.
struct AutoLoadRequest {
    pthread_mutex_t lock;
    bool pending;
    char slot[24];
    uint32_t generation;
    double raised_at;
};
AutoLoadRequest g_auto_load_request = {PTHREAD_MUTEX_INITIALIZER};
void net_world_autoload_cancel() {
    pthread_mutex_lock(&g_auto_load_request.lock);
    g_auto_load_request.pending=false;
    pthread_mutex_unlock(&g_auto_load_request.lock);
}

void net_world_autoload_notify(const char* slot, uint32_t generation) {
    if (!slot || !*slot || strlen(slot)>=sizeof(g_auto_load_request.slot)) return;
    pthread_mutex_lock(&g_auto_load_request.lock);
    g_auto_load_request.pending = true;
    snprintf(g_auto_load_request.slot, sizeof(g_auto_load_request.slot), "%s", slot);
    g_auto_load_request.generation = generation;
    g_auto_load_request.raised_at = monotonic_seconds();
    pthread_mutex_unlock(&g_auto_load_request.lock);
}

bool net_world_autoload_peek(char* slot_out, size_t capacity, uint32_t* generation_out,
                             double* raised_at_out) {
    pthread_mutex_lock(&g_auto_load_request.lock);
    bool pending = g_auto_load_request.pending;
    if (pending) {
        if (slot_out && capacity) snprintf(slot_out, capacity, "%s", g_auto_load_request.slot);
        if (generation_out) *generation_out = g_auto_load_request.generation;
        if (raised_at_out) *raised_at_out = g_auto_load_request.raised_at;
    }
    pthread_mutex_unlock(&g_auto_load_request.lock);
    return pending;
}

bool net_world_autoload_pop_if(const char* slot, uint32_t generation) {
    pthread_mutex_lock(&g_auto_load_request.lock);
    bool matches = slot && g_auto_load_request.pending &&
        g_auto_load_request.generation==generation && !strcmp(slot,g_auto_load_request.slot);
    if (matches) g_auto_load_request.pending=false;
    pthread_mutex_unlock(&g_auto_load_request.lock);
    return matches;
}

bool net_world_autoload_claim(const char* slot, uint32_t generation) {
    pthread_mutex_lock(&g_session.lock);
    const bool eligible=slot && g_session.state==kSessionSlotReady &&
        g_session.active && g_session.local_member && g_session.join_accepted &&
        !g_session.local_host && !g_session.local_has_world && !g_session.world_active &&
        !g_session.explicit_leave && !strcmp(slot,g_session.slot);
    const bool taken=eligible && net_world_autoload_pop_if(slot,generation);
    pthread_mutex_unlock(&g_session.lock);
    return taken;
}
