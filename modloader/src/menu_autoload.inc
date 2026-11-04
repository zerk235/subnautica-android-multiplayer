// Both native MainMenuLoadButton.Load292522c and LoadMostRecentSavedGame2c87a78
// dispatch LoadGameAsync through STATIC UWE.CoroutineHost.StartCoroutine.
// MonoBehaviour.StartCoroutine(menu,...) loses its owner during scene unload.
using MenuLoadAsyncFn=void* (*)(void*,void*,void*,int32_t,int32_t,int32_t,void*);
using PersistentStartCoroutineFn=void* (*)(void*,void*);
Entry<MenuLoadAsyncFn> g_menu_load_game_async;
Entry<PersistentStartCoroutineFn> g_menu_start_persistent;
void* g_menu_main_field=nullptr;
void* g_menu_starting_field=nullptr;
bool g_menu_api_resolved=false;
double g_menu_api_retry_at=0.0;
ManagedFn g_slm_update_original=nullptr;
double g_autoload_started_at=0.0;
char g_autoload_slot[24]{};
uint32_t g_autoload_generation=0;
bool g_autoload_started_once=false;
void* autoload_field_on(struct Il2CppClass*,const char*,int);

bool resolve_menu_autoload_api() {
    if (g_menu_api_resolved) return true;
    const double now=monotonic_seconds();
    if (now<g_menu_api_retry_at) return false;
    g_menu_api_retry_at=now+2.0;
    auto* menu=find_class_in_domain("","uGUI_MainMenu");
    auto* host=find_class_in_domain("UWE","CoroutineHost");
    if (!menu || !host) return false;
    g_menu_main_field=autoload_field_on(menu,"main",0);
    g_menu_starting_field=autoload_field_on(menu,"isStartingNewGame",0x80);
    g_menu_load_game_async.bind(find_method(menu,"LoadGameAsync",5));
    g_menu_start_persistent.bind(find_method(host,"StartCoroutine",1));
    g_menu_api_resolved=g_menu_main_field && g_menu_starting_field &&
        g_menu_load_game_async && g_menu_start_persistent;
    if (g_menu_api_resolved)
        LOG_INFO("autoload: exact-slot API ready; persistent UWE.CoroutineHost owns load");
    return g_menu_api_resolved;
}

void menu_autoload_tick() {
    if (!resolve_menu_autoload_api()) return;
    void* menu=nullptr;
    il2cpp_field_static_get_value(g_menu_main_field,&menu);
    if (!menu) return;
    char slot[24]{};uint32_t generation=0;double raised_at=0.0;
    if (!net_world_autoload_peek(slot,sizeof(slot),&generation,&raised_at)) return;
    if (monotonic_seconds()-raised_at>180.0) {
        net_world_autoload_pop_if(slot,generation);
        LOG_WARN("autoload: exact slot=%s gen=%u request expired; manual load remains",slot,generation);
        return;
    }
    const auto session=net_session_state();
    if (session==kSessionHost || session==kSessionGuest) {
        net_world_autoload_pop_if(slot,generation);
        return;
    }
    if (session!=kSessionSlotReady) return;
    uint8_t starting=0;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(menu),g_menu_starting_field,&starting);
    if (starting) {
        net_world_autoload_pop_if(slot,generation);
        LOG_INFO("autoload: manual/new load already starting; exact slot=%s not launched",slot);
        return;
    }
    if (!world_slot_is_host_received(slot)) {
        net_world_autoload_pop_if(slot,generation);
        LOG_WARN("autoload: slot=%s has no host-received marker; manual load remains",slot);
        return;
    }
    WorldLoadFields fields{};
    if (!world_read_load_fields(slot,&fields) || !fields.valid) {
        net_world_autoload_pop_if(slot,generation);
        LOG_WARN("autoload: slot=%s has invalid load metadata; manual load remains",slot);
        return;
    }
    if (g_autoload_started_once && generation==g_autoload_generation && !strcmp(slot,g_autoload_slot)) {
        net_world_autoload_pop_if(slot,generation);
        return;
    }
    void* slot_string=il2cpp_string_new(slot);
    void* session_string=il2cpp_string_new(fields.session);
    if (!slot_string || !session_string) return;
    // Revalidate membership, exact slot and generation under the network lock.
    // This also refuses an already live world even before its save report arrives.
    if (!net_world_autoload_claim(slot,generation)) return;
    // Consume BEFORE starting: StartCoroutine may immediately enter MoveNext and
    // re-enter game callbacks. There is one attempt per exact transfer, even on failure.
    snprintf(g_autoload_slot,sizeof(g_autoload_slot),"%s",slot);
    g_autoload_generation=generation;
    g_autoload_started_once=true;
    try {
        void* routine=g_menu_load_game_async.call(menu,slot_string,session_string,
            fields.change_set,fields.game_mode,0,g_menu_load_game_async.method);
        if (!routine) {
            LOG_WARN("autoload: exact slot=%s produced no routine; manual load remains",slot);
            return;
        }
        g_autoload_started_at=monotonic_seconds();
        void* coroutine=g_menu_start_persistent.call(routine,g_menu_start_persistent.method);
        if (!coroutine) {
            g_autoload_started_at=0.0;
            LOG_WARN("autoload: persistent host refused slot=%s; manual load remains",slot);
            return;
        }
        LOG_INFO("autoload: started exact slot=%s gen=%u changeSet=%d mode=%d via UWE.CoroutineHost",
            slot,generation,fields.change_set,fields.game_mode);
    } catch (...) {
        g_autoload_started_at=0.0;
        LOG_WARN("autoload: native exception starting slot=%s; manual load remains",slot);
    }
}
