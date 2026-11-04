#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "modloader/src/held_rotation.h"
struct Quaternion { float x,y,z,w; };
#define LOG_WARN(...) ((void)0)
struct Vector3 { float x=0,y=0,z=0; };
template<class Fn> struct Entry { Fn call=nullptr; void* method=nullptr; explicit operator bool() const { return call!=nullptr; } };
struct Node { const char* name; Node* parent=nullptr; std::vector<Node*> children,bones; Vector3 local; int orientation=0; Vector3 scale{1,1,1}; };
Vector3 position(void* x,void*) {
    auto* n=static_cast<Node*>(x); Vector3 p=n->local;
    for (unsigned i=0;n->parent && i<64;++i) { n=n->parent; p.x+=n->local.x; p.y+=n->local.y; p.z+=n->local.z; }
    return p;
}
Entry<decltype(&position)> g_get_position{&position};
void set_position(void* x,Vector3 p,void*) {
    auto* n=static_cast<Node*>(x);
    if (n->parent) { auto q=position(n->parent,nullptr); p.x-=q.x; p.y-=q.y; p.z-=q.z; }
    n->local=p;
}
Entry<decltype(&set_position)> g_set_position{&set_position};
void* parent(void* x,void*) { return static_cast<Node*>(x)->parent; }
Entry<decltype(&parent)> g_transform_get_parent{&parent};
void set_parent(void* x,void* target,bool keep_world,void*) {
    auto p=position(x,nullptr); static_cast<Node*>(x)->parent=static_cast<Node*>(target);
    if (keep_world) set_position(x,p,nullptr);
}
Entry<decltype(&set_parent)> g_set_parent{&set_parent};
void* last_native_target=nullptr;
void native_attach(void* x,void* plug,void* target,void*) {
    last_native_target=target;
    auto a=position(x,nullptr),b=position(plug,nullptr),p=position(target,nullptr);
    static_cast<Node*>(x)->parent=static_cast<Node*>(target);
    set_position(x,Vector3{p.x+a.x-b.x,p.y+a.y-b.y,p.z+a.z-b.z},nullptr);
    static_cast<Node*>(x)->orientation=static_cast<Node*>(target)->orientation;
}
Entry<decltype(&native_attach)> g_plug_visual{&native_attach};
#include "modloader/src/held_grip.inc"
void* identity(void* x,void*) { return x; }
Entry<decltype(&identity)> g_get_go_transform{&identity},g_get_game_object{&identity};
void* component(void* x,void*,void*) { auto* n=static_cast<Node*>(x); return n->bones.empty() ? nullptr : n; }
Entry<decltype(&component)> g_get_component{&component};
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
void* g_smr_type_obj=reinterpret_cast<void*>(1); bool g_get_component_is_string=false;
Vector3 transform_point(void* x,Vector3 offset,void*) {
    auto* n=static_cast<Node*>(x); auto p=position(x,nullptr);
    offset={offset.x*n->scale.x,offset.y*n->scale.y,offset.z*n->scale.z};
    if (n->orientation==72) offset=Vector3{-offset.y,offset.x,offset.z};
    return Vector3{p.x+offset.x,p.y+offset.y,p.z+offset.z};
}
Entry<decltype(&transform_point)> g_held_transform_point{&transform_point};
#include "modloader/src/held_palm.inc"
unsigned rotation_writes=0;
void set_rotation(void* x,Quaternion q,void*) {
    ++rotation_writes;
    static_cast<Node*>(x)->orientation=std::fabs(q.z)>0.7f ? 72 : 0;
}
Entry<decltype(&set_rotation)> g_held_set_local_rotation{&set_rotation};
#include "modloader/src/held_rotation_apply.inc"
bool near(Vector3 a,Vector3 b) { return std::fabs(a.x-b.x)<0.0001f && std::fabs(a.y-b.y)<0.0001f && std::fabs(a.z-b.z)<0.0001f; }
int main() {
    Node root{"root"},wrist{"wrist",&root},mesh{"mesh",&root},clothing{"clothing",&root};
    root.local={10,20,30}; wrist.local={0,1,0};
    Node a{"fingerA",&wrist},b{"fingerB",&wrist},c{"fingerC",&wrist},socket{"socket",&wrist},foreign{"foreign"};
    a.local={0.08f,0.01f,-0.02f}; b.local={0.10f,0.01f,-0.02f}; c.local={0.12f,0.01f,-0.02f}; socket.local={0.49f,0,0}; socket.orientation=72;
    root.children={&wrist,&mesh,&clothing}; wrist.children={&a,&b,&c,&socket};
    mesh.bones={&a,&b,&c,&socket,&foreign}; clothing.bones=mesh.bones;
    Vector3 palm{}; unsigned samples=0;
    assert(visual_palm_center(&root,&wrist,&palm,&samples));
    assert(samples==3 && near(palm,Vector3{10.05f,21.005f,29.99f}));
    Node model{"model"},grip{"grip",&model}; grip.local={0.03f,-0.02f,0.01f};
    assert(plug_visual_to_palm(&model,&grip,&wrist,&socket,palm));
    assert(model.parent==&wrist && last_native_target==&socket && model.orientation==72);
    assert(near(position(&grip,nullptr),palm));
    wrist.local={5,2,1};
    assert(near(position(&grip,nullptr),Vector3{15.05f,22.005f,30.99f}));
    assert(plug_visual_to_palm(&model,&grip,&wrist,&foreign,Vector3{15,22,31}));
    assert(last_native_target==&wrist && near(position(&grip,nullptr),Vector3{15,22,31}));
    // A virtual handle point must land at the palm after rotation and scale.
    model.scale={2,3,4};
    Vector3 handle{0.10f,0.02f,-0.01f};
    Vector3 target{15.0f,22.0f,31.0f};
    assert(plug_visual_to_palm(&model,&model,&wrist,&socket,target,handle));
    assert(near(transform_point(&model,handle,nullptr),target));
    assert(model.parent==&wrist && model.orientation==72);
    assert(!near(position(&model,nullptr),target));
    auto before=position(&model,nullptr);
    g_held_transform_point.call=nullptr;
    assert(!plug_visual_to_palm(&model,&model,&wrist,&socket,target,handle));
    assert(near(position(&model,nullptr),before));
    assert(plug_visual_to_palm(&model,&grip,&wrist,&socket,target));
    assert(near(position(&grip,nullptr),target));
    g_held_transform_point.call=&transform_point;
    Vector3 palm_local{0.05f,0.005f,-0.01f};
    auto expected=transform_point(&wrist,palm_local,nullptr);
    assert(apply_held_rotation_pose(&model,&model,&wrist,palm_local,handle,{0,0,0.70710678f,0.70710678f}));
    assert(model.orientation==72 && near(transform_point(&model,handle,nullptr),expected));
    assert(apply_held_rotation_pose(&model,&model,&wrist,palm_local,handle,{0,0,0,1}));
    assert(model.orientation==0 && near(transform_point(&model,handle,nullptr),expected));
    wrist.local.x+=2;
    assert(near(transform_point(&model,handle,nullptr),transform_point(&wrist,palm_local,nullptr)));
    auto writes=rotation_writes;
    assert(!apply_held_rotation_pose(&model,&foreign,&wrist,palm_local,handle,{0,0,0,1}));
    assert(!apply_held_rotation_pose(&model,&model,&root,palm_local,handle,{0,0,0,1}));
    assert(!apply_held_rotation_pose(&model,&model,&wrist,palm_local,handle,{0,0,0,0}));
    assert(rotation_writes==writes && foreign.orientation==0 && wrist.orientation==0);
    clothing.bones.clear(); mesh.bones={&a,&b,&foreign,&socket};
    assert(!visual_palm_center(&root,&wrist,&palm,&samples) && samples==0);
    puts("actual rotation apply: handle remains at palm, follows wrist; invalid/foreign/parent rejected; actual palm: proximal centroid/dedup/outlier/foreign checks; authored orientation and grip position preserved while following wrist");
}
