#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#define LOG_WARN(...) ((void)0)
template<class Fn> struct Entry {
    Fn call=nullptr; void* method=nullptr;
    explicit operator bool() const { return call!=nullptr; }
};
struct Node { const char* name; Node* parent=nullptr; std::vector<Node*> children,bones; bool active=false; };
void* parent(void* x,void*) { return static_cast<Node*>(x)->parent; }
Entry<decltype(&parent)> g_transform_get_parent{&parent};
void native_attach(void*,void*,void*,void*) {}
Entry<decltype(&native_attach)> g_plug_visual{&native_attach};
void set_parent(void*,void*,bool,void*) {}
Entry<decltype(&set_parent)> g_set_parent{&set_parent};
#include "modloader/src/held_grip.inc"
void* identity(void* x,void*) { return x; }
Entry<decltype(&identity)> g_get_go_transform{&identity},g_get_game_object{&identity};
void* get_component(void* x,void*,void*) { auto* n=static_cast<Node*>(x); return n->bones.empty() ? nullptr : n; }
Entry<decltype(&get_component)> g_get_component{&get_component};
void* bones(void* x,void*) { return &static_cast<Node*>(x)->bones; }
Entry<decltype(&bones)> g_smr_get_bones{&bones};
int32_t length(void* x,void*) { return static_cast<std::vector<Node*>*>(x)->size(); }
Entry<decltype(&length)> g_array_get_length{&length};
void* element(void* x,int32_t i,void*) { return static_cast<std::vector<Node*>*>(x)->at(i); }
Entry<decltype(&element)> g_array_get_value{&element};
int32_t count(void* x,void*) { return static_cast<Node*>(x)->children.size(); }
Entry<decltype(&count)> g_get_child_count{&count};
void* child(void* x,int32_t i,void*) { return static_cast<Node*>(x)->children.at(i); }
Entry<decltype(&child)> g_get_child{&child};
void* name(void* x,void*) { return const_cast<char*>(static_cast<Node*>(x)->name); }
Entry<decltype(&name)> g_get_name{&name};
void* g_smr_type_obj=reinterpret_cast<void*>(1);
bool read_managed_name(void* x,char* out,size_t n) { if (!x) return false; std::snprintf(out,n,"%s",static_cast<char*>(x)); return true; }
bool g_get_component_is_string=false;
#include "modloader/src/held_mesh_hand.inc"
void active(void* x,bool value,void*) { static_cast<Node*>(x)->active=value; }
Entry<decltype(&active)> g_set_active{&active};
#include "modloader/src/held_equipped_model.inc"
int main() {
    assert(visual_right_hand_name("hand.R"));
    assert(visual_right_hand_name("Bip01 R Hand"));
    assert(!visual_right_hand_name("hand.L"));
    assert(!visual_right_hand_name("otherhand"));
    assert(!visual_right_hand_name("rightHandIndex1"));
    Node root{"body"},target{"rightHand",&root},wrist{"hand.R",&root},left{"hand.L",&root},foreign{"RightHand"};
    Node mesh{"mesh",&root}; root.children={&mesh,&target,&wrist,&left};
    mesh.bones={&wrist,&left,&foreign};
    assert(find_visual_right_hand(&root,&root,&target)==&wrist);
    mesh.bones.push_back(&wrist); // repeated across slots/renderers is the same wrist
    assert(find_visual_right_hand(&root,&root,&target)==&wrist);
    Node other{"RightHand",&root}; mesh.bones.push_back(&other);
    assert(find_visual_right_hand(&root,&root,&target)==nullptr);
    assert(find_visual_right_hand(&root,&root,&other)==&other);
    mesh.bones={&foreign};
    assert(find_visual_right_hand(&root,&root,&foreign)==nullptr);
    mesh.bones={&left};
    assert(find_visual_right_hand(&root,&root,nullptr)==nullptr);
    Node world{"world",&root},view{"view",&root};
    world.active=true;
    assert(set_equipped_visual(&root,&world,&view));
    assert(!world.active && view.active);
    world.active=true; view.active=false;
    assert(!set_equipped_visual(&root,&foreign,&view));
    assert(world.active && !view.active && !foreign.active);
    assert(!set_equipped_visual(&root,&world,&world));
    assert(!set_equipped_visual(&root,&root,&view));
    view.parent=&world;
    assert(!set_equipped_visual(&root,&world,&view));
    assert(world.active && !view.active);
    puts("equipped visual: owned sibling models switched; foreign, root, same and ancestor references rejected");
    puts("actual mesh hand: deforming wrist selected, controller target/foreign/finger/ambiguous bones rejected");
}
