#include "player_marker_icon.h"

struct HudIconRect { float x,y,width,height; };
struct HudIconPivot { float x,y; };
struct PeerHudIcon {
    ManagedRoot texture,sprite,widget;
    Entry<void (*)(void*,int,int,int,bool,bool,void*)> texture_ctor;
    Entry<void (*)(void*,void*,void*)> load_raw;
    Entry<void (*)(void*,bool,bool,void*)> apply;
    Entry<void* (*)(void*,HudIconRect,HudIconPivot,void*)> create_sprite;
    Entry<void (*)(void*,void*,void*)> set_icon;
    Entry<void* (*)(void*,void*)> get_sprite;
    Il2CppClass *texture_class=nullptr,*byte_class=nullptr;
    void* widget_icon=nullptr;
    bool ready=false;
    double next_tick=0,next_error=0;
} g_peer_hud_icon;

// Texture2D has both (..,int mipCount,bool) and (..,bool mipChain,bool)
// constructors of the same arity. Match every parameter, not just the first one.
Il2CppMethodInfo* hud_icon_method(Il2CppClass* klass,const char* name,
                                std::initializer_list<const char*> types) {
    if(!klass||!il2cpp_class_get_methods||!il2cpp_method_get_name||
       !il2cpp_method_get_param_count||!il2cpp_method_get_param)return nullptr;
    void* it=nullptr;
    for(auto* method=il2cpp_class_get_methods(klass,&it);method;
        method=il2cpp_class_get_methods(klass,&it)) {
        const char* candidate=il2cpp_method_get_name(method);
        if(!candidate||strcmp(candidate,name)||il2cpp_method_get_param_count(method)!=types.size())continue;
        unsigned i=0;bool match=true;
        for(const char* type:types) {
            char actual[128]{};
            if(!il2cpp_type_name(il2cpp_method_get_param(method,i++),actual,sizeof(actual))||strcmp(actual,type)) {
                match=false;break;
            }
        }
        if(match)return method;
    }
    return nullptr;
}

bool hud_icon_resolve() {
    auto& a=g_peer_hud_icon;
    if(a.ready)return true;
    a.texture_class=find_class_in_domain("UnityEngine","Texture2D");
    a.byte_class=find_class_in_domain("System","Byte");
    auto* sprite=find_class_in_domain("UnityEngine","Sprite");
    auto* widget=find_class_in_domain("","uGUI_Ping");
    auto* icon=find_class_in_domain("","uGUI_Icon");
    if(!il2cpp_array_new||!a.texture_class||!a.byte_class||!sprite||!widget||!icon)return false;
    a.texture_ctor.bind(hud_icon_method(a.texture_class,".ctor",{
        "system.int32","system.int32","unityengine.textureformat","system.boolean","system.boolean"}));
    a.load_raw.bind(hud_icon_method(a.texture_class,"LoadRawTextureData",{"system.byte[]"}));
    a.apply.bind(hud_icon_method(a.texture_class,"Apply",{"system.boolean","system.boolean"}));
    a.create_sprite.bind(hud_icon_method(sprite,"Create",{
        "unityengine.texture2d","unityengine.rect","unityengine.vector2"}));
    a.set_icon.bind(hud_icon_method(widget,"SetIcon",{"unityengine.sprite"}));
    a.get_sprite.bind(find_method(icon,"get_sprite",0));
    a.widget_icon=ping_field_of(widget,"icon");
    a.ready=a.texture_ctor&&a.load_raw&&a.apply&&a.create_sprite&&a.set_icon&&a.get_sprite&&a.widget_icon;
    if(a.ready)LOG_INFO("peer HUD: player icon API ready; exact constructor and Sprite.Create signatures");
    return a.ready;
}

