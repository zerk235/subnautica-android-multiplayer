#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
template<class Fn> struct Entry { Fn call=nullptr; void* method=nullptr; explicit operator bool() const { return call!=nullptr; } };
struct Node;
struct Renderer { int kind=1; Node* owner; int shadow; };
struct Node { int kind=0; const char* name; bool active=true; Renderer* renderer=nullptr; std::vector<Node*> children; };
void* g_local_game_object=nullptr;
void* g_renderer_type_obj=reinterpret_cast<void*>(1);
void* g_renderer_name_str=reinterpret_cast<void*>(2);
bool g_get_component_is_string=false;
unsigned writes=0;
void* component(void* n,void* type,void*) { assert(type==(g_get_component_is_string ? g_renderer_name_str : g_renderer_type_obj)); return static_cast<Node*>(n)->renderer; }
Entry<decltype(&component)> g_get_component{&component};
void* object(void* n,void*) { return *static_cast<int*>(n)==1 ? static_cast<Renderer*>(n)->owner : n; }
Entry<decltype(&object)> g_get_game_object{&object},g_get_go_transform{&object};
void* name(void* n,void*) { return const_cast<char*>(static_cast<Node*>(n)->name); }
Entry<decltype(&name)> g_get_name{&name};
bool read_managed_name(void* value,char* out,size_t size) { if (!value || strlen(static_cast<char*>(value))>=size) return false; strcpy(out,static_cast<char*>(value)); return true; }
int32_t count(void* n,void*) { return static_cast<Node*>(n)->children.size(); }
Entry<decltype(&count)> g_get_child_count{&count};
void* child(void* n,int32_t i,void*) { return static_cast<Node*>(n)->children.at(i); }
Entry<decltype(&child)> g_get_child{&child};
int32_t shadow(void* r,void*) { return static_cast<Renderer*>(r)->shadow; }
Entry<decltype(&shadow)> g_head_get_shadow{&shadow};
void set_shadow(void* r,int32_t mode,void*) { ++writes; static_cast<Renderer*>(r)->shadow=mode; }
Entry<decltype(&set_shadow)> g_head_set_shadow{&set_shadow};
bool active(void* n,void*) { return static_cast<Node*>(n)->active; }
Entry<decltype(&active)> g_go_get_active_hierarchy{&active};
#include "modloader/src/avatar_head_visibility.inc"
int main() {
    Node local{0,"Player"},source{0,"body"},copy{0,"body(Clone)"}; g_local_game_object=&local;
    Node head{0,"diveSuit_head_geo"},radiation{0,"radiationSuit_head_geo",false},scuba{0,"scuba_head",false},body{0,"diveSuit_body_geo"},bone{0,"head_rig"};
    Renderer h{1,&head,3},r{1,&radiation,3},s{1,&scuba,1},b{1,&body,3},original{1,&source,3};
    head.renderer=&h; radiation.renderer=&r; scuba.renderer=&s; body.renderer=&b; source.renderer=&original;
    copy.children={&head,&radiation,&scuba,&body,&bone};
    assert(restore_avatar_head_visibility(&copy,&source,42));
    assert(h.shadow==1 && r.shadow==1 && s.shadow==1 && b.shadow==3 && original.shadow==3);
    assert(head.active && !radiation.active && !scuba.active && writes==2);
    assert(restore_avatar_head_visibility(&copy,&source,42) && writes==2);
    head.active=false; radiation.active=true; h.shadow=0; r.shadow=2;
    g_get_component_is_string=true;
    assert(restore_avatar_head_visibility(&copy,&source,42));
    assert(!head.active && radiation.active && h.shadow==0 && r.shadow==2 && writes==2);
    assert(!restore_avatar_head_visibility(&source,&source,42));
    assert(!restore_avatar_head_visibility(&local,&source,42));
    head.renderer=&original;
    assert(!restore_avatar_head_visibility(&copy,&source,42) && original.shadow==3 && writes==2);
    head.renderer=&h; h.shadow=3;
    g_head_set_shadow.call=nullptr;
    assert(!restore_avatar_head_visibility(&copy,&source,42) && h.shadow==3 && writes==2);
    g_head_set_shadow.call=&set_shadow;
    Node empty{0,"empty"}; assert(!restore_avatar_head_visibility(&empty,&source,42));
    assert(!avatar_head_mesh_name("head_rig") && !avatar_head_mesh_name("head_R") && !avatar_head_mesh_name("scuba_head_extra"));
    copy.children={&source}; assert(!restore_avatar_head_visibility(&copy,&source,42) && original.shadow==3);
    puts("actual avatar head helper: ShadowsOnly restored only on clone head renderers; equipment activity and source unchanged; idempotent; foreign renderer, unavailable API and original roots rejected");
}
