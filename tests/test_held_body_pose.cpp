#include <cassert>
#include <cstdio>
#include <limits>
#include <cstdint>
#include "modloader/src/held_pose.h"
#include "modloader/src/held_rotation.h"
#define LOG_INFO(...) ((void)0)
struct Vector3 {float x,y,z;};struct Quaternion{float x,y,z,w;};
template<class F>struct Entry{F call;void* method=nullptr;explicit operator bool()const{return call!=nullptr;}};
struct Node{Node* parent=nullptr;Vector3 p{};Quaternion q{};bool active=false;};
Node body,prop,left{&prop},right{&prop},foreign;
int arms,type;
void* g_arms_type_obj=&type;void* g_arms_name_str=nullptr;
unsigned g_avatar_frame=1,binds=0,clears=0;
constexpr uint32_t kFlagHeldWorldPose=1u<<10;
struct RemoteAvatar{void* transform=&body,*game_object=&body,*held_prop=&prop,*held_arms=nullptr,*held_left_ik=nullptr,*held_right_ik=nullptr;bool held_world_ik_bound=false;uint32_t id=1,held_tech=751;};
struct PeerState{float x=100,y=20,z=200;uint32_t id=1,held_tech=751,flags=kFlagHeldWorldPose;bool held_rotation_valid=true;HeldRotation held_rotation{0,0,0,1};HeldOffset held_offset{0.3f,0.5f,1};};
bool prop_owns_transform(void* root,void* p){for(auto* n=(Node*)p;n;n=n->parent)if(n==root)return true;return false;}
void* find_component_in_hierarchy(void*,void*,void*,int){return &arms;}
void set_parent(void* node,void* parent,bool keep,void*){assert(!keep);((Node*)node)->parent=(Node*)parent;}
void set_pos(void* node,Vector3 p,void*){((Node*)node)->p=p;}
void set_rot(void* node,Quaternion q,void*){((Node*)node)->q=q;}
void set_active(void* node,bool on,void*){((Node*)node)->active=on;}
void* transform(void* node,void*){return node;}
void targets(void* self,void* l,void* r,void*){assert(self==&arms);if(l||r){assert(l==&left && r==&right);++binds;}else ++clears;}
Entry<decltype(&set_parent)>g_set_parent{set_parent};
Entry<decltype(&set_pos)>g_set_position{set_pos};
Entry<decltype(&set_rot)>g_set_rotation{set_rot};
Entry<decltype(&set_active)>g_set_active{set_active};
Entry<decltype(&transform)>g_get_go_transform{transform};
Entry<decltype(&targets)>g_arms_world_targets{targets};
#include "modloader/src/held_body_pose.inc"
int main(){
 assert(held_offset_valid({3,0,0}));assert(!held_offset_valid({3,0.1f,0}));
 assert(!held_offset_valid({std::numeric_limits<float>::quiet_NaN(),0,0}));
 assert(!held_offset_valid({0,0,std::numeric_limits<float>::infinity()}));
 RemoteAvatar avatar;PeerState peer;
 assert(!prepare_held_body_pose(&avatar,&prop,&foreign,&right,751));
 assert(!prepare_held_body_pose(&avatar,&prop,&left,&right,509));
 assert(prepare_held_body_pose(&avatar,&prop,&left,&right,751));
 assert(prop.parent==&body); // not attached beneath either solved wrist
 apply_held_body_pose(&avatar,peer);
 assert(prop.active && binds==1 && avatar.held_world_ik_bound);
 assert(std::fabs(prop.p.x-100.3f)<0.001f && prop.p.y==20.5f && prop.p.z==201);
 peer.x+=10;apply_held_body_pose(&avatar,peer);assert(prop.p.x>110 && binds==1);
 peer.flags=0;apply_held_body_pose(&avatar,peer);assert(!prop.active && clears==1 && !avatar.held_world_ik_bound);
 peer.flags=kFlagHeldWorldPose;apply_held_body_pose(&avatar,peer);assert(prop.active && binds==2);
 peer.held_offset.x=1000;apply_held_body_pose(&avatar,peer);assert(!prop.active && clears==2);
 peer.held_offset={0,0,1};peer.held_rotation={0,0,0,0};apply_held_body_pose(&avatar,peer);assert(!prop.active && binds==2);
 peer.held_rotation={0,0,0,2};apply_held_body_pose(&avatar,peer);assert(prop.active && prop.q.w==1 && binds==3);
 peer.held_tech=0;apply_held_body_pose(&avatar,peer);assert(!prop.active && clears==3);
 clear_held_body_ik(&avatar);assert(clears==3);
 puts("actual two-hand pose: authored clone targets, body parent without wrist feedback, exact owner frame, one IK binding, invalid/stale/holstered pose hidden and IK cleared passed");
}
