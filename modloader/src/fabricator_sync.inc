#include "fabricator_state.h"

struct FabricatorApi {
    bool ready=false,failed=false;
    void *type=nullptr,*logic_field=nullptr,*initialized=nullptr,*opened=nullptr;
    void *relay=nullptr,*num_crafted=nullptr,*picking_up=nullptr;
    void *ui_main=nullptr,*ui_menu=nullptr;
    Entry<BuildObjectFn> logic,menu_client;
    Entry<uint8_t (*)(void*,void*)> menu_selected;
    Entry<uint8_t (*)(void*,void*)> in_progress;
} g_fabricator_api;

bool build_fabricator_resolve() {
    auto& a=g_fabricator_api;if(a.ready)return true;
    auto* fabricator=build_class("Fabricator");auto* ghost=build_class("GhostCrafter");
    auto* crafter=build_class("Crafter");auto* logic=build_class("CrafterLogic");
    auto* ui=build_class("uGUI");auto* menu=build_class("uGUI_CraftingMenu");
    auto* input=build_class("uGUI_InputGroup");
    if(!fabricator||!ghost||!crafter||!logic||!ui||!menu||!input)return false;
    a.type=il2cpp_type_get_object(il2cpp_class_get_type(fabricator));
    a.logic_field=build_field_on(crafter,"crafterLogic",0x28);
    a.initialized=build_field_on(crafter,"_initialized",0x30);
    a.opened=build_field_on(ghost,"_opened",0x70);
    a.relay=build_field_on(ghost,"powerRelay",0x68);
    a.num_crafted=build_field_on(logic,"numCrafted",0x54);
    a.picking_up=build_field_on(logic,"pickingUp",0x68);
    a.ui_main=build_field_on(ui,"_main",0x28);
    a.ui_menu=build_field_on(ui,"craftingMenu",0x90);
    a.logic.bind(find_method(crafter,"get_logic",0));
    a.in_progress.bind(find_method(logic,"get_inProgress",0));
    a.menu_client.bind(find_method(menu,"get_client",0));
    a.menu_selected.bind(find_method(input,"get_selected",0));
    a.ready=a.type&&a.logic_field&&a.initialized&&a.opened&&a.relay&&
        a.num_crafted&&a.picking_up&&a.logic&&a.in_progress&&a.ui_main&&a.ui_menu&&
        a.menu_client&&a.menu_selected&&il2cpp_field_static_get_value;
    if(a.ready)LOG_INFO("fabricator: API ready tech=1502; native initialization and busy-removal guard");
    return a.ready;
}

void* build_fabricator_component(void* go,bool require_ready) {
    if(!build_fabricator_resolve())return nullptr;
    void* list=g_locker_api.children.call(go,g_fabricator_api.type,true,g_locker_api.children.method);
    if(!list||g_array_get_length.call(list,g_array_get_length.method)!=1)return nullptr;
    void* component=g_array_get_value.call(list,0,g_array_get_value.method);
    if(!build_live(component))return nullptr;
    void* serialized_logic=nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(component),g_fabricator_api.logic_field,&serialized_logic);
    if(!build_live(serialized_logic))return nullptr;
    if(require_ready) {
        bool initialized=false;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(component),g_fabricator_api.initialized,&initialized);
        if(!initialized||!build_live(g_fabricator_api.logic.call(component,g_fabricator_api.logic.method)))return nullptr;
    }
    return component;
}

bool build_fabricator_removal_ready(void* go,const char* id) {
    void* f=build_fabricator_component(go,true);
    if(!f){LOG_INFO("fabricator: removal state id=%s block=component-not-ready",id);return false;}
    void* logic=g_fabricator_api.logic.call(f,g_fabricator_api.logic.method);
    fabricator_sync::State state{};
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(f),g_fabricator_api.initialized,&state.initialized);
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(f),g_fabricator_api.opened,&state.visual_open);
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(logic),g_fabricator_api.num_crafted,&state.num_crafted);
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(logic),g_fabricator_api.picking_up,&state.picking_up);
    state.in_progress=g_fabricator_api.in_progress.call(logic,g_fabricator_api.in_progress.method)!=0;
    void *ui=nullptr,*menu=nullptr;
    il2cpp_field_static_get_value(g_fabricator_api.ui_main,&ui);
    if(build_live(ui))il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(ui),g_fabricator_api.ui_menu,&menu);
    if(!build_live(menu)){LOG_INFO("fabricator: removal state id=%s block=menu-unavailable",id);return false;}
    const void* client=g_fabricator_api.menu_client.call(menu,g_fabricator_api.menu_client.method);
    state.menu_open=client==f&&g_fabricator_api.menu_selected.call(menu,g_fabricator_api.menu_selected.method);
    const char* block=fabricator_sync::removal_block(state);
    LOG_INFO("fabricator: removal state id=%s block=%s initialized=%u menu=%u visual_open=%u crafting=%u pickup=%u output=%d",
        id,block?block:"none",unsigned(state.initialized),unsigned(state.menu_open),unsigned(state.visual_open),
        unsigned(state.in_progress),unsigned(state.picking_up),state.num_crafted);
    return fabricator_sync::removal_allowed(state);
}

void build_fabricator_log_ready(void* go,const char* id) {
    void* f=build_fabricator_component(go,true);if(!f)return;
    void* relay=nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(f),g_fabricator_api.relay,&relay);
    LOG_INFO("fabricator: ready id=%s initialized=1 power_relay=%u",id,unsigned(build_live(relay)));
}