bool hud_icon_create() {
    auto& a=g_peer_hud_icon;
    if(hud_live(a.texture.get())&&hud_live(a.sprite.get()))return true;
    constexpr unsigned bytes=4*kPlayerMarkerIconSize*kPlayerMarkerIconSize;
    ManagedRoot pixels;
    if(!hud_retain(pixels,il2cpp_array_new(a.byte_class,bytes))||
       g_array_get_length.call(pixels.get(),g_array_get_length.method)!=bytes)return false;
    render_player_marker_icon(reinterpret_cast<std::uint8_t*>(pixels.get())+kArrayDataOffset);
    struct Candidate {
        ManagedRoot texture,sprite;
        bool committed=false;
        ~Candidate() {
            if(committed)return;
            if(hud_live(sprite.get()))g_object_destroy.call(sprite.get(),g_object_destroy.method);
            if(hud_live(texture.get()))g_object_destroy.call(texture.get(),g_object_destroy.method);
        }
    } candidate;
    if(!hud_retain(candidate.texture,il2cpp_object_new(a.texture_class)))return false;
    a.texture_ctor.call(candidate.texture.get(),kPlayerMarkerIconSize,kPlayerMarkerIconSize,4,false,false,a.texture_ctor.method);
    a.load_raw.call(candidate.texture.get(),pixels.get(),a.load_raw.method);
    a.apply.call(candidate.texture.get(),false,true,a.apply.method);
    void* sprite=a.create_sprite.call(candidate.texture.get(),HudIconRect{0,0,64,64},HudIconPivot{0.5f,0.5f},a.create_sprite.method);
    if(!sprite||!hud_retain(candidate.sprite,sprite)||!hud_retain(a.sprite,sprite)||
       !hud_retain(a.texture,candidate.texture.get()))return false;
    candidate.committed=true;
    LOG_INFO("peer HUD: original player silhouette sprite ready size=64x64");
    return true;
}

bool hud_icon_matches_widget(void* widget,void* wanted_label) {
    if(!wanted_label||!hud_live(widget))return false;
    void* label=nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(widget),g_peer_hud_api.widget_label,&label);
    return label==wanted_label;
}

void hud_icon_tick_inner() {
    auto& a=g_peer_hud_icon;auto& m=g_peer_hud;auto& api=g_peer_hud_api;
    if(!m.registered||!api.widget_type||!api.widget_label||!hud_icon_resolve()||!hud_icon_create())return;
    void* label=nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(m.ping.get()),api.label,&label);
    if(!hud_icon_matches_widget(a.widget.get(),label)) {
        // A native pooled widget can be recycled for a different ping. Never
        // change its icon until the unique managed label identity matches ours.
        a.widget.clear();
        ManagedRoot list;
        if(!hud_retain(list,g_find_objects_all.call(api.widget_type,true,g_find_objects_all.method)))return;
        const int n=g_array_get_length.call(list.get(),g_array_get_length.method);
        if(n<0||n>256)return;
        for(int i=0;i<n;++i) {
            void* widget=g_array_get_value.call(list.get(),i,g_array_get_value.method);
            if(hud_icon_matches_widget(widget,label)){hud_retain(a.widget,widget);break;}
        }
    }
    void* widget=a.widget.get();
    if(!widget)return;
    void* icon=nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(widget),a.widget_icon,&icon);
    if(!hud_live(icon))return;
    if(a.get_sprite.call(icon,a.get_sprite.method)==a.sprite.get())return;
    a.set_icon.call(widget,a.sprite.get(),a.set_icon.method);
    const bool verified=a.get_sprite.call(icon,a.get_sprite.method)==a.sprite.get();
    LOG_INFO("peer HUD: icon=player peer=%08x verified=%u own_widget=1",m.peer,unsigned(verified));
}

void hud_icon_tick() {
    auto& a=g_peer_hud_icon;
    const double now=build_now();
    if(now<a.next_tick)return;
    a.next_tick=now+0.5;
    try {hud_icon_tick_inner();}
    catch(...) {
        a.widget.clear();
        if(now>=a.next_error) {
            LOG_WARN("peer HUD: player icon game exception; native marker remains active, retry deferred");
            a.next_error=now+5;
        }
        a.next_tick=now+5;
    }
}
