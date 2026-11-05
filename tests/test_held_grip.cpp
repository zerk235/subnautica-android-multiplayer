#include <cassert>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#define LOG_WARN(...) ((void)0)
template<class Fn> struct Entry {
    Fn call=nullptr; void* method=nullptr;
    explicit operator bool() const { return call!=nullptr; }
};
struct Transform { Transform* parent=nullptr; };
unsigned attaches=0,repairs=0;
bool fail=false,wrong_parent=false;
void* parent(void* tf,void*) { return static_cast<Transform*>(tf)->parent; }
Entry<decltype(&parent)> g_transform_get_parent{&parent};
void native_attach(void* model,void* plug,void* socket,void*) {
    ++attaches; assert(plug);
    static Transform temporary;
    static_cast<Transform*>(model)->parent=&temporary;
    if (fail) throw std::runtime_error("native call failed after temporary parent");
    if (!wrong_parent) static_cast<Transform*>(model)->parent=static_cast<Transform*>(socket);
}
Entry<decltype(&native_attach)> g_plug_visual{&native_attach};
void set_parent(void* model,void* socket,bool keep_world,void*) {
    assert(keep_world); ++repairs;
    static_cast<Transform*>(model)->parent=static_cast<Transform*>(socket);
}
Entry<decltype(&set_parent)> g_set_parent{&set_parent};
#include "modloader/src/held_grip.inc"
int main() {
    Transform model,mesh{&model},grip{&mesh},hand,local_player_grip;
    Transform wrist{&model};
    Transform offset_socket{&wrist};
    assert(visual_hand_anchor(&model,&wrist,&offset_socket)==&wrist);
    assert(visual_hand_anchor(&model,&local_player_grip,&offset_socket)==&offset_socket);
    assert(visual_hand_anchor(&model,nullptr,&local_player_grip)==&model);
    assert(select_visual_grip(&model,&grip,&wrist,1)==&wrist);
    assert(select_visual_grip(&model,&grip,&wrist,0)==&grip);
    assert(select_visual_grip(&model,&grip,&wrist,0,751)==&wrist);
    assert(select_visual_grip(&model,&grip,&local_player_grip,0,751)==&grip);
    assert(select_visual_grip(&model,&grip,nullptr,0,751)==&grip);
    assert(select_visual_grip(&model,nullptr,&wrist,1)==&wrist);
    assert(select_visual_grip(&model,nullptr,nullptr,0)==&model);
    assert(select_visual_grip(&model,&local_player_grip,nullptr,0)==nullptr);
    assert(select_visual_grip(&model,&grip,&local_player_grip,1)==&grip);
    assert(prop_owns_transform(&model,&grip));
    assert(prop_owns_transform(&model,&model));
    assert(!prop_owns_transform(&model,&local_player_grip));
    assert(!plug_visual_to_hand(&model,&local_player_grip,&hand) && attaches==0);
    assert(!plug_visual_to_hand(&model,nullptr,&hand) && attaches==0);
    assert(!plug_visual_to_hand(&model,&grip,nullptr) && attaches==0);
    assert(plug_visual_to_hand(&model,&grip,&hand));
    assert(model.parent==&hand && attaches==1 && repairs==0);
    fail=true;
    assert(!plug_visual_to_hand(&model,&grip,&hand));
    assert(model.parent==&hand && attaches==2 && repairs==1);
    fail=false; wrong_parent=true;
    assert(!plug_visual_to_hand(&model,&grip,&hand));
    assert(model.parent==&hand && attaches==3 && repairs==2);
    Transform cycle; cycle.parent=&cycle;
    assert(!prop_owns_transform(&model,&cycle));
    g_plug_visual.call=nullptr;
    assert(!plug_visual_to_hand(&model,&grip,&hand) && attaches==3);
    puts("actual held grip: owned transform, foreign/null/cyclic references and native failure repair passed");
}
