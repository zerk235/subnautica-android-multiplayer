#include "managed_hook.h"

#include <stdio.h>
#include <string.h>

#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <math.h>
#include <initializer_list>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "hooks.h"
#include "il2cpp_api.h"
#include "log.h"
#include "launch_role_bridge.h"
#include "net.h"
#include "interior_build.h"
#include "locker_contents.h"
#include "managed_root.h"
#include "runtime_load_guard.h"
#include "avatar_update_frame.h"
#include <deque>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include "held_light.h"
#include "base_geometry_envelope.h"
#include "base_geometry_cache.h"
#include "base_geometry_corridor.h"
#include "base_build_completion.h"

#include "world.h"
#include "world_context.h"

// Role state, deliberately outside the anonymous namespace below. net.cpp's pump thread signals
// session-role changes here; Unity-facing effects are consumed by Player.Update.
bool g_local_is_host = false;
bool g_role_known = false;
void net_note_role_from_peer(uint32_t peer_id);
void net_note_session_role(bool is_host, uint32_t host_id);
bool loading_screen_blocks_world();

namespace {

// Offsets taken from the Il2CppDumper header generated for this exact build
// (Unity 6000.0.44f1, metadata version shipped with 1.22.83315).
constexpr size_t kMethodPointerOffset = 0x00;      // MethodInfo::methodPointer
constexpr size_t kVirtualPointerOffset = 0x08;      // MethodInfo::virtualMethodPointer
constexpr size_t kMethodInvokerOffset = 0x10;       // MethodInfo::invoker_method
constexpr size_t kMethodSlotOffset = 0x50;          // MethodInfo::slot (uint16)
constexpr size_t kClassVtableOffset = 0x138;        // Il2CppClass::vtable[255]
constexpr size_t kVirtualInvokeSize = 16;           // VirtualInvokeData {methodPtr, method}
constexpr size_t kClassVtableCountOffset = 0x126;   // Il2CppClass_2::vtable_count (uint16)

using ManagedFn = void (*)(void* self, void* method);

bool write_pointer(void* addr, void* value) {
    return mem_patch_write(addr, &value, sizeof(void*));
}

// Counter for the smoke-test hook; written from the game thread.
volatile unsigned long g_update_calls = 0;
ManagedFn g_player_update_original = nullptr;

// A Vector3 is a homogeneous float aggregate of three members, so AAPCS64 returns it in
// s0-s2. That makes it safe to call the generated getter directly instead of boxing a
// copy through il2cpp_runtime_invoke on every frame.
struct Vector3 {
    float x;
    float y;
    float z;
};

using GetTransformFn = void* (*)(void* self, void* method);
using GetPositionFn = Vector3 (*)(void* self, void* method);
using GetEulerFn = Vector3 (*)(void* self, void* method);
using PredicateFn = bool (*)(void* self, void* method);
using AddComponentFn = void* (*)(void* self, void* type, void* method);

// Every IL2CPP entry point is called with its own MethodInfo as the trailing argument.
// Passing nullptr works for plain managed methods but breaks the Unity internal-call
// bridge, which dereferences it.
template <typename Fn>
struct Entry {
    Fn call = nullptr;
    struct Il2CppMethodInfo* method = nullptr;

    void bind(struct Il2CppMethodInfo* info) {
        method = info;
        call = info ? reinterpret_cast<Fn>(
                          *reinterpret_cast<void**>(reinterpret_cast<char*>(info)))
                    : nullptr;
    }
    explicit operator bool() const { return call != nullptr; }
};

Entry<GetTransformFn> g_get_transform;
Entry<GetPositionFn> g_get_position;
Entry<GetEulerFn> g_get_euler;
Entry<PredicateFn> g_is_swimming;
Entry<PredicateFn> g_is_inside;
Entry<PredicateFn> g_is_piloting;
Entry<PredicateFn> g_is_underwater_for_swimming;
// GameObject.AddComponent(Type), used to give a clone its own SkyApplier. Called through its
// MethodInfo like every other method here: il2cpp_runtime_invoke takes the game down in this
// build, so there is no boxing path available.
Entry<AddComponentFn> g_add_component;
// SkyApplier.RefreshDirtySky(), called after the renderers array is filled so the applier redoes
// its work instead of waiting for the clone to move further than updateDistance from where it was
// first applied.
Entry<ManagedFn> g_sky_refresh_dirty;
// SkyEnvironmentChanged.Broadcast(GameObject, GameObject), static. Tells the copy's appliers
// which environment sky applies (Nitrox does this on every attach). Without it a dynamically
// added applier keeps a null environmentSky and the copy renders as a black silhouette.
using BroadcastEnvFn = void (*)(void* target, void* environment, void* method);
Entry<BroadcastEnvFn> g_sky_env_broadcast;
using AvatarEnvironmentFn=void* (*)(void*,void*);
Entry<AvatarEnvironmentFn> g_avatar_get_environment;
Entry<BroadcastEnvFn> g_avatar_apply_environment; // SkyApplier.GetAndApplySkybox(GameObject)
void* g_avatar_subroot_type=nullptr;

struct Il2CppMethodInfo* find_method(struct Il2CppClass* klass, const char* name, int argc) {
    if (!klass || !il2cpp_class_get_method_from_name) return nullptr;
    return il2cpp_class_get_method_from_name(klass, name, argc);
}

// Cloning the whole player hangs the main thread, so only a child object is cloned. The
// model child carries the diver mesh without any of the Player gameplay scripts.
void* g_avatar_source = nullptr;

// Player.camAnchor, the pivot the camera look rotates around. Cached after the first read.
void* g_local_cam_anchor = nullptr;

void resolve_avatar_source();
void read_pose_angles(void* player, LocalState* out);


bool read_player_transform(void* self, LocalState* out) {
    if (!self || !g_get_transform || !g_get_position) return false;

    void* transform = g_get_transform.call(self, g_get_transform.method);
    if (!transform) return false;

    Vector3 position = g_get_position.call(transform, g_get_position.method);
    out->x = position.x;
    out->y = position.y;
    out->z = position.z;

    // Subnautica never rotates the root Player transform - its euler stays at zero while
    // the diver mesh lives in the rotated "body"/"model" child - so reading the root used to
    // send a constant yaw of 0 and every remote avatar kept facing world +Z ("looking at the
    // back"). The angles therefore come from those two objects instead; see
    // read_pose_angles, which needs the GameObject/Transform entries declared further down.
    read_pose_angles(self, out);
    out->valid = true;
    return true;
}

uint32_t read_player_flags(void* self) {
    if (!self) return 0;

    bool swimming = g_is_swimming && g_is_swimming.call(self, g_is_swimming.method);
    bool inside = g_is_inside && g_is_inside.call(self, g_is_inside.method);
    bool piloting = g_is_piloting && g_is_piloting.call(self, g_is_piloting.method);
    // This is the exact predicate the game feeds into its own "is_underwater" Animator
    // trigger (Player.UpdateIsUnderwaterForSwimming computes it from the water level), so
    // sending it reproduces the real swim state instead of guessing from IsSwimming.
    bool underwater = g_is_underwater_for_swimming &&
                      g_is_underwater_for_swimming.call(self, g_is_underwater_for_swimming.method);

    uint32_t flags = 0;
    if (swimming) {
        flags |= kFlagSwimming;
    } else if (!inside) {
        flags |= kFlagWalking;
    }
    if (inside) flags |= kFlagInside;
    if (piloting) flags |= kFlagPiloting;
    if (underwater) {
        flags |= kFlagUnderwater;
    } else if (!inside) {
        flags |= kFlagOnSurface;
    }
    return flags;
}

// The parameters worth watching for an external overwrite: the ones that decide where the head
// looks and which way the body is going, so a component that mirrors the local player cannot hide
// behind the quiet ones.
constexpr int kWatchedParamCount = 5;
const char* const kWatchedParamNames[kWatchedParamCount] = {
    "move_speed", "view_pitch", "view_turn", "chestAim_lean", "chestAim_twist"};
void* g_watch_params[kWatchedParamCount] = {nullptr};

// Remote avatars clone the local player object, because CreatePrimitive relies on
// built-in mesh assets that are stripped from the shipped build and crash libunity.
struct RemoteAvatar {
    uint32_t id;
    void* game_object;
    void* transform;
    bool used;
    unsigned long last_update;
    void* animator;
    Vector3 last_position;
    unsigned long last_position_frame;
    bool has_last_position;
    // Peer poses arrive at 20 Hz while this runs every frame, so a per-frame delta is zero
    // on two frames out of three and spikes on the third. The travelled distance is therefore
    // accumulated over a short window and divided by the elapsed time, which yields the real
    // speed in units per second instead of zeros with 3x spikes in between.
    Vector3 sample_position;
    unsigned long sample_frame;
    float dist_accum;
    float speed;
    Vector3 velocity;
    bool last_swim;
    bool last_underwater;
    bool last_surface;
    bool underwater_mirrored;
    bool underwater_mirror_init;
    bool cinematics_mirrored;
    bool cinematics_mirror_init;
    // What this mod wrote to the Animator on the previous frame, kept so the next frame can tell
    // whether those values survived. Anything that ran in between and changed them is a component
    // driving the clone from the local player.
    float written_floats[kWatchedParamCount];
    bool written_valid;
    bool overwrite_reported;
    // The SkyApplier added to this clone, and whether its post-Start state has been reported yet.
    void* sky_applier;
    bool sky_reported;
    uint8_t sky_environment_id[16];
    bool sky_environment_bound;
    unsigned long sky_environment_next;
    // The visual copy of the peer's held item, parented under this clone. held_tech is the
    // TechType the prop currently corresponds to, held_retry the frame after which a failed
    // source lookup may run again.
    void* held_prop;
    void* held_lights_parent;
    void* held_lights[8];
    bool held_light_enabled[8];
    unsigned held_light_count;
    bool held_body_pose;
    bool held_world_ik_bound;
    void *held_arms,*held_left_ik,*held_right_ik;
    void* held_plug_origin;
    bool held_native_grip;
    bool held_use_palm;
    Vector3 held_grip_offset;
    Vector3 held_palm_local;
    bool held_palm_local_valid;
    bool held_received_rotation;
    HeldRotation held_last_rotation;
    bool held_visual_reported;
    uint32_t held_tech;
    unsigned long held_retry;
    // Held v3 debounce: the incoming tech has to agree for a few frames before the prop is
    // rebuilt (GetHeld goes null for a frame while switching slots, and without hysteresis the
    // prop flickers). held_pending is the latest wire value, held_stable counts agreeing frames.
    uint32_t held_pending;
    unsigned long held_stable;
    // Held v3 negative cache: a TechType with no scene source skips the full Pickupable scan
    // until held_nosource_until. A different TechType still scans immediately.
    uint32_t held_nosource;
    unsigned long held_nosource_until;
    // Peer-driven equipment (item sync v3): the peer's real suit/fins/head TechTypes and whether
    // they have been applied yet. Replaces the local-mirror, which dressed every avatar in the
    // local player's suit. equip_pose_held is the held_tech the holding_* pose was built for.
    uint32_t equip_suit;
    uint32_t equip_fins;
    uint32_t equip_head;
    bool equip_applied;
    uint32_t equip_pose_held;
    // Cached hand anchor (rightHandAttach, else the body root). Resolved once at spawn so the
    // per-frame follow below never walks the hierarchy.
    void* held_hand;
    void* held_hand_bone;
    void* held_controller_socket;
    void* held_grip_socket;
    // Peer piloting a vehicle: the diver sits inside, so the clone hides. Memset-safe.
    bool piloting_hidden;
    // using_tool overwrite watch: what the drive wrote last frame, what the peer flag was, and
    // whether both are valid. A mismatch with a STABLE peer flag is an external overwrite; a
    // mismatch across a peer transition is just the user clicking and stays silent.
    bool written_using;
    bool peer_using_last;
    bool written_using_valid;
};

constexpr size_t kMaxAvatars = kMaxPeers;
// Keep an avatar alive for a while after its peer drops, so a brief network hiccup does not
// make the character vanish and respawn elsewhere.
constexpr unsigned long kAvatarGraceFrames = 600;
RemoteAvatar g_avatars[kMaxAvatars];
unsigned long g_avatar_frame = 0;
void* g_last_player_object = nullptr;
void* g_local_game_object = nullptr;
// The local player's own Animator, kept only as a reference for diagnostics: it is what the
// game drives correctly, so reading its parameters shows what a working diver looks like.
void* g_local_animator = nullptr;

// Cloning the player creates another Player, whose own Update re-enters this function.
// Without the guard it would instantiate clones forever and hang the main thread.
bool g_updating_avatars = false;

// The first Player seen is the real one; clones must not drive the outgoing state.
void* g_local_player = nullptr;

struct Quaternion {
    float x;
    float y;
    float z;
    float w;
};

using GetGameObjectFn = void* (*)(void* self, void* method);
using GetChildFn = void* (*)(void* self, int32_t index, void* method);
using GetChildCountFn = int32_t (*)(void* self, void* method);
using GetNameFn = void* (*)(void* self, void* method);
using InstantiateFn = void* (*)(void* original, Vector3 position, Quaternion rotation,
                                void* method);
using SetVectorFn = void (*)(void* self, Vector3 value, void* method);
using DestroyFn = void (*)(void* obj, void* method);
using GetComponentFn = void* (*)(void* self, void* type, void* method);
using GetAnimParamsFn = void* (*)(void* self, void* method);
using SetAnimBoolFn = void (*)(void* self, void* name, bool value, void* method);
using SetAnimFloatFn = void (*)(void* self, void* name, float value, void* method);
using SetAnimTriggerFn = void (*)(void* self, void* name, void* method);
using GetAnimBoolFn = bool (*)(void* self, void* name, void* method);
using GetAnimFloatFn = float (*)(void* self, void* name, void* method);
using GetAnimIntFn = int32_t (*)(void* self, void* name, void* method);
// Behaviour.enabled, Animator.applyRootMotion and GameObject.activeSelf are all plain bool
// properties, so one getter shape covers all three.
using GetPropBoolFn = bool (*)(void* self, void* method);
using SetPropBoolFn = void (*)(void* self, bool value, void* method);
using GetPropIntFn = int32_t (*)(void* self, void* method);
using GetAnimObjectFn = void* (*)(void* self, void* method);
using SetAnimObjectFn = void (*)(void* self, void* value, void* method);
using GetPropFloatFn = float (*)(void* self, void* method);
using GetLayerWeightFn = float (*)(void* self, int32_t layer, void* method);
// Animator's int-id overloads. These take a parameter index and no managed string at all,
// which is the only reason they are used: an Il2CppString created from native code is a
// garbage-collected object with no managed reference pointing at it, so the GC is free to
// collect it. Once it does, every SetFloat/SetTrigger through that pointer silently fails to
// match a parameter and GetFloat answers 0, which looks exactly like "the peer state is
// being ignored". Indices cannot be collected.
using SetAnimFloatIdFn = void (*)(void* self, int32_t id, float value, void* method);
using GetAnimFloatIdFn = float (*)(void* self, int32_t id, void* method);
using SetAnimTriggerIdFn = void (*)(void* self, int32_t id, void* method);
using SetAnimBoolIdFn = void (*)(void* self, int32_t id, bool value, void* method);
using GetLengthFn = int32_t (*)(void* self, void* method);
using GetValueFn = void* (*)(void* self, int32_t index, void* method);

Entry<GetGameObjectFn> g_get_game_object;
Entry<GetTransformFn> g_get_go_transform;
Entry<GetChildFn> g_get_child;
Entry<GetChildCountFn> g_get_child_count;

// Body yaw and look angles, kept out of read_player_transform because the GameObject and
// Transform entries they need are declared here.
void read_pose_angles(void* player, LocalState* out) {
    out->yaw = 0.0f;
    out->pitch = 0.0f;
    out->view_yaw = 0.0f;

    if (g_local_game_object) resolve_avatar_source();
    void* body = g_avatar_source && g_get_go_transform
                     ? g_get_go_transform.call(g_avatar_source, g_get_go_transform.method)
                     : nullptr;
    if (body && g_get_euler) {
        Vector3 body_euler = g_get_euler.call(body, g_get_euler.method);
        out->yaw = body_euler.y;
    }

    // Player.camAnchor sits at offset 0x60 and is the pivot the mouse look rotates around, so
    // it carries the direction the peer is actually facing, independent of the body.
    if (!g_local_cam_anchor && player) {
        g_local_cam_anchor = *reinterpret_cast<void**>(reinterpret_cast<char*>(player) + 0x60);
        LOG_INFO("local camAnchor = %p", g_local_cam_anchor);
    }
    if (g_local_cam_anchor && g_get_euler) {
        Vector3 view = g_get_euler.call(g_local_cam_anchor, g_get_euler.method);
        out->pitch = view.x;
        out->view_yaw = view.y;
    }
}
using SetActiveFn = void (*)(void* self, bool value, void* method);
Entry<GetNameFn> g_get_name;
Entry<InstantiateFn> g_instantiate;
Entry<SetVectorFn> g_set_position;
Entry<SetVectorFn> g_set_euler;
Entry<SetActiveFn> g_set_active;
Entry<DestroyFn> g_destroy;
Entry<GetComponentFn> g_get_component;
Entry<GetAnimParamsFn> g_get_anim_params;
Entry<SetAnimBoolFn> g_anim_set_bool;
Entry<SetAnimBoolIdFn> g_anim_set_bool_id;
Entry<SetAnimFloatFn> g_anim_set_float;
Entry<SetAnimFloatIdFn> g_anim_set_float_id;
Entry<GetAnimFloatIdFn> g_anim_get_float_id;
Entry<SetAnimTriggerFn> g_anim_set_trigger;
Entry<SetAnimTriggerIdFn> g_anim_set_trigger_id;
Entry<SetAnimTriggerFn> g_anim_reset_trigger;
Entry<SetAnimTriggerIdFn> g_anim_reset_trigger_id;
Entry<GetAnimBoolFn> g_anim_get_bool;
Entry<GetAnimFloatFn> g_anim_get_float;
Entry<GetAnimIntFn> g_anim_get_int;
Entry<GetPropBoolFn> g_behaviour_get_enabled;
Entry<SetPropBoolFn> g_behaviour_set_enabled;
void* g_player_type_obj = nullptr;
void* g_player_name_str = nullptr;
// Components that Object.Instantiate copies into the clone along with the body, and that keep
// writing the local player's state into the clone's Animator every frame.
void* g_arms_type_obj = nullptr;
void* g_arms_name_str = nullptr;
void* g_rules_type_obj = nullptr;
void* g_rules_name_str = nullptr;
// SkyApplier is what gives the player's body its lighting. It sits on the Player root, which is
// exactly the object that is NOT part of the cloned body, so a clone may arrive with no lighting
// of its own and turn into a silhouette indoors. Diagnosed, not yet fixed: see diagnose_clone_lighting.
void* g_sky_type_obj = nullptr;
void* g_sky_name_str = nullptr;
// Renderer is only needed to count what a SkyApplier would have to cover.
void* g_renderer_type_obj = nullptr;
void* g_renderer_name_str = nullptr;
// Renderer properties are not Behaviour properties: bind them on Renderer itself.
using SetHeadShadowFn=void (*)(void* self,int32_t mode,void* method);
Entry<GetPropIntFn> g_head_get_shadow;
Entry<SetHeadShadowFn> g_head_set_shadow;

// SkyApplier field offsets, from dump.cs.
constexpr size_t kSkyAnchorSkyOffset = 0x20;
constexpr size_t kSkyCustomPrefabOffset = 0x28;
constexpr size_t kSkyDynamicOffset = 0x30;
constexpr size_t kSkyEmissiveFromPowerOffset = 0x31;
constexpr size_t kSkyRenderersOffset = 0x38;
constexpr size_t kSkyEnvironmentSkyOffset = 0x40;
constexpr size_t kSkyApplySkyOffset = 0x58;
constexpr size_t kSkyUpdaterIndexOffset = 0x68;

// IL2CPP array layout on 64-bit: Il2CppObject {klass, monitor} is 0x10, then bounds at 0x10 and
// max_length at 0x18, so the first element sits at 0x20. max_length is logged after allocation as
// a check that this is really the layout the runtime used.
constexpr size_t kArrayDataOffset = 0x20;
constexpr size_t kArrayMaxLengthOffset = 0x18;

// A renderer's position in the hierarchy, relative to the body root, as the chain of GameObject
// names leading to it. Both sides of the mapping are matched by name, because Instantiate keeps
// the clone's hierarchy identical to the source's - the same assumption mirror_active_state already
// relies on.
constexpr size_t kSkyPathMaxDepth = 12;
constexpr size_t kSkyPathNameChars = 48;

struct RelativePath {
    size_t depth = 0;
    char names[kSkyPathMaxDepth][kSkyPathNameChars];
};

// ArmsController.smoothSpeedUnderWater and .smoothSpeedAboveWater, as listed in dump.cs. These
// are the smoothing factors its own Update uses to turn its velocity estimate into animation.
constexpr size_t kArmsSmoothUnderWaterOffset = 0x20;
constexpr size_t kArmsSmoothAboveWaterOffset = 0x24;
// Rest of the ArmsController layout that the remote-body Update path needs, from dump.cs.
constexpr size_t kArmsIkToggleTimeOffset = 0x60;
constexpr size_t kArmsLeftAimOffset = 0xE8;
constexpr size_t kArmsRightAimOffset = 0xF0;
constexpr size_t kArmsReconfigureWorldTargetOffset = 0x140;
// ArmsController.rightHandAttach, the transform the right hand is glued to. A held-item prop is
// parented here so it moves with the remote body's arm instead of floating at the chest.
constexpr size_t kArmsRightHandAttachOffset = 0x70;

using VoidFn = void (*)(void* self, void* method);
using ReconfigureFn = void (*)(void* self, void* tool, void* method);
using FloatArgFn = void (*)(void* self, float value, void* method);

// The ArmsController of each remote clone, so the Update hook can tell a clone from the local
// player by identity rather than by guessing from field values.
void* g_remote_arms[kMaxAvatars] = {nullptr};
ManagedFn g_arms_update_original = nullptr;
Entry<VoidFn> g_arms_update_hand_ik_weights;
Entry<ReconfigureFn> g_arms_reconfigure;
using WorldIKTargetsFn=void (*)(void*,void*,void*,void*);
Entry<WorldIKTargetsFn> g_arms_world_targets;
Entry<FloatArgFn> g_arm_aiming_update;
Entry<GetPropBoolFn> g_anim_get_root_motion;
Entry<GetPropBoolFn> g_go_get_active_self;
Entry<GetPropIntFn> g_anim_get_layer_count;
Entry<GetAnimObjectFn> g_anim_get_controller;
Entry<SetAnimObjectFn> g_anim_set_controller;
Entry<GetPropIntFn> g_anim_get_culling;
Entry<GetPropIntFn> g_anim_get_update_mode;
Entry<GetPropFloatFn> g_anim_get_speed;
Entry<GetAnimObjectFn> g_anim_get_avatar;
Entry<GetLayerWeightFn> g_anim_get_layer_weight;
struct Il2CppClass* g_smr_class = nullptr;
void* g_smr_type_obj = nullptr;
void* g_smr_name_str = nullptr;
Entry<GetTransformFn> g_smr_get_root_bone;
Entry<GetAnimParamsFn> g_smr_get_bones;
Entry<GetPropBoolFn> g_go_get_active_hierarchy;
Entry<GetTransformFn> g_transform_get_parent;
Entry<GetLengthFn> g_array_get_length;
Entry<GetValueFn> g_array_get_value;

// The player's AnimatorController parameter names are discovered once at runtime, so the
// clone can be driven by the remote state instead of mirroring the local player.
struct Il2CppClass* g_animator_class = nullptr;
void* g_animator_type_obj = nullptr;
void* g_animator_name_str = nullptr;
bool g_get_component_is_string = false;
char g_swim_param[64] = {0};
char g_speed_param[64] = {0};
char g_speedx_param[64] = {0};
char g_speedy_param[64] = {0};
char g_speedz_param[64] = {0};
char g_pitch_param[64] = {0};
char g_turn_param[64] = {0};
char g_underwater_param[64] = {0};
char g_surface_param[64] = {0};
char g_cinematics_param[64] = {0};
int g_cinematics_param_type = -1;
int32_t g_cinematics_param_id = -1;
int g_swim_param_type = -1;
int g_speed_param_type = -1;
int g_speedx_param_type = -1;
int g_speedy_param_type = -1;
int g_speedz_param_type = -1;
int g_pitch_param_type = -1;
int g_turn_param_type = -1;
int g_underwater_param_type = -1;
int g_surface_param_type = -1;
void* g_swim_param_str = nullptr;
void* g_speed_param_str = nullptr;
void* g_speedx_param_str = nullptr;
void* g_speedy_param_str = nullptr;
void* g_speedz_param_str = nullptr;
void* g_pitch_param_str = nullptr;
void* g_turn_param_str = nullptr;
void* g_underwater_param_str = nullptr;
void* g_surface_param_str = nullptr;
void* g_cinematics_param_str = nullptr;
// Parameter indices, the values actually used at runtime. -1 means "not discovered".
int32_t g_swim_param_id = -1;
int32_t g_speed_param_id = -1;
int32_t g_speedx_param_id = -1;
int32_t g_speedy_param_id = -1;
int32_t g_speedz_param_id = -1;
int32_t g_pitch_param_id = -1;
int32_t g_turn_param_id = -1;
int32_t g_underwater_param_id = -1;
int32_t g_surface_param_id = -1;
bool g_anim_params_logged = false;

// Every float parameter of the controller, cached by name at discovery time. The local
// player's Animator is driven correctly by the game, so reading just these floats shows what
// a properly animated diver looks like without walking the boxed parameter array again.
struct FloatParamSlot {
    char name[64];
    void* str;
};
constexpr size_t kMaxFloatParams = 32;
FloatParamSlot g_float_params[kMaxFloatParams];
size_t g_float_param_count = 0;

// Il2CppString is { klass, monitor, length, chars[1] }, so the text starts at 0x14.
struct StringHeader {
    void* klass;
    void* monitor;
    int32_t length;
};

bool name_contains(void* unity_object, const char* needle) {
    if (!unity_object || !g_get_name) return false;
    void* text = g_get_name.call(unity_object, g_get_name.method);
    if (!text) return false;
    StringHeader* header = reinterpret_cast<StringHeader*>(text);
    if (header->length <= 0 || header->length > 256) return false;

    char16_t* chars = reinterpret_cast<char16_t*>(reinterpret_cast<char*>(text) + 0x14);
    char lowered[257];
    size_t length = static_cast<size_t>(header->length);
    for (size_t i = 0; i < length; i++) {
        lowered[i] = (chars[i] >= 'A' && chars[i] <= 'Z')
                         ? static_cast<char>(chars[i] + 32)
                         : static_cast<char>(chars[i]);
    }
    lowered[length] = '\0';

    char wanted[64];
    size_t i = 0;
    for (; needle[i] && i + 1 < sizeof(wanted); i++) {
        wanted[i] = (needle[i] >= 'A' && needle[i] <= 'Z')
                        ? static_cast<char>(needle[i] + 32)
                        : needle[i];
    }
    wanted[i] = '\0';
    return strstr(lowered, wanted) != nullptr;
}

void log_hierarchy(void* game_object, int depth);

// Copies an Il2CppString into an ascii buffer, preserving case. Returns false on any
// anomaly. The Animator getters compare the name against the controller's own definition,
// so the original casing has to survive; matching for discovery lowercases separately.
bool string_to_ascii(void* text, char* out, size_t capacity) {
    if (!text || capacity == 0) return false;
    StringHeader* header = reinterpret_cast<StringHeader*>(text);
    if (header->length <= 0 || static_cast<size_t>(header->length) + 1 > capacity) return false;

    char16_t* chars = reinterpret_cast<char16_t*>(reinterpret_cast<char*>(text) + 0x14);
    size_t length = static_cast<size_t>(header->length);
    for (size_t i = 0; i < length; i++) {
        if (chars[i] >= 128) return false;
        out[i] = static_cast<char>(chars[i]);
    }
    out[length] = '\0';
    return true;
}

// Copies an Il2CppString into a lowercase ascii buffer. Returns false on any anomaly.
bool string_to_lower(void* text, char* out, size_t capacity) {
    char original[128];
    if (!string_to_ascii(text, original, sizeof(original))) return false;
    size_t length = strlen(original);
    if (length + 1 > capacity) return false;
    for (size_t i = 0; i < length; i++) {
        char c = original[i];
        out[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    }
    out[length] = '\0';
    return true;
}

// Returns the runtime type name (e.g. "system.type") of an Il2CppType via its reflection
// object. Used to tell GetComponent overloads apart without pointer assumptions.
bool il2cpp_type_name(const void* il2cpp_type, char* out, size_t capacity) {
    if (!il2cpp_type || !out || capacity == 0 || !il2cpp_type_get_object ||
        !il2cpp_object_get_class || !il2cpp_class_get_method_from_name) {
        return false;
    }
    void* type_obj = il2cpp_type_get_object(il2cpp_type);
    if (!type_obj) return false;
    struct Il2CppClass* obj_class =
        il2cpp_object_get_class(reinterpret_cast<struct Il2CppObject*>(type_obj));
    if (!obj_class) return false;
    struct Il2CppMethodInfo* to_string =
        il2cpp_class_get_method_from_name(obj_class, "ToString", 0);
    if (!to_string) return false;
    void* entry = *reinterpret_cast<void**>(reinterpret_cast<char*>(to_string));
    if (!entry) return false;
    using ToStringFn = void* (*)(void* self, void* method);
    void* text = reinterpret_cast<ToStringFn>(entry)(type_obj, to_string);
    return string_to_lower(text, out, capacity);
}

// Finds a 1-argument GameObject.GetComponent overload, preferring GetComponent(Type) and
// falling back to GetComponent(string). The kind is reported so the caller passes either
// a reflection Type object or a name string.
struct Il2CppMethodInfo* find_getcomponent(struct Il2CppClass* game_object, bool* out_is_string) {
    if (out_is_string) *out_is_string = false;
    if (!game_object || !il2cpp_class_get_methods || !il2cpp_method_get_name ||
        !il2cpp_method_get_param_count || !il2cpp_method_get_param) {
        return nullptr;
    }

    struct Il2CppMethodInfo* type_overload = nullptr;
    struct Il2CppMethodInfo* string_overload = nullptr;
    void* iterator = nullptr;
    for (struct Il2CppMethodInfo* m = il2cpp_class_get_methods(game_object, &iterator);
         m; m = il2cpp_class_get_methods(game_object, &iterator)) {
        const char* name = il2cpp_method_get_name(m);
        if (!name || strcmp(name, "GetComponent") != 0) continue;
        if (il2cpp_method_get_param_count(m) != 1) continue;
        const void* p0 = il2cpp_method_get_param(m, 0);
        char type_name[128] = {0};
        il2cpp_type_name(p0, type_name, sizeof(type_name));
        LOG_INFO("getcomp candidate: method=%p p0type=%s", (void*)m, type_name);
        if (strcmp(type_name, "system.type") == 0) {
            type_overload = m;
        } else if (strcmp(type_name, "system.string") == 0) {
            string_overload = m;
        }
    }
    if (type_overload) return type_overload;
    if (string_overload) {
        if (out_is_string) *out_is_string = true;
        return string_overload;
    }
    return nullptr;
}

// Same overload problem as GetComponent, and worse here: Animator exposes both
// SetBool(string,bool) and SetBool(int,bool), SetFloat(string,float) and SetFloat(int,float),
// SetTrigger(string) and SetTrigger(int) - every pair has an identical argument count, so
// il2cpp_class_get_method_from_name may hand back the hash-based overload. Passing an
// Il2CppString where an int hash is expected corrupts the lookup, so the first parameter's
// type name is verified before the method is accepted.
struct Il2CppMethodInfo* find_method_typed(struct Il2CppClass* klass, const char* name,
                                           int argc, const char* first_param_type) {
    if (!klass || !il2cpp_class_get_methods) return nullptr;
    void* iterator = nullptr;
    for (struct Il2CppMethodInfo* m = il2cpp_class_get_methods(klass, &iterator);
         m; m = il2cpp_class_get_methods(klass, &iterator)) {
        const char* method_name = il2cpp_method_get_name(m);
        if (!method_name || strcmp(method_name, name) != 0) continue;
        if (il2cpp_method_get_param_count(m) != static_cast<unsigned int>(argc)) continue;
        char type_name[96] = {0};
        il2cpp_type_name(il2cpp_method_get_param(m, 0), type_name, sizeof(type_name));
        if (strcmp(type_name, first_param_type) == 0) return m;
    }
    return nullptr;
}

void* get_animator(void* game_object) {
    if (!game_object || !g_get_component || !g_animator_class || !il2cpp_object_get_class) {
        return nullptr;
    }
    void* arg = g_get_component_is_string ? g_animator_name_str : g_animator_type_obj;
    if (!arg) return nullptr;
    void* component = g_get_component.call(game_object, arg, g_get_component.method);
    if (!component) return nullptr;
    if (il2cpp_object_get_class(reinterpret_cast<struct Il2CppObject*>(component)) !=
        g_animator_class) {
        LOG_INFO("GetComponent(Animator) returned unexpected class");
        return nullptr;
    }
    return component;
}

// The Animator can be perfectly healthy, correctly driven and still have nothing to do with
// what the player sees: a SkinnedMeshRenderer is skinned by the transforms in its bones array,
// not by the Animator's position in the hierarchy. If the clone's renderer ended up pointing at
// the local player's skeleton - which is what Instantiate can leave behind when the source body
// is only a subtree of the avatar - then the clone would show the local player's pose no matter
// what values the clone's own Animator receives, which is exactly the reported symptom.
void* find_component_in_hierarchy(void* game_object, void* type_obj, void* name_str,
                                  int depth) {
    if (!game_object || depth > 5 || !g_get_component) return nullptr;
    void* arg = g_get_component_is_string ? name_str : type_obj;
    if (!arg) return nullptr;
    void* component = g_get_component.call(game_object, arg, g_get_component.method);
    if (component) return component;
    if (!g_get_go_transform || !g_get_child || !g_get_child_count) return nullptr;
    void* tf = g_get_go_transform.call(game_object, g_get_go_transform.method);
    if (!tf) return nullptr;
    int32_t count = g_get_child_count.call(tf, g_get_child_count.method);
    for (int32_t i = 0; i < count; i++) {
        void* child_tf = g_get_child.call(tf, i, g_get_child.method);
        if (!child_tf) continue;
        void* child_go = g_get_game_object ? g_get_game_object.call(child_tf,
                                                                   g_get_game_object.method)
                                           : nullptr;
        void* found = find_component_in_hierarchy(child_go, type_obj, name_str, depth + 1);
        if (found) return found;
    }
    return nullptr;
}

// Reads a managed string's UTF-16 payload into a bounded ASCII buffer.
bool read_managed_name(void* text, char* out, size_t cap);

int g_mirror_nodes = 0;
int g_mirror_changes = 0;

// Switches off the Player script that Object.Instantiate copies along with the body.
//
// This is the actual reason the clone was a copy of the local player. The clone is a Player in
// its own right, so Unity calls Player.Update on it every frame, and that method is the game's
// own animation driver: it is what writes view_pitch, chestAim_lean, chestAim_twist, move_speed
// and is_underwater from the local camera and the local input. It therefore overwrote the peer's
// values every single frame. Everything measured from the outside was consistent with that and
// looked like a mystery:
//
//   - the Animator was healthy and shared the local controller, because it is the same object;
//   - writes read back correctly, because they were read back inside the same frame that wrote
//     them, before the clone's own Player.Update ran again;
//   - the head ignored view_pitch, because Player.Update put the local player's head back;
//   - the pose matched the local player exactly, because Player.Update is the local player's
//     animation, applied to the clone's skeleton.
//
// Disable local animation writers on the copy. Head rendering needs a separate fix:
// SetHeadVisible(false) has already left the copied head renderer in ShadowsOnly mode.
void disable_clone_player(void* clone_go) {
    if (!clone_go || !g_get_component || !g_behaviour_set_enabled) {
        LOG_WARN("cannot disable clone Player: getComponent=%p setEnabled=%p typeObj=%p",
                 (void*)g_get_component.call, (void*)g_behaviour_set_enabled.call, g_player_type_obj);
        return;
    }
    void* arg = g_get_component_is_string ? g_player_name_str : g_player_type_obj;
    if (!arg) {
        LOG_WARN("cannot disable clone Player: no type argument available");
        return;
    }
    void* player = g_get_component.call(clone_go, arg, g_get_component.method);
    if (!player) {
        LOG_WARN("clone has no Player component to disable");
        return;
    }
    bool before = g_behaviour_get_enabled
                      ? g_behaviour_get_enabled.call(player, g_behaviour_get_enabled.method)
                      : true;
    g_behaviour_set_enabled.call(player, false, g_behaviour_set_enabled.method);
    bool after = g_behaviour_get_enabled
                     ? g_behaviour_get_enabled.call(player, g_behaviour_get_enabled.method)
                     : false;
    LOG_INFO("clone Player disabled: component=%p enabled %d -> %d", player, before ? 1 : 0,
             after ? 1 : 0);
}

void* cached_managed_string(const char* ascii);

// Resolves the three methods the remote-body Update path calls, once, from the first clone.
//
// ArmAiming is a private nested class, so it cannot be looked up by name through the domain the
// way ArmsController itself is. It is reached through the runtime class of the leftAim instance
// instead, which is the same class the call will dispatch to.
void resolve_arms_ik_api(void* arms) {
    if (!il2cpp_class_get_method_from_name || !il2cpp_object_get_class) return;
    struct Il2CppClass* klass =
        il2cpp_object_get_class(reinterpret_cast<struct Il2CppObject*>(arms));
    if (!klass) return;
    if (!g_arms_update_hand_ik_weights) {
        g_arms_update_hand_ik_weights.bind(find_method(klass, "UpdateHandIKWeights", 0));
    }
    if (!g_arms_reconfigure) {
        g_arms_reconfigure.bind(find_method(klass, "Reconfigure", 1));
    }
    if (!g_arms_world_targets) g_arms_world_targets.bind(find_method(klass,"SetWorldIKTarget",2));
    if (!g_arm_aiming_update) {
        void* left = *reinterpret_cast<void**>(reinterpret_cast<char*>(arms) +
                                                kArmsLeftAimOffset);
        struct Il2CppClass* aiming =
            left ? il2cpp_object_get_class(reinterpret_cast<struct Il2CppObject*>(left)) : nullptr;
        if (aiming) {
            g_arm_aiming_update.bind(find_method(aiming, "Update", 1));
        }
    }
    LOG_INFO("arms ik api: handWeights=%p reconfigure=%p armAimingUpdate=%p",
             (void*)g_arms_update_hand_ik_weights.call, (void*)g_arms_reconfigure.call,
             (void*)g_arm_aiming_update.call);
}

// Silences the two components that Object.Instantiate copies into the clone and that keep writing
// the local player's state into the clone's Animator.
//
// The clone is an Instantiate of the local "body", so it arrives carrying every component the
// local player_view has. Two of them are driven from Player.main, that is, from the local player,
// and they run their own Update against the clone every single frame:
//
//   ArmsController keeps its own smoothedVelocity plus prevPosition and previousVelocities, holds
//   its own Animator reference, and turns that velocity estimate into animation using
//   smoothSpeedUnderWater / smoothSpeedAboveWater as the smoothing factor. It is also what aims
//   the arms. Nitrox, which solves this same problem in C#, zeroes exactly these two fields on a
//   remote body and comments it as "Move variables to keep player animations from mirroring".
//
//   ConditionRules evaluates rules that reference Player.Main through ArmsController, so with the
//   local player standing next to the clone it feeds the local player's condition values straight
//   into the clone's Animator. Nitrox disables it on remote bodies for the same reason.
//
// This is also why every earlier measurement looked contradictory. Each one was read back inside
// the very frame that wrote it, before these components got their turn, so a perfect round trip
// and a pose copied from the local player were both true at the same time.
void neutralize_clone_drivers(void* clone_go) {
    if (!clone_go || !g_get_component) return;
    if (!g_arms_name_str) g_arms_name_str = cached_managed_string("ArmsController");
    if (!g_rules_name_str) g_rules_name_str = cached_managed_string("ConditionRules");

    void* arms = find_component_in_hierarchy(clone_go, g_arms_type_obj, g_arms_name_str, 0);
    if (arms) {
        float* under =
            reinterpret_cast<float*>(reinterpret_cast<char*>(arms) + kArmsSmoothUnderWaterOffset);
        float* above =
            reinterpret_cast<float*>(reinterpret_cast<char*>(arms) + kArmsSmoothAboveWaterOffset);
        LOG_INFO("clone ArmsController=%p smoothing underWater %.3f -> 0, aboveWater %.3f -> 0",
                 arms, *under, *above);
        *under = 0.0f;
        *above = 0.0f;
        // The component stays enabled, because it owns the arm IK and without it the chest twist
        // swings an arm out sideways: the Animator counter-rotates the arms against the chest, and
        // the IK is what applies that correction on top. What has to stop is the rest of its
        // Update, which writes the local player's state into this Animator, and that is handled by
        // hooked_arms_controller_update skipping everything except the IK. See
        // resolve_arms_ik_api for the methods it calls.
        for (size_t i = 0; i < kMaxAvatars; i++) {
            if (g_remote_arms[i] == nullptr || g_remote_arms[i] == arms) {
                g_remote_arms[i] = arms;
                break;
            }
        }
        resolve_arms_ik_api(arms);
    } else {
        LOG_INFO("clone has no ArmsController to silence");
    }

    void* rules = find_component_in_hierarchy(clone_go, g_rules_type_obj, g_rules_name_str, 0);
    if (rules && g_behaviour_set_enabled) {
        bool before = g_behaviour_get_enabled
                          ? g_behaviour_get_enabled.call(rules, g_behaviour_get_enabled.method)
                          : true;
        g_behaviour_set_enabled.call(rules, false, g_behaviour_set_enabled.method);
        bool after = g_behaviour_get_enabled
                         ? g_behaviour_get_enabled.call(rules, g_behaviour_get_enabled.method)
                         : false;
        LOG_INFO("clone ConditionRules=%p enabled %d -> %d", rules, before ? 1 : 0, after ? 1 : 0);
    } else if (rules) {
        LOG_WARN("clone ConditionRules=%p found but set_enabled is unavailable", rules);
    } else {
        LOG_INFO("clone has no ConditionRules to disable");
    }
}

// Counts components of one type under a GameObject, so the diagnostic can say how many Renderers a
// SkyApplier on the clone would have to cover without needing to build the managed array yet.
int count_components_in_hierarchy(void* game_object, void* type_obj, void* name_str, int depth) {
    if (!game_object || depth > 6 || !g_get_component) return 0;
    int found = 0;
    void* arg = g_get_component_is_string ? name_str : type_obj;
    if (arg && g_get_component.call(game_object, arg, g_get_component.method)) found++;
    if (!g_get_go_transform || !g_get_child || !g_get_child_count) return found;
    void* tf = g_get_go_transform.call(game_object, g_get_go_transform.method);
    if (!tf) return found;
    int32_t count = g_get_child_count.call(tf, g_get_child_count.method);
    for (int32_t i = 0; i < count; i++) {
        void* child_tf = g_get_child.call(tf, i, g_get_child.method);
        if (!child_tf) continue;
        void* child_go = g_get_game_object
                             ? g_get_game_object.call(child_tf, g_get_game_object.method)
                             : nullptr;
        found += count_components_in_hierarchy(child_go, type_obj, name_str, depth + 1);
    }
    return found;
}

// Reports the applier's own fields. Takes the component pointer rather than a root object so the
// same function serves both the hierarchy walk below and the post-attach check on a fresh applier.
void log_sky_applier_fields(const char* label, void* sky, int32_t renderers_in_hierarchy) {
    if (!sky) {
        LOG_INFO("sky %s: applier=none renderersInHierarchy=%d", label, renderers_in_hierarchy);
        return;
    }
    char* base = reinterpret_cast<char*>(sky);
    void* prefab = *reinterpret_cast<void**>(base + kSkyCustomPrefabOffset);
    void* renderers = *reinterpret_cast<void**>(base + kSkyRenderersOffset);
    int32_t renderer_count =
        renderers && g_array_get_length
            ? g_array_get_length.call(renderers, g_array_get_length.method)
            : -1;
    LOG_INFO("sky %s: applier=%p anchorSky=%d dynamic=%d emissiveFromPower=%d prefab=%p "
             "renderers=%p(%d) environmentSky=%p applySky=%p updaterIndex=%d "
             "renderersInHierarchy=%d",
             label, sky, *reinterpret_cast<int32_t*>(base + kSkyAnchorSkyOffset),
             *reinterpret_cast<bool*>(base + kSkyDynamicOffset) ? 1 : 0,
             *reinterpret_cast<bool*>(base + kSkyEmissiveFromPowerOffset) ? 1 : 0, prefab, renderers,
             renderer_count, *reinterpret_cast<void**>(base + kSkyEnvironmentSkyOffset),
             *reinterpret_cast<void**>(base + kSkyApplySkyOffset),
             *reinterpret_cast<int32_t*>(base + kSkyUpdaterIndexOffset), renderers_in_hierarchy);
}

void log_sky_applier(const char* label, void* root_go) {
    void* sky = find_component_in_hierarchy(root_go, g_sky_type_obj, g_sky_name_str, 0);
    log_sky_applier_fields(
        label, sky,
        count_components_in_hierarchy(root_go, g_renderer_type_obj, g_renderer_name_str, 0));
}

// Why the clone goes dark indoors, checked rather than assumed. The game lights the player's body
// through a SkyApplier that sits on the Player root; we clone the body, so the clone may have no
// applier at all, and in an interior with its own sky it then has nothing to take light from.
// This only reports; the fix needs a component added to the clone at runtime.
void diagnose_clone_lighting(void* player_go, void* body_go, void* clone_go) {
    if (!g_sky_type_obj && !g_sky_name_str) {
        LOG_INFO("sky diagnostics unavailable: SkyApplier type not resolved");
        return;
    }
    // The Player root is where the game keeps its applier, the body is what we actually clone,
    // so reporting both tells us which of the two lost its lighting.
    log_sky_applier("local player root", player_go);
    log_sky_applier("local body", body_go);
    log_sky_applier("clone root", clone_go);
}

// Collects the chain of GameObject names from root_go down to target_go, so the same object can be
// found again in the clone by walking the identical chain. Walks upwards from the target because
// that is the only direction available from a leaf.
bool build_relative_path(void* target_go, void* root_go, RelativePath* out) {
    if (!target_go || !root_go || !g_get_game_object || !g_transform_get_parent || !g_get_name) {
        return false;
    }
    size_t found = 0;
    char reversed[kSkyPathMaxDepth][kSkyPathNameChars] = {{0}};
    void* current = target_go;
    while (current && current != root_go && found < kSkyPathMaxDepth) {
        if (!read_managed_name(g_get_name.call(current, g_get_name.method), reversed[found],
                               kSkyPathNameChars)) {
            return false;
        }
        void* transform = g_get_go_transform.call(current, g_get_go_transform.method);
        if (!transform) return false;
        current = g_get_game_object.call(g_transform_get_parent.call(transform, g_transform_get_parent.method),
                                         g_get_game_object.method);
        ++found;
    }
    if (current != root_go) return false;  // ran out of depth, or target is not under the root
    out->depth = found;
    for (size_t i = 0; i < found; i++) {
        memcpy(out->names[i], reversed[found - 1 - i], kSkyPathNameChars);
    }
    return found > 0;
}

// Walks the name chain down from root_go. Returns null on the first name that is not found, which
// is the interesting case: it means the clone's hierarchy is not shaped like the source's.
void* resolve_relative_path(void* root_go, const RelativePath* path) {
    if (!root_go || !g_get_go_transform || !g_get_child || !g_get_child_count || !g_get_game_object) {
        return nullptr;
    }
    void* current = root_go;
    for (size_t i = 0; i < path->depth; i++) {
        void* transform = g_get_go_transform.call(current, g_get_go_transform.method);
        if (!transform) return nullptr;
        int32_t children = g_get_child_count.call(transform, g_get_child_count.method);
        void* next = nullptr;
        for (int32_t c = 0; c < children && !next; c++) {
            void* child_transform = g_get_child.call(transform, c, g_get_child.method);
            if (!child_transform) continue;
            void* child_go = g_get_game_object.call(child_transform, g_get_game_object.method);
            char name[kSkyPathNameChars] = {0};
            if (!child_go || !read_managed_name(g_get_name.call(child_go, g_get_name.method), name,
                                                kSkyPathNameChars)) {
                continue;
            }
            if (strcmp(name, path->names[i]) == 0) next = child_go;
        }
        if (!next) {
            LOG_WARN("sky map: path step %u '%s' not found under %p", static_cast<unsigned>(i),
                     path->names[i], current);
            return nullptr;
        }
        current = next;
    }
    return current;
}

// Builds the clone's own renderers array from the local player's.
//
// The array cannot be shared: those Renderer references belong to the local player's body, so a
// clone holding them would light the local player and stay dark itself. Instead every renderer is
// located by its path in the hierarchy and the matching object is taken from the clone.
//
// A renderer that sits outside the body root is skipped and logged rather than guessed at: the
// clone is an Instantiate of the body, so anything outside it simply does not exist on the clone,
// and inventing a substitute would light the wrong thing.
void populate_clone_sky_renderers(void* sky, void* clone_go) {
    char* dst = reinterpret_cast<char*>(sky);
    void* local_sky =
        find_component_in_hierarchy(g_local_game_object, g_sky_type_obj, g_sky_name_str, 0);
    void* local_renderers =
        local_sky ? *reinterpret_cast<void**>(reinterpret_cast<char*>(local_sky) +
                                              kSkyRenderersOffset)
                  : nullptr;
    if (!local_renderers || !g_array_get_length || !g_array_get_value || !il2cpp_array_new ||
        !il2cpp_object_get_class) {
        LOG_WARN("sky map: prerequisites missing, renderers array left empty");
        return;
    }
    if (!g_transform_get_parent) {
        LOG_WARN("sky map: Transform.get_parent is not bound, cannot map any path");
        return;
    }
    int32_t total =
        g_array_get_length.call(local_renderers, g_array_get_length.method);
    if (total <= 0) {
        LOG_WARN("sky map: local applier has no renderers to copy from");
        return;
    }

    void* mapped[kSkyPathMaxDepth * 4] = {nullptr};
    size_t mapped_count = 0;
    for (int32_t i = 0; i < total && mapped_count < sizeof(mapped) / sizeof(mapped[0]); i++) {
        void* source_renderer =
            g_array_get_value.call(local_renderers, i, g_array_get_value.method);
        if (!source_renderer) continue;
        void* source_go = g_get_game_object.call(source_renderer, g_get_game_object.method);
        RelativePath path;
        if (!build_relative_path(source_go, g_avatar_source, &path)) {
            LOG_INFO("sky map: renderer %d has no path to the body root (outside it, or deeper "
                     "than %zu), skipped",
                     i, kSkyPathMaxDepth);
            continue;
        }
        void* target_go = resolve_relative_path(clone_go, &path);
        void* target_renderer =
            target_go && g_get_component
                ? g_get_component.call(target_go, g_renderer_type_obj, g_get_component.method)
                : nullptr;
        if (!target_renderer) {
            LOG_WARN("sky map: renderer %d resolved to no Renderer on the clone, skipped", i);
            continue;
        }
        mapped[mapped_count++] = target_renderer;
        LOG_INFO("sky map: renderer %d -> clone renderer %p (depth %u)", i, target_renderer,
                 static_cast<unsigned>(path.depth));
    }
    if (mapped_count == 0) {
        LOG_WARN("sky map: nothing resolved, renderers array left empty");
        return;
    }

    // The array class is taken from the local array itself, so it is the exact Renderer[] the game
    // already built rather than one derived by hand.
    struct Il2CppClass* array_class =
        il2cpp_object_get_class(reinterpret_cast<struct Il2CppObject*>(local_renderers));
    void* array = il2cpp_array_new(array_class, static_cast<unsigned int>(mapped_count));
    if (!array) {
        LOG_WARN("sky map: array allocation failed");
        return;
    }
    // Rooted by the component before anything else runs, so a collection between here and the
    // element writes cannot take the array away.
    *reinterpret_cast<void**>(dst + kSkyRenderersOffset) = array;
    char* elements = reinterpret_cast<char*>(array) + kArrayDataOffset;
    for (size_t i = 0; i < mapped_count; i++) {
        *reinterpret_cast<void**>(elements + i * sizeof(void*)) = mapped[i];
    }
    LOG_INFO("sky map: array=%p maxLength=%zu (expected %zu) elements=%zu of %d from the local "
             "applier, layout %s",
             array,
             *reinterpret_cast<size_t*>(reinterpret_cast<char*>(array) + kArrayMaxLengthOffset),
             mapped_count, mapped_count, total,
             *reinterpret_cast<size_t*>(reinterpret_cast<char*>(array) + kArrayMaxLengthOffset) ==
                     mapped_count
                 ? "confirmed"
                 : "MISMATCH");
}

// Gives the clone its own interior lighting.
//
// The game lights a player body through a SkyApplier on the Player root, and the clone is made
// from the body, so the clone arrives with none and reads as a dark silhouette indoors.
//
// The applier's renderers array cannot be copied from the local player: those are references to
// the local player's own Renderer components, so a clone holding them would light the local player
// and leave the clone black - the same trap as the shared skeleton in INC-12. The array has to
// point at the clone's own renderers.
//
// So the component is added instead of filled in. AddComponent runs Awake() itself and Start() on
// the next frame, and Start() is where a SkyApplier sets itself up (1148 bytes against Awake's 12),
// which is where the renderers are expected to be gathered from the component's own hierarchy.
// That ordering is also why the configuration is copied right after the call returns rather than
// after Start: the fields are in place before the game reads them.
void attach_sky_applier(void* clone_go, void** out_applier) {
    *out_applier = nullptr;
    if (!g_add_component || !g_sky_type_obj) {
        LOG_WARN("sky: cannot attach, addComponent=%p skyTypeObj=%p", (void*)g_add_component.call,
                 g_sky_type_obj);
        return;
    }
    LOG_INFO("sky: AddComponent(SkyApplier) on clone %p", clone_go);
    void* sky = g_add_component.call(clone_go, g_sky_type_obj, g_add_component.method);
    if (!sky) {
        LOG_WARN("sky: AddComponent(SkyApplier) returned null on clone %p", clone_go);
        return;
    }
    *out_applier = sky;
    log_sky_applier_fields("freshly added", sky,
                           count_components_in_hierarchy(clone_go, g_renderer_type_obj,
                                                         g_renderer_name_str, 0));

    // The peer owns its environment. A viewer's custom/outdoor sky is not the
    // peer's sky; Auto uses the exact interior parent supplied by the pose.
    char* dst = reinterpret_cast<char*>(sky);
    int32_t anchor = 0; // Skies.Auto
    bool dynamic = true;
    bool emissive = false;
    *reinterpret_cast<int32_t*>(dst + kSkyAnchorSkyOffset) = anchor;
    *reinterpret_cast<bool*>(dst + kSkyDynamicOffset) = dynamic;
    *reinterpret_cast<bool*>(dst + kSkyEmissiveFromPowerOffset) = emissive;
    LOG_INFO("sky: independent peer config anchorSky=%d dynamic=%d emissiveFromPower=%d", anchor,
             dynamic ? 1 : 0, emissive ? 1 : 0);

    // Filled before Start() runs, so the game finds a valid array already in place rather than
    // having to be told about it afterwards.
    populate_clone_sky_renderers(sky, clone_go);
}

// Reports what the added applier ended up with, a frame later, which is when Unity has run its
// Start(). A non-zero renderers count here means the component filled the array from the clone's
// own hierarchy and there is nothing left to build by hand; a null array means it expects the
// array to arrive with the prefab and the mapping has to be done explicitly.
void verify_sky_applier(RemoteAvatar* avatar) {
    if (!avatar->sky_applier || avatar->sky_reported) return;
    avatar->sky_reported = true;
    void* renderers = *reinterpret_cast<void**>(
        reinterpret_cast<char*>(avatar->sky_applier) + kSkyRenderersOffset);
    int32_t count = renderers && g_array_get_length
                        ? g_array_get_length.call(renderers, g_array_get_length.method)
                        : -1;
    log_sky_applier_fields("after Start", avatar->sky_applier,
                           count_components_in_hierarchy(avatar->game_object, g_renderer_type_obj,
                                                         g_renderer_name_str, 0));
    // Forces the applier to redo its work now that the array exists. Without this it would wait for
    // the clone to move further than SkyApplier.updateDistance from where it was first applied,
    // which for a standing clone is never.
    if (count > 0 && g_sky_refresh_dirty) {
        g_sky_refresh_dirty.call(avatar->sky_applier, g_sky_refresh_dirty.method);
        LOG_INFO("sky: RefreshDirtySky called for peer %08x", avatar->id);
        log_sky_applier_fields("after RefreshDirtySky", avatar->sky_applier, -1);
    }
    LOG_INFO("sky verdict for peer %08x: renderers=%d -> %s", avatar->id, count,
             count > 0 ? "array built, fix is in place"
                       : "array still empty, needs explicit renderer mapping");
}

// Stands in for ArmsController.Update on a remote clone.
//
// This is Nitrox's ArmsController_Update_Patch, reproduced because it solves a problem that has no
// other answer. The component has to keep running, since it owns the arm IK: the Animator
// counter-rotates the arms against the chest, and the IK applies that correction on top, so
// switching the component off leaves an arm sticking out sideways whenever the chest twists.
// But the rest of its Update writes the local player's camera and velocity into the clone's
// Animator, which is what made the clone a copy of the local player.
//
// So on a clone the original body is skipped and only the three IK calls are made, in the same
// order and with the same arguments Nitrox uses. On the local player the original runs untouched.
void hooked_arms_controller_update(void* self, void* method) {
    bool remote = false;
    for (size_t i = 0; i < kMaxAvatars; i++) {
        if (g_remote_arms[i] == self) {
            remote = true;
            break;
        }
    }
    if (remote) {
        char* obj = reinterpret_cast<char*>(self);
        if (g_arms_reconfigure &&
            *reinterpret_cast<bool*>(obj + kArmsReconfigureWorldTargetOffset)) {
            g_arms_reconfigure.call(self, nullptr, g_arms_reconfigure.method);
            *reinterpret_cast<bool*>(obj + kArmsReconfigureWorldTargetOffset) = false;
        }
        float smooth = *reinterpret_cast<float*>(obj + kArmsIkToggleTimeOffset);
        void* left = *reinterpret_cast<void**>(obj + kArmsLeftAimOffset);
        void* right = *reinterpret_cast<void**>(obj + kArmsRightAimOffset);
        if (left && g_arm_aiming_update) {
            g_arm_aiming_update.call(left, smooth, g_arm_aiming_update.method);
        }
        if (right && g_arm_aiming_update) {
            g_arm_aiming_update.call(right, smooth, g_arm_aiming_update.method);
        }
        if (g_arms_update_hand_ik_weights) {
            g_arms_update_hand_ik_weights.call(self, g_arms_update_hand_ik_weights.method);
        }
        return;
    }
    if (g_arms_update_original) {
        g_arms_update_original(self, method);
    }
}

// Copies the local player's active flags onto the matching clone nodes, walking both trees in
// parallel by child index and only touching nodes whose names match.
//
// The clone is an Instantiate of the local body, so it starts out with whatever the local player
// had active at that instant and then never receives another equipment update, because none of
// the scripts that toggle these nodes came along. The result was a clone with diveSuit,
// radiationSuit, reinforcedSuit, scubaSuit, stillSuit and generalSuit all active at once: six
// overlapping bodies, which is not a pose anybody can judge. The local player is the authority on
// which suit is worn, so its flags are simply copied over.
void mirror_active_state(void* local_go, void* clone_go, int depth) {
    if (!local_go || !clone_go || depth > 8) return;
    if (g_go_get_active_self && g_set_active) {
        bool local_self =
            g_go_get_active_self.call(local_go, g_go_get_active_self.method);
        bool clone_self =
            g_go_get_active_self.call(clone_go, g_go_get_active_self.method);
        g_mirror_nodes++;
        if (local_self != clone_self) g_mirror_changes++;
        g_set_active.call(clone_go, local_self, g_set_active.method);
    }
    if (!g_get_go_transform || !g_get_child || !g_get_child_count || !g_get_name) return;

    void* local_tf = g_get_go_transform.call(local_go, g_get_go_transform.method);
    void* clone_tf = g_get_go_transform.call(clone_go, g_get_go_transform.method);
    if (!local_tf || !clone_tf) return;

    int32_t local_count = g_get_child_count.call(local_tf, g_get_child_count.method);
    int32_t clone_count = g_get_child_count.call(clone_tf, g_get_child_count.method);
    // The clone may carry children the source does not have - the held-item prop is one. Walk
    // the shared prefix instead of refusing the whole subtree: pairs are matched by name below,
    // so extra clone children at the end are simply never visited.
    int32_t shared = local_count < clone_count ? local_count : clone_count;

    for (int32_t i = 0; i < shared && i < 64; i++) {
        void* local_child_tf = g_get_child.call(local_tf, i, g_get_child.method);
        void* clone_child_tf = g_get_child.call(clone_tf, i, g_get_child.method);
        if (!local_child_tf || !clone_child_tf) continue;
        void* local_child_go =
            g_get_game_object ? g_get_game_object.call(local_child_tf, g_get_game_object.method)
                              : nullptr;
        void* clone_child_go =
            g_get_game_object ? g_get_game_object.call(clone_child_tf, g_get_game_object.method)
                              : nullptr;
        if (!local_child_go || !clone_child_go) continue;

        char local_name[64] = {0};
        char clone_name[64] = {0};
        read_managed_name(g_get_name.call(local_child_go, g_get_name.method), local_name,
                          sizeof(local_name));
        read_managed_name(g_get_name.call(clone_child_go, g_get_name.method), clone_name,
                          sizeof(clone_name));
        // Index parity alone is not identity: only recurse into a pair that is provably the same
        // node, otherwise a renamed node would receive another node's flag.
        if (strcmp(local_name, clone_name) != 0) continue;
        mirror_active_state(local_child_go, clone_child_go, depth + 1);
    }
}

// Reads a managed string's UTF-16 payload into a bounded ASCII buffer.
bool read_managed_name(void* text, char* out, size_t cap) {
    if (!text) return false;
    StringHeader* header = reinterpret_cast<StringHeader*>(text);
    if (header->length <= 0 || header->length >= 256) return false;
    char16_t* chars = reinterpret_cast<char16_t*>(reinterpret_cast<char*>(text) + 0x14);
    size_t n = static_cast<size_t>(header->length);
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++) {
        out[i] = (chars[i] < 128) ? static_cast<char>(chars[i]) : '?';
    }
    out[n] = '\0';
    return true;
}

void* find_animator_in_hierarchy(void* game_object, int depth) {
    if (!game_object || depth > 4) return nullptr;
    void* direct = get_animator(game_object);
    if (direct) return direct;
    if (!g_get_go_transform || !g_get_child || !g_get_child_count || !g_get_game_object) {
        return nullptr;
    }
    void* tf = g_get_go_transform.call(game_object, g_get_go_transform.method);
    if (!tf) return nullptr;
    int32_t count = g_get_child_count.call(tf, g_get_child_count.method);
    for (int32_t i = 0; i < count && i < 24; i++) {
        void* child = g_get_child.call(tf, i, g_get_child.method);
        if (!child) continue;
        void* child_go = g_get_game_object.call(child, g_get_game_object.method);
        if (!child_go) continue;
        void* found = find_animator_in_hierarchy(child_go, depth + 1);
        if (found) return found;
    }
    return nullptr;
}

enum FieldTypeKind { kFieldTypeUnknown = 0, kFieldTypeString = 1, kFieldTypeInt32 = 2 };

// AnimatorControllerParameterType, as stored in AnimatorControllerParameter.m_Type.
enum AnimParamType { kAnimParamFloat = 1, kAnimParamBool = 2, kAnimParamInt = 3, kAnimParamTrigger = 4 };

struct FieldTypeInfo {
    const void* key;
    FieldTypeKind kind;
    char name[96];
};

// Il2CppType pointers are not identity-comparable: one logical type yields different
// pointers depending on whether the entry is the corlib class type or a per-field
// metadata record, so comparing against il2cpp_class_get_type(String) never matches.
// Classify by the runtime type name instead, memoised per distinct pointer, because a
// class only has a handful of distinct field types. The resolved name is kept because it
// is the only way to tell what an unrecognised type actually is.
const FieldTypeInfo* lookup_field_type(const void* field_type) {
    static FieldTypeInfo cache[16];
    static size_t used = 0;

    if (!field_type) return nullptr;
    for (size_t i = 0; i < used; i++) {
        if (cache[i].key == field_type) return &cache[i];
    }
    if (used >= sizeof(cache) / sizeof(cache[0])) return nullptr;

    FieldTypeInfo* slot = &cache[used++];
    slot->key = field_type;
    slot->kind = kFieldTypeUnknown;
    slot->name[0] = '\0';
    if (il2cpp_type_name(field_type, slot->name, sizeof(slot->name))) {
        if (strcmp(slot->name, "system.string") == 0) {
            slot->kind = kFieldTypeString;
        } else if (strcmp(slot->name, "system.int32") == 0) {
            slot->kind = kFieldTypeInt32;
        }
    }
    return slot;
}

// Reads one AnimatorControllerParameter element: the parameter name into out_name and the
// AnimatorControllerParameterType into out_type. The layout comes from metadata rather than
// hardcoded offsets, so a game update that reorders the struct cannot silently break it.
// The name is returned with its original casing because that is what the Animator getters
// compare against; matching for discovery is done on a lowercased copy.
bool read_anim_param(char* element, char* out_name, size_t name_capacity, int* out_type,
                     bool dump_fields, void** out_text = nullptr) {
    out_name[0] = '\0';
    if (out_text) *out_text = nullptr;
    *out_type = -1;
    if (!element || !il2cpp_object_get_class) return false;
    struct Il2CppClass* element_class =
        il2cpp_object_get_class(reinterpret_cast<struct Il2CppObject*>(element));
    if (!element_class) return false;

    void* iterator = nullptr;
    for (void* field = il2cpp_class_get_fields(element_class, &iterator); field;
         field = il2cpp_class_get_fields(element_class, &iterator)) {
        const char* field_name = il2cpp_field_get_name(field);
        if (!field_name) continue;
        size_t offset = static_cast<size_t>(il2cpp_field_get_offset(field));
        const FieldTypeInfo* info = lookup_field_type(il2cpp_field_get_type(field));
        FieldTypeKind kind = info ? info->kind : kFieldTypeUnknown;
        if (dump_fields) {
            LOG_INFO("anim field: %s offset=%zu ftype=%p kind=%d type=%s", field_name, offset,
                     il2cpp_field_get_type(field), static_cast<int>(kind),
                     info ? info->name : "?");
        }
        if (kind == kFieldTypeString) {
            void* text = *reinterpret_cast<void**>(element + offset);
            if (!string_to_ascii(text, out_name, name_capacity)) out_name[0] = '\0';
            if (out_text) *out_text = text;
        } else if (strcmp(field_name, "m_Type") == 0) {
            // m_Type is the AnimatorControllerParameterType enum, whose Il2CppType reports
            // the enum's own name and not system.int32, so the type classifier cannot see it.
            // It is matched by field name instead and the underlying int32 is read directly.
            // m_DefaultInt is an Int32 too, so matching by type alone would end up reporting
            // the default value as the parameter type.
            int32_t value = *reinterpret_cast<int32_t*>(element + offset);
            if (value >= kAnimParamFloat && value <= kAnimParamTrigger) *out_type = value;
        }
    }
    return out_name[0] != '\0';
}

// Reads Animator.parameters once and logs every parameter name, so the clone can be driven
// by name without any hardcoded Unity metadata offsets.
void discover_animator_params(void* animator);
// Returns a GC-pinned Il2CppString for an ASCII name, created once per distinct name.
void* cached_managed_string(const char* ascii);

void discover_animator_params(void* animator) {
    if (g_anim_params_logged || !animator || !g_get_anim_params || !il2cpp_class_get_fields ||
        !il2cpp_field_get_name || !il2cpp_field_get_type || !il2cpp_field_get_offset) {
        return;
    }
    g_anim_params_logged = true;

    void* array = g_get_anim_params.call(animator, g_get_anim_params.method);
    if (!array) {
        LOG_INFO("animator has no parameters array");
        return;
    }
    // Prefer the managed Array.Length over raw header offsets.
    int32_t count = -1;
    if (g_array_get_length) {
        count = g_array_get_length.call(array, g_array_get_length.method);
    }
    if (count < 0) {
        count = *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(array) + 0x18);
    }
    if (il2cpp_object_get_class && il2cpp_class_get_name && il2cpp_class_get_namespace) {
        struct Il2CppClass* array_class = il2cpp_object_get_class(
            reinterpret_cast<struct Il2CppObject*>(array));
        LOG_INFO("anim params array class=%s.%s count=%d",
                 il2cpp_class_get_namespace(array_class), il2cpp_class_get_name(array_class),
                 count);
    } else {
        LOG_INFO("animator parameters count=%d", count);
    }
    if (count <= 0 || count > 1024) {
        LOG_INFO("animator parameters count out of range: %d", count);
        return;
    }

    for (int32_t i = 0; i < count; i++) {
        // Array.GetValue boxes value types, so this works whether the parameter is a
        // class or a struct, with no raw layout assumptions.
        char* element = nullptr;
        if (g_array_get_value) {
            element = reinterpret_cast<char*>(
                g_array_get_value.call(array, i, g_array_get_value.method));
        } else {
            element =
                reinterpret_cast<char**>(reinterpret_cast<char*>(array) + 0x20)[i];
        }
        char param_name[65] = {0};
        char lowered[65] = {0};
        int param_type = -1;
        if (!read_anim_param(element, param_name, sizeof(param_name), &param_type, i == 0)) {
            continue;
        }
        for (size_t k = 0; param_name[k] && k + 1 < sizeof(lowered); k++) {
            char c = param_name[k];
            lowered[k] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        }
        if (!lowered[0]) continue;
        LOG_INFO("anim param[%d] = %s (type=%d)", i, param_name, param_type);

        if (param_type == kAnimParamFloat && g_float_param_count < kMaxFloatParams) {
            FloatParamSlot& slot = g_float_params[g_float_param_count];
            snprintf(slot.name, sizeof(slot.name), "%s", param_name);
            // Pinned through the shared cache, not a bare il2cpp_string_new: an unpinned managed
            // string is collected by the GC within about a minute, after which every SetFloat
            // through it silently does nothing while still reading back the previous value.
            slot.str = cached_managed_string(param_name);
            g_float_param_count++;
        }

        // Nearly every state parameter in this controller is a Trigger rather than a Bool,
        // so the discovered type decides which setter is legal: calling SetBool on a Trigger
        // is rejected by Unity and calling SetFloat on a Bool is a silent no-op. A candidate
        // of the wanted type always wins over one that was only found by substring, and the
        // exact-name matches are the ones the game itself drives, so they are preferred.
        if (strcmp(lowered, "swimming_fast") == 0) {
            snprintf(g_swim_param, sizeof(g_swim_param), "%s", param_name);
            g_swim_param_type = param_type;
            g_swim_param_id = i;
        } else if (!g_swim_param[0] && strstr(lowered, "swim")) {
            snprintf(g_swim_param, sizeof(g_swim_param), "%s", param_name);
            g_swim_param_type = param_type;
            g_swim_param_id = i;
        }
        if (strcmp(lowered, "move_speed") == 0) {
            snprintf(g_speed_param, sizeof(g_speed_param), "%s", param_name);
            g_speed_param_type = param_type;
            g_speed_param_id = i;
        } else if (!g_speed_param[0] &&
                   (strstr(lowered, "speed") || strstr(lowered, "velocity") ||
                    strstr(lowered, "move"))) {
            snprintf(g_speed_param, sizeof(g_speed_param), "%s", param_name);
            g_speed_param_type = param_type;
            g_speed_param_id = i;
        }
        // The per-axis speeds are what the swim cycle blends on; leaving them at zero while
        // move_speed is set is not enough for the horizontal swim pose to appear.
        if (strcmp(lowered, "move_speed_x") == 0) {
            snprintf(g_speedx_param, sizeof(g_speedx_param), "%s", param_name);
            g_speedx_param_type = param_type;
            g_speedx_param_id = i;
        }
        if (strcmp(lowered, "move_speed_y") == 0) {
            snprintf(g_speedy_param, sizeof(g_speedy_param), "%s", param_name);
            g_speedy_param_type = param_type;
            g_speedy_param_id = i;
        }
        if (strcmp(lowered, "move_speed_z") == 0) {
            snprintf(g_speedz_param, sizeof(g_speedz_param), "%s", param_name);
            g_speedz_param_type = param_type;
            g_speedz_param_id = i;
        }
        if (strcmp(lowered, "view_pitch") == 0) {
            snprintf(g_pitch_param, sizeof(g_pitch_param), "%s", param_name);
            g_pitch_param_type = param_type;
            g_pitch_param_id = i;
        }
        if (strcmp(lowered, "view_turn") == 0) {
            snprintf(g_turn_param, sizeof(g_turn_param), "%s", param_name);
            g_turn_param_type = param_type;
            g_turn_param_id = i;
        }
        // is_underwater / on_surface are the two triggers the game itself uses to pick the
        // locomotion state, so they are matched exactly and never by substring.
        if (strcmp(lowered, "is_underwater") == 0) {
            snprintf(g_underwater_param, sizeof(g_underwater_param), "%s", param_name);
            g_underwater_param_type = param_type;
            g_underwater_param_id = i;
        }
        if (strcmp(lowered, "on_surface") == 0) {
            snprintf(g_surface_param, sizeof(g_surface_param), "%s", param_name);
            g_surface_param_type = param_type;
            g_surface_param_id = i;
        }
        // The second trigger the game keeps set on a live player, found by reading the local
        // Animator rather than by guessing from the name.
        if (strcmp(lowered, "cinematics_enabled") == 0) {
            snprintf(g_cinematics_param, sizeof(g_cinematics_param), "%s", param_name);
            g_cinematics_param_type = param_type;
            g_cinematics_param_id = i;
        }
    }

    // The names are kept for the log, and the pinned managed strings are what the Animator
    // calls actually use. Passing raw il2cpp_string_new results instead would work for about a
    // minute and then silently stop, once the garbage collector noticed nothing referenced them.
    struct NamedParam {
        const char* name;
        int32_t* type;
        int32_t* id;
        void** str;
    };
    const NamedParam wanted[] = {
        {g_swim_param, &g_swim_param_type, &g_swim_param_id, &g_swim_param_str},
        {g_speed_param, &g_speed_param_type, &g_speed_param_id, &g_speed_param_str},
        {g_pitch_param, &g_pitch_param_type, &g_pitch_param_id, &g_pitch_param_str},
        {g_turn_param, &g_turn_param_type, &g_turn_param_id, &g_turn_param_str},
        {g_underwater_param, &g_underwater_param_type, &g_underwater_param_id,
         &g_underwater_param_str},
        {g_surface_param, &g_surface_param_type, &g_surface_param_id, &g_surface_param_str},
        {g_cinematics_param, &g_cinematics_param_type, &g_cinematics_param_id,
         &g_cinematics_param_str},
    };
    for (const NamedParam& want : wanted) {
        if (want.name[0]) {
            *want.str = cached_managed_string(want.name);
            LOG_INFO("anim param %s id=%d type=%d str=%p", want.name, *want.id, *want.type,
                     *want.str);
        }
    }
    if (g_speedx_param[0]) {
        g_speedx_param_str = cached_managed_string(g_speedx_param);
        g_speedy_param_str = cached_managed_string(g_speedy_param);
        g_speedz_param_str = cached_managed_string(g_speedz_param);
        LOG_INFO("anim axis speed params = %s/%s/%s id=%d/%d/%d str=%p/%p/%p", g_speedx_param,
                 g_speedy_param, g_speedz_param, g_speedx_param_id, g_speedy_param_id,
                 g_speedz_param_id, g_speedx_param_str, g_speedy_param_str, g_speedz_param_str);
    }

    // The float slot order is the wire order for the mirrored Animator payload, so it is logged
    // explicitly: both phones run the same controller and must agree on it, and if a game update
    // ever renumbers the parameters this line is what shows the mismatch.
    LOG_INFO("anim float payload order: count=%d", static_cast<int>(g_float_param_count));
    for (size_t i = 0; i < g_float_param_count; i++) {
        LOG_INFO("anim float[%d] = %-24s str=%p", static_cast<int>(i), g_float_params[i].name,
                 g_float_params[i].str);
    }
}

// The tool-in-use moment as a flags bit (see kFlagUsingTool): knife swing, scanner scan,
// seaglide burst... Read off the local Animator next to the other bools, every frame.
//
// The game pulses using_tool for ~30 ms, shorter than the 50 ms STATE interval, so a raw
// sample usually misses the swing entirely. Each rising edge is therefore stretched to a
// ~0.5 s latch: every swing is guaranteed several packets with the flag set.
uint32_t read_using_tool_flag() {
    if (!g_local_animator || !g_anim_get_bool) return 0;
    static void* s_using = nullptr;
    if (!s_using) s_using = cached_managed_string("using_tool");
    if (!s_using) return 0;
    bool now = g_anim_get_bool.call(g_local_animator, s_using, g_anim_get_bool.method);
    static bool s_latched = false;
    static unsigned long s_latch_until = 0;
    static bool s_last = false;
    static bool s_had = false;
    if (now && !s_latched) {
        s_latched = true;
        s_latch_until = g_avatar_frame + 15;
    }
    bool out = now || (s_latched && g_avatar_frame < s_latch_until);
    if (!out) s_latched = false;
    if (!s_had || out != s_last) {
        LOG_INFO("using_tool local: %d -> %d", s_last ? 1 : 0, out ? 1 : 0);
        s_last = out;
        s_had = true;
    }
    return out ? kFlagUsingTool : 0;
}

// Reads every discovered float and both state triggers off the local player's own Animator, so
// the clone can be handed the same numbers instead of a reconstruction from guessed names.
void read_local_anim_state(LocalState* out) {
    if (!out || !g_local_animator || !g_anim_get_float || !g_anim_get_bool) return;
    int count = static_cast<int>(g_float_param_count);
    if (count > kNetAnimFloats) count = kNetAnimFloats;
    out->anim_float_count = count;
    for (int i = 0; i < count; i++) {
        void* str = g_float_params[i].str;
        out->anim_floats[i] = str ? g_anim_get_float.call(g_local_animator, str,
                                                           g_anim_get_float.method)
                                  : 0.0f;
    }
    out->anim_underwater = 0;
    out->anim_cinematics = 0;
    if (g_underwater_param_str) {
        out->anim_underwater =
            g_anim_get_bool.call(g_local_animator, g_underwater_param_str, g_anim_get_bool.method)
                ? 1
                : 0;
    }
    if (g_cinematics_param_str) {
        out->anim_cinematics =
            g_anim_get_bool.call(g_local_animator, g_cinematics_param_str, g_anim_get_bool.method)
                ? 1
                : 0;
    }
}

// --- Animator diagnostics -------------------------------------------------
//
// The clone is cloned from the body GameObject, which may carry more than one Animator, and
// only the first one found in the hierarchy is ever driven. Swimming may well live on a
// different component than walking does, which would explain a clone that walks correctly
// but stays upright in the water. These helpers dump every Animator under a root with its
// path, liveness and live parameter values, for the local player and for a clone, so the
// difference between "the trigger is not set" and "the trigger went to the wrong Animator"
// can be told apart without guessing.

// Il2CppString handles are cached by parameter name: the getters take a managed string, and
// allocating a fresh one on every diagnostic tick would leak managed memory for the lifetime
// of the session.
void* cached_managed_string(const char* ascii) {
    static char names[128][64];
    static void* handles[128];
    static size_t used = 0;
    if (!ascii || !il2cpp_string_new) return nullptr;

    for (size_t i = 0; i < used; i++) {
        if (strcmp(names[i], ascii) == 0) return handles[i];
    }
    if (used >= sizeof(handles) / sizeof(handles[0])) return nullptr;

    snprintf(names[used], sizeof(names[used]), "%s", ascii);
    handles[used] = il2cpp_string_new(ascii);
    // Pin it. Caching the pointer is not enough: an Il2CppString is a garbage-collected
    // managed object, and one that only native code holds a pointer to is unreachable from
    // the managed heap. The collector takes it, the pointer goes stale, and Animator calls
    // through it stop matching any parameter while still returning normally - so the values
    // read back as zero and the peer state looks like it is being ignored. A GC handle is a
    // root the collector always scans, so pinning makes the string immortal for the session.
    if (handles[used] && il2cpp_gchandle_new) {
        il2cpp_gchandle_new(reinterpret_cast<Il2CppObject*>(handles[used]), true);
    }
    return handles[used++];
}

// --- Animator set diagnostic -------------------------------------------------
//
// The body animator may never hear about tool use: in first person the game swings the
// VIEWMODEL, which can own a different Animator under the camera rig. If the sender reads
// using_tool off the wrong Animator, the flag never fires and the peer never swings. This
// enumerates every Animator under the local Player once and polls using_tool on each,
// logging transitions with the node path, so the swing's real owner names itself.
struct DiagAnim {
    void* animator;
    char path[128];
    bool using_state;
};
static DiagAnim g_diag_anims[8];
static int g_diag_anim_count = -1;
static void* g_diag_anim_root = nullptr;

void diag_anim_collect(void* go, const char* prefix, int depth) {
    if (!go || depth > 5 || g_diag_anim_count < 0 || g_diag_anim_count >= 8) return;
    if (g_animator_type_obj && g_get_component && !g_get_component_is_string) {
        void* a = g_get_component.call(go, g_animator_type_obj, g_get_component.method);
        if (a) {
            char n[64] = {0};
            if (g_get_name) {
                read_managed_name(g_get_name.call(go, g_get_name.method), n, sizeof(n));
            }
            snprintf(g_diag_anims[g_diag_anim_count].path,
                     sizeof(g_diag_anims[g_diag_anim_count].path), "%s/%s", prefix, n);
            g_diag_anims[g_diag_anim_count].animator = a;
            g_diag_anims[g_diag_anim_count].using_state = false;
            LOG_INFO("animset: [%d] %s/%s (%p)", g_diag_anim_count, prefix, n, a);
            g_diag_anim_count++;
        }
    }
    if (!g_get_go_transform || !g_get_child || !g_get_child_count || !g_get_game_object) return;
    void* tf = g_get_go_transform.call(go, g_get_go_transform.method);
    if (!tf) return;
    int32_t count = g_get_child_count.call(tf, g_get_child_count.method);
    for (int32_t i = 0; i < count && i < 24; i++) {
        void* ctf = g_get_child.call(tf, i, g_get_child.method);
        if (!ctf) continue;
        void* cgo = g_get_game_object.call(ctf, g_get_game_object.method);
        if (!cgo) continue;
        char n[64] = {0};
        if (g_get_name) {
            read_managed_name(g_get_name.call(cgo, g_get_name.method), n, sizeof(n));
        }
        char sub[128] = {0};
        snprintf(sub, sizeof(sub), "%s/%s", prefix, n);
        diag_anim_collect(cgo, sub, depth + 1);
    }
}

void diag_anim_ensure() {
    if (!g_local_game_object) return;
    if (g_diag_anim_root == g_local_game_object && g_diag_anim_count >= 0) return;
    g_diag_anim_root = g_local_game_object;
    g_diag_anim_count = 0;
    diag_anim_collect(g_local_game_object, "", 0);
    // Silent first read so discovery itself does not log fake transitions.
    void* s = cached_managed_string("using_tool");
    if (s && g_anim_get_bool) {
        for (int i = 0; i < g_diag_anim_count; i++) {
            g_diag_anims[i].using_state = g_anim_get_bool.call(g_diag_anims[i].animator, s,
                                                               g_anim_get_bool.method);
        }
    }
    LOG_INFO("animset: %d animator(s) under the local player", g_diag_anim_count);
}

void diag_anim_poll() {
    if (g_diag_anim_count <= 0 || !g_anim_get_bool) return;
    static void* s_using = nullptr;
    if (!s_using) s_using = cached_managed_string("using_tool");
    if (!s_using) return;
    for (int i = 0; i < g_diag_anim_count; i++) {
        bool v =
            g_anim_get_bool.call(g_diag_anims[i].animator, s_using, g_anim_get_bool.method);
        if (v != g_diag_anims[i].using_state) {
            LOG_INFO("animset: [%d] %s using_tool %d -> %d", i, g_diag_anims[i].path,
                     g_diag_anims[i].using_state ? 1 : 0, v ? 1 : 0);
            g_diag_anims[i].using_state = v;
        }
    }
}

void resolve_avatar_source() {
    if (g_avatar_source || !g_local_game_object || !g_get_child || !g_get_child_count) return;

    // g_local_game_object is a GameObject, so it needs the GameObject.transform property.
    void* local_transform = g_get_go_transform.call(g_local_game_object, g_get_go_transform.method);
    if (!local_transform) return;

    int32_t children = g_get_child_count.call(local_transform, g_get_child_count.method);
    LOG_INFO("local player has %d child objects", children);

    for (int32_t i = 0; i < children && i < 24; i++) {
        void* child = g_get_child.call(local_transform, i, g_get_child.method);
        if (!child) continue;
        void* child_go = g_get_game_object ? g_get_game_object.call(child, g_get_game_object.method)
                                          : nullptr;
        if (!child_go) continue;

        void* text = g_get_name ? g_get_name.call(child_go, g_get_name.method) : nullptr;
        if (text) {
            StringHeader* header = reinterpret_cast<StringHeader*>(text);
            if (header->length > 0 && header->length < 64) {
                char16_t* chars =
                    reinterpret_cast<char16_t*>(reinterpret_cast<char*>(text) + 0x14);
                char ascii[65];
                for (int32_t k = 0; k < header->length && k < 64; k++) {
                    ascii[k] = (chars[k] < 128) ? static_cast<char>(chars[k]) : '?';
                }
                ascii[header->length < 64 ? header->length : 64] = '\0';
                LOG_INFO("  child[%d] = %s", i, ascii);
            }
        }

        if (!g_avatar_source && (name_contains(child_go, "body") ||
                                name_contains(child_go, "model"))) {
            g_avatar_source = child_go;
            LOG_INFO("avatar source = child[%d]", i);
        }
    }

    if (!g_avatar_source) {
        LOG_WARN("no child named *body*/*model*, falling back to child[0]");
        void* first = g_get_child.call(local_transform, 0, g_get_child.method);
        g_avatar_source =
            first && g_get_game_object ? g_get_game_object.call(first, g_get_game_object.method)
                                       : nullptr;
        LOG_INFO("avatar source fallback = %p", g_avatar_source);
    }

    log_hierarchy(g_avatar_source, 0);

    // Discover the AnimatorController parameter names once, so every clone can be driven by
    // the remote state instead of mirroring the local player. The Animator may sit on a
    // descendant of the cloned model object rather than on the object itself.
    void* source_animator = find_animator_in_hierarchy(g_avatar_source, 0);
    LOG_INFO("avatar source animator = %p", source_animator);
    g_local_animator = source_animator;
    discover_animator_params(source_animator);
}

// Initialise copied hierarchy activation before mirroring equipment. This alone cannot
// restore a head renderer inherited in first-person ShadowsOnly mode.
void activate_hierarchy(void* game_object, int depth) {
    if (!game_object || depth > 4 || !g_get_go_transform || !g_set_active) return;

    g_set_active.call(game_object, true, g_set_active.method);

    void* tf = g_get_go_transform.call(game_object, g_get_go_transform.method);
    if (!tf) return;
    int32_t count = g_get_child_count.call(tf, g_get_child_count.method);
    for (int32_t i = 0; i < count && i < 24; i++) {
        void* child = g_get_child.call(tf, i, g_get_child.method);
        if (!child) continue;
        void* child_go = g_get_game_object.call(child, g_get_game_object.method);
        if (child_go) activate_hierarchy(child_go, depth + 1);
    }
}

void log_hierarchy(void* game_object, int depth) {
    if (!game_object || depth > 3 || !g_get_go_transform) return;

    void* tf = g_get_go_transform.call(game_object, g_get_go_transform.method);
    if (!tf) return;
    int32_t count = g_get_child_count.call(tf, g_get_child_count.method);
    if (count <= 0) return;

    for (int32_t i = 0; i < count && i < 24; i++) {
        void* child = g_get_child.call(tf, i, g_get_child.method);
        if (!child) continue;
        void* child_go = g_get_game_object.call(child, g_get_game_object.method);
        if (!child_go) continue;

        void* text = g_get_name ? g_get_name.call(child_go, g_get_name.method) : nullptr;
        if (text) {
            StringHeader* header = reinterpret_cast<StringHeader*>(text);
            if (header->length > 0 && header->length < 64) {
                char16_t* chars =
                    reinterpret_cast<char16_t*>(reinterpret_cast<char*>(text) + 0x14);
                char ascii[65];
                for (int32_t k = 0; k < header->length && k < 64; k++) {
                    ascii[k] = (chars[k] < 128) ? static_cast<char>(chars[k]) : '?';
                }
                ascii[header->length < 64 ? header->length : 64] = '\0';
                LOG_INFO("  %*sdesc[%d] = %s", depth * 2, "", i, ascii);
            }
        }
        log_hierarchy(child_go, depth + 1);
    }
}

// Finds a descendant by (case-insensitive) name substring. Used by the clone diagnostics: the
// oxygen tank mesh is expected somewhere under "bottom" and the clone's camera under "Cam",
// both outside the depth-3 dump in resolve_avatar_source.
void* find_child_by_name(void* game_object, const char* needle, int depth) {
    if (!game_object || !needle || depth > 6 || !g_get_go_transform || !g_get_child ||
        !g_get_child_count || !g_get_game_object) {
        return nullptr;
    }
    void* tf = g_get_go_transform.call(game_object, g_get_go_transform.method);
    if (!tf) return nullptr;
    int32_t count = g_get_child_count.call(tf, g_get_child_count.method);
    for (int32_t i = 0; i < count && i < 24; i++) {
        void* child_tf = g_get_child.call(tf, i, g_get_child.method);
        if (!child_tf) continue;
        void* child_go = g_get_game_object.call(child_tf, g_get_game_object.method);
        if (!child_go) continue;
        if (name_contains(child_go, needle)) return child_go;
        void* found = find_child_by_name(child_go, needle, depth + 1);
        if (found) return found;
    }
    return nullptr;
}

// One-shot: dumps the "bottom" subtree of the avatar source, where the oxygen tank mesh is
// expected to live for the equipment sync stage.
void log_bottom_subtree_once() {
    static bool done = false;
    if (done || !g_avatar_source) return;
    done = true;
    void* bottom = find_child_by_name(g_avatar_source, "bottom", 0);
    if (!bottom) {
        LOG_INFO("clone diag: no child named *bottom* under the avatar source");
        return;
    }
    LOG_INFO("clone diag: dumping *bottom* subtree for the tank mesh");
    log_hierarchy(bottom, 0);
}

// One-shot: hunts the oxygen tank mesh for the equipment stage. Whatever the player wears has
// to live under the local Player object (otherwise it would not follow the body), so every
// node matching a tank-ish name is reported with its parent and active flag.
void log_tank_hunt_once() {
    static bool done = false;
    if (done || !g_local_game_object) return;
    done = true;
    static const char* kNeedles[] = {"tank", "oxygen", "highcapacity", "plasteel",
                                     "lightweight", "ultrahigh", "rebreather"};
    for (size_t k = 0; k < sizeof(kNeedles) / sizeof(kNeedles[0]); k++) {
        void* node = find_child_by_name(g_local_game_object, kNeedles[k], 0);
        if (!node) {
            LOG_INFO("tank hunt: no *%s* under the local player", kNeedles[k]);
            continue;
        }
        char name[64] = {0};
        char parent[64] = {0};
        if (g_get_name) {
            read_managed_name(g_get_name.call(node, g_get_name.method), name, sizeof(name));
        }
        if (g_get_go_transform && g_transform_get_parent && g_get_game_object &&
            g_get_name) {
            void* tf =
                g_get_go_transform.call(node, g_get_go_transform.method);
            void* p =
                tf ? g_transform_get_parent.call(tf, g_transform_get_parent.method) : nullptr;
            void* pgo = p ? g_get_game_object.call(p, g_get_game_object.method) : nullptr;
            if (pgo) {
                read_managed_name(g_get_name.call(pgo, g_get_name.method), parent,
                                  sizeof(parent));
            }
        }
        bool active = g_go_get_active_self
                          ? g_go_get_active_self.call(node, g_go_get_active_self.method)
                          : false;
        LOG_INFO("tank hunt: *%s* node '%s' (%p) parent='%s' activeSelf=%d", kNeedles[k], name,
                 node, parent, active ? 1 : 0);
    }
}

// Reports whether the clone carries a live Camera: a second rendering camera on every remote
// body would cost a full scene pass each.
void diagnose_clone_camera(void* clone_go) {
    if (!clone_go || !g_go_get_active_self) return;
    void* cam = find_child_by_name(clone_go, "cam", 0);
    if (!cam) {
        LOG_INFO("clone diag: no *cam* node on the clone");
        return;
    }
    char name[64] = {0};
    if (g_get_name) {
        read_managed_name(g_get_name.call(cam, g_get_name.method), name, sizeof(name));
    }
    bool active = g_go_get_active_self.call(cam, g_go_get_active_self.method);
    LOG_INFO("clone diag: cam node '%s' (%p) activeSelf=%d", name, cam, active ? 1 : 0);
    struct Il2CppClass* camera_class = find_class_in_domain("UnityEngine", "Camera");
    if (camera_class && il2cpp_class_get_type && il2cpp_type_get_object && g_get_component &&
        !g_get_component_is_string) {
        void* camera_type = il2cpp_type_get_object(il2cpp_class_get_type(camera_class));
        if (camera_type) {
            void* camera =
                g_get_component.call(cam, camera_type, g_get_component.method);
            bool enabled = camera && g_behaviour_get_enabled
                               ? g_behaviour_get_enabled.call(camera, g_behaviour_get_enabled.method)
                               : false;
            LOG_INFO("clone diag: Camera component=%p enabled=%d", camera,
                     enabled ? 1 : 0);
        }
    }
}

// Unity's ZXY euler order, computed locally to avoid another managed call.
Quaternion euler_to_quaternion(float pitch, float yaw) {
    constexpr float kDegToRad = 0.01745329252f;
    float half_x = pitch * 0.5f * kDegToRad;
    float half_y = yaw * 0.5f * kDegToRad;
    float sx = sinf(half_x);
    float cx = cosf(half_x);
    float sy = sinf(half_y);
    float cy = cosf(half_y);
    Quaternion q{};
    q.x = sx * cy;
    q.y = cx * sy;
    q.z = -sx * sy;
    q.w = cx * cy;
    return q;
}

RemoteAvatar* find_avatar(uint32_t id) {
    for (size_t i = 0; i < kMaxAvatars; i++) {
        if (g_avatars[i].used && g_avatars[i].id == id) return &g_avatars[i];
    }
    return nullptr;
}

// Defined with the held-item sync, after the item scan it depends on.
void update_avatar_held_prop(RemoteAvatar* avatar, uint32_t tech);
void update_avatar_environment(RemoteAvatar* avatar,const PeerState& peer);
bool avatar_object_live(RemoteAvatar* avatar);
// Defined with the vehicle sync block below; the avatar loop drives them.
void tick_remote_vehicles(const PeerState* peers, size_t peer_count);
void clear_driven_vehicles();
// Defined with spawn_held_prop; the spawn path caches the anchor, the frame loop follows it.
void cache_avatar_hand(RemoteAvatar* avatar);
void follow_avatar_held_prop(RemoteAvatar* avatar);
void apply_peer_held_rotation(RemoteAvatar* avatar,const PeerState& peer);
void apply_peer_held_light(RemoteAvatar* avatar,const PeerState& peer);

void destroy_avatar(RemoteAvatar* avatar) {
    if (!avatar->used) return;
    // The prop is a child of the avatar, so destroying the clone takes it with it.
    if (g_destroy && avatar->game_object) {
        g_destroy.call(avatar->game_object, g_destroy.method);
    }
    memset(avatar, 0, sizeof(*avatar));
}

// Drives the clone's Animator from the remote state: the swim flag is applied through
// whichever setter the discovered parameter type actually allows, the position delta feeds
// the speed parameter, and the peer's head orientation feeds view_pitch/view_turn. Only
// parameters discovered by name in discover_animator_params are ever touched.
void drive_avatar_animator(RemoteAvatar* avatar, const PeerState& peer,
                           const Vector3& position) {
    if (!avatar->animator) return;

    // Detects a component that writes to the clone's Animator after this function does.
    //
    // Every measurement so far was taken inside the same frame that wrote the values, so anything
    // running later in the frame, in its own Update or in FixedUpdate, was invisible: the read
    // back agreed with what had just been written while the rendered pose was something else
    // entirely. Comparing what was written last frame against what is there now, before writing
    // again, makes that overwrite visible and names the parameters it touches.
    //
    // This runs on a timer rather than latching on the first hit, so a clean result is reported
    // too. Silence here would otherwise be ambiguous between "nothing overwrites the clone" and
    // "the detector gave up reporting", and those two need very different amounts of trust.
    if ((g_avatar_frame % 120) == 0 && avatar->written_valid && g_anim_get_float) {
        bool any = false;
        char line[512] = {0};
        size_t used = 0;
        for (int i = 0; i < kWatchedParamCount; i++) {
            if (!g_watch_params[i]) continue;
            float want = avatar->written_floats[i];
            if (want != want) continue;  // not part of the mirrored set
            float now =
                g_anim_get_float.call(avatar->animator, g_watch_params[i], g_anim_get_float.method);
            if (fabsf(now - want) > 0.001f) {
                any = true;
                used += static_cast<size_t>(snprintf(line + used, sizeof(line) - used,
                                                    " %s=%.3f(wrote %.3f)",
                                                    kWatchedParamNames[i], now, want));
                if (used >= sizeof(line) - 1) break;
            }
        }
        if (any) {
            avatar->overwrite_reported = true;
            LOG_INFO("anim overwrite on the clone by another component:%s", line);
        } else if (!avatar->overwrite_reported) {
            LOG_INFO("anim overwrite check: clean, all watched params survived the frame");
        }
    }

    // Peer poses arrive at 20 Hz while this runs every frame, so a per-frame delta is zero on
    // two frames out of three and spikes on the third. Measuring that delta directly made
    // move_speed read 0.00 almost always and 20+ on the odd frame, which is why the swim cycle
    // never played even though the peer was moving: the Animator saw a stationary diver with
    // the occasional teleport. Accumulating distance over ~0.25 s and dividing by the real
    // elapsed time gives a stable value in units per second.
    constexpr unsigned long kSpeedSampleFrames = 15;
    if (!avatar->has_last_position) {
        avatar->has_last_position = true;
        avatar->sample_position = position;
        avatar->sample_frame = g_avatar_frame;
        avatar->dist_accum = 0.0f;
        avatar->speed = 0.0f;
        avatar->velocity = Vector3{0.0f, 0.0f, 0.0f};
    }
    float dx = position.x - avatar->last_position.x;
    float dy = position.y - avatar->last_position.y;
    float dz = position.z - avatar->last_position.z;
    avatar->dist_accum += sqrtf(dx * dx + dy * dy + dz * dz);
    avatar->last_position = position;
    avatar->last_position_frame = g_avatar_frame;

    unsigned long elapsed = g_avatar_frame - avatar->sample_frame;
    if (elapsed >= kSpeedSampleFrames) {
        float dt = static_cast<float>(elapsed) / 60.0f;
        avatar->speed = avatar->dist_accum / dt;
        avatar->velocity = Vector3{(position.x - avatar->sample_position.x) / dt,
                                   (position.y - avatar->sample_position.y) / dt,
                                   (position.z - avatar->sample_position.z) / dt};
        avatar->sample_position = position;
        avatar->sample_frame = g_avatar_frame;
        avatar->dist_accum = 0.0f;
    }
    float speed = avatar->speed;

    // view_turn is body-relative: the clone's root already carries peer.yaw, so what is left
    // for the head is how far the peer is looking away from their own body.
    //
    // The model's euler.y and camAnchor's yaw use opposite conventions, so view_yaw - yaw is
    // pinned at exactly -180 (or +180) instead of describing the head. Sending that raw value
    // asks the Animator to point the head backwards, and the local player's own view_turn is
    // 0.000 while their head points straight ahead - which is what the offset below restores.
    float turn = peer.view_yaw - peer.yaw + 180.0f;
    while (turn > 180.0f) turn -= 360.0f;
    while (turn < -180.0f) turn += 360.0f;
    float pitch = peer.pitch;
    if (pitch > 80.0f) pitch = 80.0f;
    if (pitch < -80.0f) pitch = -80.0f;

    bool swimming = (peer.flags & kFlagSwimming) != 0;
    if (swimming != avatar->last_swim) {
        if (g_swim_param_type == kAnimParamBool && g_anim_set_bool && g_swim_param_str) {
            g_anim_set_bool.call(avatar->animator, g_swim_param_str, swimming,
                                 g_anim_set_bool.method);
        } else if (g_swim_param_type == kAnimParamTrigger) {
            // A trigger is a one-shot transition, not a state: it is set on the rising edge
            // and has to be reset explicitly when swimming ends, otherwise the animator keeps
            // replaying the transition on every re-entry.
            if (swimming && g_anim_set_trigger && g_swim_param_str) {
                g_anim_set_trigger.call(avatar->animator, g_swim_param_str,
                                        g_anim_set_trigger.method);
            } else if (!swimming && g_anim_reset_trigger && g_swim_param_str) {
                g_anim_reset_trigger.call(avatar->animator, g_swim_param_str,
                                          g_anim_reset_trigger.method);
            }
        }
        avatar->last_swim = swimming;
    }

    // is_underwater / on_surface are what actually select the swim animation. A trigger is
    // consumed by the state machine, so it is re-armed every frame while the condition holds
    // and only reset on the falling edge, which keeps the state stable without a rising-edge
    // gate that could miss a transition after the Animator has moved on by itself.
    bool underwater = (peer.flags & kFlagUnderwater) != 0;
    if (underwater) {
        if (g_underwater_param_type == kAnimParamTrigger && g_anim_set_trigger &&
            g_underwater_param_str) {
            g_anim_set_trigger.call(avatar->animator, g_underwater_param_str,
                                    g_anim_set_trigger.method);
        } else if (g_underwater_param_type == kAnimParamBool && g_anim_set_bool &&
                   g_underwater_param_str) {
            g_anim_set_bool.call(avatar->animator, g_underwater_param_str, true,
                                 g_anim_set_bool.method);
        }
    } else if (!avatar->last_underwater) {
        if (g_underwater_param_type == kAnimParamTrigger && g_anim_reset_trigger &&
            g_underwater_param_str) {
            g_anim_reset_trigger.call(avatar->animator, g_underwater_param_str,
                                      g_anim_reset_trigger.method);
        } else if (g_underwater_param_type == kAnimParamBool && g_anim_set_bool &&
                   g_underwater_param_str) {
            g_anim_set_bool.call(avatar->animator, g_underwater_param_str, false,
                                 g_anim_set_bool.method);
        }
    }
    avatar->last_underwater = underwater;

    bool on_surface = (peer.flags & kFlagOnSurface) != 0;
    if (on_surface) {
        if (g_surface_param_type == kAnimParamTrigger && g_anim_set_trigger &&
            g_surface_param_str) {
            g_anim_set_trigger.call(avatar->animator, g_surface_param_str,
                                    g_anim_set_trigger.method);
        } else if (g_surface_param_type == kAnimParamBool && g_anim_set_bool &&
                   g_surface_param_str) {
            g_anim_set_bool.call(avatar->animator, g_surface_param_str, true,
                                 g_anim_set_bool.method);
        }
    } else if (!avatar->last_surface) {
        if (g_surface_param_type == kAnimParamTrigger && g_anim_reset_trigger &&
            g_surface_param_str) {
            g_anim_reset_trigger.call(avatar->animator, g_surface_param_str,
                                      g_anim_reset_trigger.method);
        } else if (g_surface_param_type == kAnimParamBool && g_anim_set_bool &&
                   g_surface_param_str) {
            g_anim_set_bool.call(avatar->animator, g_surface_param_str, false,
                                 g_anim_set_bool.method);
        }
    }
    avatar->last_surface = on_surface;

    // Tool-in-use moment: the peer plays the swing/scan, not just the hold. Set unconditionally
    // every frame - it is transient, and edge tracking could miss a use that fits between two
    // state packets. Drives all three use params: the swing transition may key off
    // using_tool_first/_alt rather than the base flag on this controller.
    if (g_anim_set_bool) {
        static const char* kUsing[3] = {"using_tool", "using_tool_first", "using_tool_alt"};
        static void* s_using[3] = {nullptr, nullptr, nullptr};
        bool use = (peer.flags & kFlagUsingTool) != 0;
        for (int i = 0; i < 3; i++) {
            if (!s_using[i]) s_using[i] = cached_managed_string(kUsing[i]);
            if (s_using[i]) {
                g_anim_set_bool.call(avatar->animator, s_using[i], use, g_anim_set_bool.method);
            }
        }
        // Overwrite watch: if something on the clone clears the flag after this write, the
        // next frame reads it back changed and the log names the eaten swing. Gated on a
        // stable peer flag so rapid clicking (legitimate transitions) stays silent.
        bool peer_use = (peer.flags & kFlagUsingTool) != 0;
        if (g_anim_get_bool && s_using[0] && avatar->written_using_valid &&
            peer_use == avatar->peer_using_last) {
            bool back =
                g_anim_get_bool.call(avatar->animator, s_using[0], g_anim_get_bool.method);
            if (back != avatar->written_using) {
                LOG_INFO("using_tool overwrite: peer %08x wrote %d but reads %d",
                         avatar->id, avatar->written_using ? 1 : 0, back ? 1 : 0);
            }
        }
        avatar->written_using = use;
        avatar->peer_using_last = peer_use;
        avatar->written_using_valid = true;
    }

    if (g_speed_param_type == kAnimParamFloat && g_anim_set_float && g_speed_param_str) {
        g_anim_set_float.call(avatar->animator, g_speed_param_str, speed,
                              g_anim_set_float.method);
    }
    if (g_anim_set_float) {
        if (g_speedx_param_type == kAnimParamFloat && g_speedx_param_str) {
            g_anim_set_float.call(avatar->animator, g_speedx_param_str, avatar->velocity.x,
                                  g_anim_set_float.method);
        }
        if (g_speedy_param_type == kAnimParamFloat && g_speedy_param_str) {
            g_anim_set_float.call(avatar->animator, g_speedy_param_str, avatar->velocity.y,
                                  g_anim_set_float.method);
        }
        if (g_speedz_param_type == kAnimParamFloat && g_speedz_param_str) {
            g_anim_set_float.call(avatar->animator, g_speedz_param_str, avatar->velocity.z,
                                  g_anim_set_float.method);
        }
    }
    if (g_pitch_param_type == kAnimParamFloat && g_anim_set_float && g_pitch_param_str) {
        g_anim_set_float.call(avatar->animator, g_pitch_param_str, pitch,
                              g_anim_set_float.method);
    }
    if (g_turn_param_type == kAnimParamFloat && g_anim_set_float && g_turn_param_str) {
        g_anim_set_float.call(avatar->animator, g_turn_param_str, turn,
                              g_anim_set_float.method);
    }

    // The peer's Animator values are applied verbatim, in the order the controller lists them.
    //
    // Everything before this reconstructed the pose from three parameter names picked because
    // they read like "look angle", and the read-back proved the writes landed while the pose
    // never changed. Reading the local Animator showed why the guess could not work: the
    // controller aims the upper body with chestAim_lean and chestAim_twist, which were never
    // written at all, and it has no swimming_fast trigger to set. Sending all thirteen floats and
    // both state triggers makes the clone's Animator numerically identical to the peer's, so the
    // controller produces the same pose without this code having to know what any name means.
    int mirror_count = peer.anim_float_count;
    if (mirror_count > static_cast<int>(g_float_param_count)) {
        mirror_count = static_cast<int>(g_float_param_count);
    }
    for (int i = 0; i < mirror_count; i++) {
        if (!g_float_params[i].str) continue;
        g_anim_set_float.call(avatar->animator, g_float_params[i].str, peer.anim_floats[i],
                              g_anim_set_float.method);
    }

    // Remember what was asked for, so the next frame can tell whether it survived the rest of the
    // frame. A watched name that is not part of the mirrored set is left as NaN and skipped.
    for (int i = 0; i < kWatchedParamCount; i++) {
        avatar->written_floats[i] = NAN;
        for (int f = 0; f < mirror_count; f++) {
            if (!g_float_params[f].str) continue;
            if (strcmp(g_float_params[f].name, kWatchedParamNames[i]) == 0) {
                avatar->written_floats[i] = peer.anim_floats[f];
                break;
            }
        }
    }
    avatar->written_valid = true;

    // Triggers fire on the transition, so they are set only when the peer's state actually
    // changes. Re-setting one every frame restarts the transition each frame.
    bool peer_underwater = peer.anim_underwater != 0;
    if (!avatar->underwater_mirror_init || peer_underwater != avatar->underwater_mirrored) {
        avatar->underwater_mirror_init = true;
        if (g_underwater_param_type == kAnimParamTrigger) {
            if (peer_underwater) {
                if (g_anim_set_trigger && g_underwater_param_str) {
                    g_anim_set_trigger.call(avatar->animator, g_underwater_param_str,
                                            g_anim_set_trigger.method);
                }
            } else if (g_anim_reset_trigger && g_underwater_param_str) {
                g_anim_reset_trigger.call(avatar->animator, g_underwater_param_str,
                                          g_anim_reset_trigger.method);
            }
        } else if (g_anim_set_bool && g_underwater_param_str) {
            g_anim_set_bool.call(avatar->animator, g_underwater_param_str, peer_underwater,
                                 g_anim_set_bool.method);
        }
        avatar->underwater_mirrored = peer_underwater;
    }

    bool peer_cinematics = peer.anim_cinematics != 0;
    if (!avatar->cinematics_mirror_init || peer_cinematics != avatar->cinematics_mirrored) {
        avatar->cinematics_mirror_init = true;
        if (g_cinematics_param_type == kAnimParamTrigger) {
            if (peer_cinematics) {
                if (g_anim_set_trigger && g_cinematics_param_str) {
                    g_anim_set_trigger.call(avatar->animator, g_cinematics_param_str,
                                            g_anim_set_trigger.method);
                }
            } else if (g_anim_reset_trigger && g_cinematics_param_str) {
                g_anim_reset_trigger.call(avatar->animator, g_cinematics_param_str,
                                          g_anim_reset_trigger.method);
            }
        } else if (g_anim_set_bool && g_cinematics_param_str) {
            g_anim_set_bool.call(avatar->animator, g_cinematics_param_str, peer_cinematics,
                                 g_anim_set_bool.method);
        }
        avatar->cinematics_mirrored = peer_cinematics;
    }

    if ((g_avatar_frame % 120) == 0) {
        LOG_INFO("anim drive: bodyYaw=%.1f viewYaw=%.1f pitch=%.1f speed=%.2f turn=%.1f "
                 "swim=%d underwater=%d onSurface=%d vel=%.2f/%.2f/%.2f",
                 peer.yaw, peer.view_yaw, pitch, speed, turn, swimming ? 1 : 0,
                 underwater ? 1 : 0, on_surface ? 1 : 0, avatar->velocity.x,
                 avatar->velocity.y, avatar->velocity.z);
    }

    // Read the mirrored floats straight back, so a value that fails to survive the round trip is
    // visible without having to reason about it.
    if ((g_avatar_frame % 120) == 0 && g_anim_get_float && mirror_count > 0) {
        int limit = mirror_count < 6 ? mirror_count : 6;
        char line[512] = {0};
        size_t used = 0;
        for (int i = 0; i < limit; i++) {
            if (!g_float_params[i].str) continue;
            float back = g_anim_get_float.call(avatar->animator, g_float_params[i].str,
                                               g_anim_get_float.method);
            used += static_cast<size_t>(
                snprintf(line + used, sizeof(line) - used, " %s=%.3f/%.3f", g_float_params[i].name,
                         back, peer.anim_floats[i]));
            if (used >= sizeof(line) - 1) break;
        }
        LOG_INFO("anim mirror roundtrip (%d floats):%s", mirror_count, line);
    }

    // The periodic full Animator dump that used to live here was removed: it walked every
    // Animator on the clone and logged all 202 parameters twice a second, which grew the log
    // to megabytes and boxed 202 objects per call. It is what took the game down while
    // swimming, and the parameter list it produced is now known, so there is nothing left to
    // sample at runtime.
}

// Everything about an Animator that decides whether it can animate at all: a disabled
// component, a zero speed, no controller or no layers all accept SetFloat silently and then do
// nothing, which looks exactly like "the parameters are being ignored".
void log_animator_health(void* local_animator, void* clone_animator) {
    const char* tag = "pair";
    void* animator = clone_animator ? clone_animator : local_animator;
    bool enabled = g_behaviour_get_enabled
                       ? g_behaviour_get_enabled.call(animator, g_behaviour_get_enabled.method)
                       : false;
    float speed =
        g_anim_get_speed ? g_anim_get_speed.call(animator, g_anim_get_speed.method) : -1.0f;
    int32_t layers = g_anim_get_layer_count
                         ? g_anim_get_layer_count.call(animator, g_anim_get_layer_count.method)
                         : -1;
    int32_t culling = g_anim_get_culling
                          ? g_anim_get_culling.call(animator, g_anim_get_culling.method)
                          : -1;
    int32_t update = g_anim_get_update_mode
                         ? g_anim_get_update_mode.call(animator, g_anim_get_update_mode.method)
                         : -1;
    void* controller = g_anim_get_controller
                           ? g_anim_get_controller.call(animator, g_anim_get_controller.method)
                           : nullptr;
    void* avatar = g_anim_get_avatar
                       ? g_anim_get_avatar.call(animator, g_anim_get_avatar.method)
                       : nullptr;
    // The controller has nine layers and the game drives their weights at runtime. A fresh
    // clone inherits whatever the local player had, so if the swim states live on a layer whose
    // weight is 0 on the clone, every parameter we set is evaluated and then thrown away.
    if (g_anim_get_layer_weight && layers > 0) {
        char local_weights[256] = {0};
        char clone_weights[256] = {0};
        for (int32_t layer = 0; layer < layers && layer < 9; layer++) {
            snprintf(local_weights + strlen(local_weights), sizeof(local_weights) - strlen(local_weights),
                     "%s%.2f", layer ? "," : "",
                     local_animator
                         ? g_anim_get_layer_weight.call(local_animator, layer,
                                                        g_anim_get_layer_weight.method)
                         : -1.0f);
        }
        for (int32_t layer = 0; layer < layers && layer < 9; layer++) {
            snprintf(clone_weights + strlen(clone_weights), sizeof(clone_weights) - strlen(clone_weights),
                     "%s%.2f", layer ? "," : "",
                     g_anim_get_layer_weight.call(animator, layer, g_anim_get_layer_weight.method));
        }
        LOG_INFO("anim layer weights %s: local=%s", tag, local_weights);
        LOG_INFO("anim layer weights %s: clone=%s", tag, clone_weights);
    }
    LOG_INFO("anim health %s: clone=%p local=%p enabled=%d speed=%.2f layers=%d culling=%d update=%d ctrl=%p avatar=%p",
             tag, clone_animator, local_animator, enabled ? 1 : 0, speed, layers, culling, update,
             controller, avatar);
}

// The save subsystem, which is the whole of step 0: handing the host's world to a guest.
//
// The game already owns a complete save API and it is the same one the host-star
// multiplayer mods drive - the host flushes the world to disk and ships the slot, the
// guest loads it into a slot of its own. Only the read-only half is bound here, so the
// paths, slot names and file sizes are observed on the device rather than assumed. Nothing
// in this section writes a file or changes a setting.
//
// SaveLoadManager (dump.cs TypeDefIndex 1711) also carries the two switches that make
// "only the host saves" enforceable by the engine instead of by our own bookkeeping:
// set_autoSaveEnabled and the allowWritingFiles field behind GetAllowWritingFiles. They are
// reported here and deliberately left alone until a guest session actually exists, since
// allowWritingFiles=false would also stop the guest baking its own CellsCache.
using GetMainFn = void* (*)(void* self, void* method);
using GetStrFn = void* (*)(void* self, void* method);
using GetBoolFn = bool (*)(void* self, void* method);
using SetInstanceBoolFn = void (*)(void* self, bool value, void* method);
using GetStrArrayFn = void* (*)(void* self, void* method);

Entry<GetMainFn> g_slm_get_main;
Entry<GetStrFn> g_slm_get_current_slot;
Entry<GetStrFn> g_slm_get_temp_path;
Entry<GetStrFn> g_slm_get_temp_root;
Entry<GetStrArrayFn> g_slm_get_active_slots;
Entry<GetBoolFn> g_slm_get_autosave_enabled;
Entry<GetBoolFn> g_slm_get_allow_writing;
Entry<GetBoolFn> g_slm_is_saving;
Entry<GetBoolFn> g_slm_is_loading;
WorldContextWatch g_world_context_watch;
uint32_t g_world_context_generation=0;
Entry<SetInstanceBoolFn> g_slm_set_autosave_enabled;

// True on the phone hosting this session, and the only one allowed to write the save.
//
// Set two ways, and the first one is not enough on its own: the phone that offers a world claims
// it, and a phone that hears a host in the session steps down to guest. Deciding from the save
// alone fails because a guest that received the host's world has a world loaded too - it read that
// as being the host, kept its own copy of the inventory ids, and silently stopped syncing items.
// g_local_is_host, g_role_known and net_note_role_from_peer are declared above the namespace: the
// pump thread in net.cpp has to reach them.
//
// False until the first report has decided the role. The guest rule has to wait for this: run
// before the decision, the host reads as a guest and switches its own autosave off on the very
// first call, which is exactly what it did.

// Where the slot directories live on Android. patch.py already backs saves up from exactly
// this path (SAVES_REL), so it is the one location the project has confirmed the game both
// reads and writes from inside its own process.
constexpr const char* kSavesRoot =
    "/sdcard/Android/data/com.unknownworlds.subnautica/files/SavedGames";

// Native root files watched for completed saves. World capture additionally
// includes the saved entity cells under CellsCache (not disposable terrain).
constexpr const char* kWorldFiles[] = {"gameinfo.json", "global-objects.bin",
                                       "scene-objects.bin", "screenshot.jpg"};

void log_world_files(const char* label, const char* slot) {
    if (!slot || !*slot) return;
    char path[512];
    for (const char* name : kWorldFiles) {
        snprintf(path, sizeof(path), "%s/%s/%s", kSavesRoot, slot, name);
        struct stat info;
        if (stat(path, &info) != 0) {
            LOG_INFO("save %s: %-18s absent", label, name);
            continue;
        }
        LOG_INFO("save %s: %-18s %lld bytes", label, name, (long long)info.st_size);
    }
}

// Reports the save subsystem. `stage` also pulls the three world-identity files of the current
// slot into memory, which is the first half of the handoff: until the bytes are known to be
// readable from inside the game process there is no point designing the transfer.
// A guest must not write the world. Left alone it autosaves its own copy, and the two diverge
// silently: measured on the devices, the guest's scene-objects.bin had grown to 28971 bytes
// against the host's 24521, with a basePresent flag that disagreed with the host's. The save it
// receives is the host's, so anything the guest writes on top of it is a fork nobody asked for.
//
// Only autoSaveEnabled is turned off. allowWritingFiles is deliberately left alone: it also gates
// the CellsCache bake, and a guest that cannot bake its own terrain pays for it on every load.
void enforce_guest_no_autosave(void* manager) {
    if (!g_role_known || g_local_is_host || !manager) return;
    if (!g_slm_set_autosave_enabled || !g_slm_get_autosave_enabled) return;
    if (!g_slm_get_autosave_enabled.call(nullptr, g_slm_get_autosave_enabled.method)) return;

    g_slm_set_autosave_enabled.call(manager, false, g_slm_set_autosave_enabled.method);
    LOG_INFO("role: guest autosave switched off, the host owns this world's save");
    static bool announced = false;
    if (!announced) {
        announced = true;
        LOG_WARN("role: this phone is a guest; it will not write the save, the host will");
    }
}

void enforce_host_autosave(void* manager) {
    if (!g_role_known || !g_local_is_host || !manager) return;
    if (!g_slm_set_autosave_enabled || !g_slm_get_autosave_enabled) return;
    if (g_slm_get_autosave_enabled.call(nullptr, g_slm_get_autosave_enabled.method)) return;
    g_slm_set_autosave_enabled.call(manager, true, g_slm_set_autosave_enabled.method);
    LOG_INFO("role: host autosave switched on");
}

// Periodic re-publish of the host's world offer. The offer is built once at world load
// (the first log_save_subsystem("world", true) call) and stays frozen from then on:
// autosaves keep rewriting scene-objects.bin on disk, but guests keep receiving the
// stale gen-N snapshot, so buildings the host placed after a guest joined never
// reached them. This tick watches the slot files and, when they change, re-captures
// and re-publishes so the guests' manifests move to the new generation. Runs on the
// game thread; the swap inside net_offer_world is reference-counted for in-flight guests.
double monotonic_seconds();
bool log_save_subsystem(const char* label, bool stage);

void host_offer_refresh_tick() {
    if (!g_role_known || !g_local_is_host || net_peer_count() == 0 || !g_slm_get_main ||
        !g_slm_get_current_slot) {
        return;
    }
    static double next_check = 0.0;
    static long long last_size[4] = {-1, -1, -1, -1};
    static long long last_mtime[4] = {-1, -1, -1, -1};
    static char last_slot[24] = "";
    double now = monotonic_seconds();
    if (now < next_check) return;
    next_check = now + 30.0;

    void* manager = g_slm_get_main.call(nullptr, g_slm_get_main.method);
    if (!manager) return;
    if(!g_slm_is_saving || g_slm_is_saving.call(manager,g_slm_is_saving.method)) return;
    char slot[24];
    if (!string_to_ascii(g_slm_get_current_slot.call(manager, g_slm_get_current_slot.method),
                         slot, sizeof(slot))) {
        return;
    }
    if (!slot[0]) return;

    bool baseline = last_slot[0] == 0;
    bool changed = strcmp(slot, last_slot) != 0;
    for (int i = 0; i < 4; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s/%s", kSavesRoot, slot, kWorldFiles[i]);
        struct stat info;
        if (stat(path, &info) != 0) continue;
        if ((long long)info.st_size != last_size[i] ||
            (long long)info.st_mtime != last_mtime[i]) {
            changed = true;
        }
        last_size[i] = (long long)info.st_size;
        last_mtime[i] = (long long)info.st_mtime;
    }
    snprintf(last_slot, sizeof(last_slot), "%s", slot);
    if (baseline) {
        LOG_INFO("world: refresh watch armed slot=%s scene-objects=%lld", slot,
                 (long long)last_size[2]);
        return;
    }
    if (!changed) return;
    LOG_INFO("world: slot %s files changed on disk, republishing the offer", slot);
    if(!log_save_subsystem("refresh", true)) last_mtime[0]=-1; // retry a refused snapshot
    // The received payload is the freshly captured one; its guid shows whether an
    // autosave actually changes world identity (open question about guid vs content).
    // generation comes from the published offer (net_offer_world bumped it).
    const WorldPayload* captured = world_payload();
    if (captured && captured->valid && captured->content_guid_valid) {
        LOG_INFO("world: republished offer gen=%llu content-guid=%02x%02x%02x%02x",
                 (unsigned long long)net_world_offer_generation(),
                 captured->content_guid[0], captured->content_guid[1],
                 captured->content_guid[2], captured->content_guid[3]);
    } else {
        LOG_WARN("world: republished offer gen=%llu, capture invalid",
                 (unsigned long long)net_world_offer_generation());
    }
}

bool log_save_subsystem(const char* label, bool stage) {
    if (!g_slm_get_main) {
        LOG_WARN("save api (%s): SaveLoadManager not bound, report skipped", label);
        return false;
    }

    void* manager = g_slm_get_main.call(nullptr, g_slm_get_main.method);
    if(stage && (!manager || !g_slm_is_saving || !g_slm_is_loading ||
        g_slm_is_saving.call(manager,g_slm_is_saving.method) ||
        g_slm_is_loading.call(manager,g_slm_is_loading.method))) {
        LOG_INFO("world: capture waits for native save/load completion");
        return false;
    }
    int autosave = -1;
    int may_write = -1;
    if (g_slm_get_autosave_enabled) {
        autosave = g_slm_get_autosave_enabled.call(nullptr, g_slm_get_autosave_enabled.method) ? 1 : 0;
    }
    if (g_slm_get_allow_writing && manager) {
        may_write = g_slm_get_allow_writing.call(manager, g_slm_get_allow_writing.method) ? 1 : 0;
    }
    LOG_INFO("save api (%s): main=%p autoSaveEnabled=%d allowWritingFiles=%d", label, manager,
             autosave, may_write);
    if (!manager) return false;

    bool world_ready=false;
    char text[256];
    if (g_slm_get_current_slot &&
        string_to_ascii(g_slm_get_current_slot.call(manager, g_slm_get_current_slot.method), text,
                        sizeof(text))) {
        LOG_INFO("save api (%s): currentSlot=%s", label, text);
        log_world_files(label, text);
        if (stage) {
            size_t staged = world_capture(text);
            LOG_INFO("save api (%s): world_capture returned %zu bytes", label, staged);
        }
        const WorldPayload* payload = world_payload();
        if (payload && payload->valid && payload->content_guid_valid) {
            // A guest that manually loaded the downloaded slot stays a guest. New worlds
            // create a session only when no accepted membership exists.
            world_ready=true;
            net_session_set_local_world(true, text, payload->content_guid);
            g_local_is_host = net_session_is_host();
            g_role_known = true;
            if (g_local_is_host && payload->valid) {
                LOG_INFO("role: this phone is host of the current session");
                net_offer_world(payload, text);
            } else {
                net_offer_world(nullptr, nullptr);
                LOG_INFO("role: this phone is a session member, not the world authority");
            }
        } else {
            LOG_ERROR("save api (%s): no valid captured content GUID; session unavailable", label);
        }
    } else {
        LOG_WARN("save api (%s): currentSlot unreadable", label);
    }

    if (g_slm_get_temp_root &&
        string_to_ascii(g_slm_get_temp_root.call(nullptr, g_slm_get_temp_root.method), text,
                        sizeof(text))) {
        LOG_INFO("save api (%s): temporarySavePathRoot=%s", label, text);
    }
    if (g_slm_get_temp_path &&
        string_to_ascii(g_slm_get_temp_path.call(nullptr, g_slm_get_temp_path.method), text,
                        sizeof(text))) {
        LOG_INFO("save api (%s): temporarySavePath=%s", label, text);
    }

    if (!g_slm_get_active_slots || !g_array_get_length) return world_ready;
    void* slots = g_slm_get_active_slots.call(manager, g_slm_get_active_slots.method);
    if (!slots) return world_ready;

    int count = g_array_get_length.call(slots, g_array_get_length.method);
    LOG_INFO("save api (%s): %d active slot(s)", label, count);
    for (int i = 0; i < count; i++) {
        void* element = *reinterpret_cast<void**>(reinterpret_cast<char*>(slots) +
                                                  kArrayDataOffset + i * sizeof(void*));
        if (!string_to_ascii(element, text, sizeof(text))) continue;
        LOG_INFO("save api (%s):   slot[%d]=%s", label, i, text);
        log_world_files(label, text);
    }
    return world_ready;
}

#include "world_startup_reports.inc"

// Every lookup is bound and reported on its own, so one name that fails to resolve cannot
// hide the state of the others. That isolation is deliberate: the closest reference project
// shipped a patch whose only fault was a wrong parameter name, and the exception it threw
// silently skipped every patch installed after it.
bool resolve_save_api() {
    struct Il2CppClass* slm = find_class_in_domain("", "SaveLoadManager");
    if (!slm) {
        LOG_WARN("save api: class SaveLoadManager not found");
        return false;
    }

    g_slm_get_main.bind(find_method(slm, "get_main", 0));
    g_slm_get_current_slot.bind(find_method(slm, "GetCurrentSlot", 0));
    g_slm_get_temp_path.bind(find_method(slm, "GetTemporarySavePath", 0));
    g_slm_get_temp_root.bind(find_method(slm, "GetTemporarySavePathRoot", 0));
    g_slm_get_active_slots.bind(find_method(slm, "GetActiveSlotNames", 0));
    g_slm_get_autosave_enabled.bind(find_method(slm, "get_autoSaveEnabled", 0));
    g_slm_get_allow_writing.bind(find_method(slm, "GetAllowWritingFiles", 0));
    g_slm_is_saving.bind(find_method(slm,"get_isSaving",0));
    g_slm_is_loading.bind(find_method(slm,"get_isLoading",0));
    g_slm_set_autosave_enabled.bind(find_method(slm, "set_autoSaveEnabled", 1));

    int unresolved = 0;
    if (!g_slm_get_main) { LOG_WARN("save api: get_main unresolved"); unresolved++; }
    if (!g_slm_get_current_slot) { LOG_WARN("save api: GetCurrentSlot unresolved"); unresolved++; }
    if (!g_slm_get_temp_path) { LOG_WARN("save api: GetTemporarySavePath unresolved"); unresolved++; }
    if (!g_slm_get_temp_root) { LOG_WARN("save api: GetTemporarySavePathRoot unresolved"); unresolved++; }
    if (!g_slm_get_active_slots) { LOG_WARN("save api: GetActiveSlotNames unresolved"); unresolved++; }
    if (!g_slm_get_autosave_enabled) { LOG_WARN("save api: get_autoSaveEnabled unresolved"); unresolved++; }
    if (!g_slm_get_allow_writing) { LOG_WARN("save api: GetAllowWritingFiles unresolved"); unresolved++; }
    if (!g_slm_is_saving) { LOG_WARN("save api: get_isSaving unresolved"); unresolved++; }
    if (!g_slm_is_loading) { LOG_WARN("save api: get_isLoading unresolved"); unresolved++; }
    if (!g_slm_set_autosave_enabled) {
        LOG_WARN("save api: set_autoSaveEnabled unresolved, guest will keep autosaving");
    }

    if (unresolved == 0) LOG_INFO("save api: all 9 SaveLoadManager entry points bound");
    return unresolved == 0;
}

// ---------------------------------------------------------------------------
// Peer-driven equipment (item sync v3)
//
// The sender publishes its real Body/Foots/Head slot TechTypes; the receiver activates the
// matching suit/fins/head nodes on the avatar. The tank mesh rides inside the suit node
// (radiationSuit_tank_geo et al, found by the tank hunt), and each suit node carries its own
// head_geo, so one suit switch dresses body, tank and head together. This replaces the
// local-mirror, which painted every avatar in the local player's suit.
// ---------------------------------------------------------------------------

void set_child_active(void* root, const char* needle, bool active) {
    if (!root || !needle || !g_set_active) return;
    void* node = find_child_by_name(root, needle, 0);
    if (!node) return;
    g_set_active.call(node, active, g_set_active.method);
}

#include "avatar_head_visibility.inc"

// Equipment suit mask layout (u32). Tops are matched by name on both ends (order-free);
// leaves are positional inside their node (stable per build, guarded):
//   bits 0-7:  male_geo children (dive, radiation, reinforced, scuba, still, UltraGlide,
//              SwimCharge, general)
//   bits 8-10:   diveSuit leaves, 11-15 radiationSuit, 16-17 reinforcedSuit, 18-19 scubaSuit,
//   bit 20 stillSuit, 21 UltraGlideFins, 22 SwimChargeFins, 23 generalSuit.
static const char* kEquipTops[8] = {"diveSuit",         "radiationSuit", "reinforcedSuit",
                                    "scubaSuit",        "stillSuit",     "UltraGlideFins",
                                    "SwimChargeFins",   "generalSuit"};
static const int kEquipLeafBase[8] = {8, 11, 16, 18, 20, 21, 22, 23};

// Applies the peer's equipment mask to the avatar: tops by name, leaves positional inside
// their node (see the layout above). A zero mask means "no data" and falls back to the base
// diveSuit instead of switching the whole body off.
void apply_equip_mask(RemoteAvatar* avatar, uint32_t mask) {
    if (!avatar->game_object || !g_get_go_transform || !g_get_child || !g_get_child_count ||
        !g_get_game_object || !g_set_active) {
        return;
    }
    if (mask == 0) {
        set_child_active(avatar->game_object, "diveSuit", true);
        return;
    }
    void* male = find_child_by_name(avatar->game_object, "male_geo", 0);
    if (!male) return;
    for (int t = 0; t < 8; t++) {
        void* top = find_child_by_name(male, kEquipTops[t], 0);
        if (!top) continue;
        g_set_active.call(top, ((mask >> (uint32_t)t) & 1u) != 0, g_set_active.method);
        void* ttf = g_get_go_transform.call(top, g_get_go_transform.method);
        if (!ttf) continue;
        int32_t lc = g_get_child_count.call(ttf, g_get_child_count.method);
        for (int32_t li = 0; li < lc; li++) {
            int bit = kEquipLeafBase[t] + li;
            if (bit >= 32) continue;
            void* ltf = g_get_child.call(ttf, li, g_get_child.method);
            if (!ltf) continue;
            void* lgo = g_get_game_object.call(ltf, g_get_game_object.method);
            if (!lgo) continue;
            g_set_active.call(lgo, ((mask >> (uint32_t)bit) & 1u) != 0, g_set_active.method);
        }
    }
}

// TechType -> holding_* animator parameter. The controller already owns a hold pose per tool,
// so deriving it from held_tech costs no packet bytes. Tools without a dedicated pose keep
// holding_tool, which is set for any non-zero tech.
const char* holding_param_for_tech(uint32_t tech) {
    switch (tech) {
        case 505: return "holding_knife";
        case 507: return "holding_flashlight";
        case 750: return "holding_welder";
        case 509:  // Habitat Builder
        case 752:  // Mobile Vehicle Bay
            return "holding_constructor";
        case 751: return "holding_powerglide";  // Seaglide's internal name
        case 523: return "holding_scanner";
        default: return nullptr;
    }
}

void apply_held_pose(void* animator, uint32_t tech) {
    if (!animator || !g_anim_set_bool) return;
    static const char* kNames[7] = {"holding_knife",   "holding_flashlight", "holding_welder",
                                    "holding_constructor", "holding_powerglide",
                                    "holding_scanner", "holding_tool"};
    static void* s_strs[7] = {nullptr, nullptr, nullptr, nullptr,
                               nullptr, nullptr, nullptr};
    for (int i = 0; i < 7; i++) {
        if (!s_strs[i]) s_strs[i] = cached_managed_string(kNames[i]);
        if (s_strs[i]) {
            g_anim_set_bool.call(animator, s_strs[i], false, g_anim_set_bool.method);
        }
    }
    if (tech && s_strs[6]) {
        g_anim_set_bool.call(animator, s_strs[6], true, g_anim_set_bool.method);
    }
    const char* want = holding_param_for_tech(tech);
    if (want) {
        for (int i = 0; i < 6; i++) {
            if (s_strs[i] && strcmp(kNames[i], want) == 0) {
                g_anim_set_bool.call(animator, s_strs[i], true, g_anim_set_bool.method);
            }
        }
    }
    LOG_INFO("held pose: animator=%p tech %u -> %s", animator, tech, want ? want : "(tool only)");
}

// Dresses the avatar in the peer's real equipment and pose. Runs every frame but touches the
// hierarchy and the Animator only on a change, so the steady state costs a few comparisons.
void apply_peer_equipment(RemoteAvatar* avatar, const PeerState& peer) {
    if (!avatar->game_object) return;
    if (!avatar->equip_applied || peer.suit_tech != avatar->equip_suit ||
        peer.fins_tech != avatar->equip_fins || peer.head_tech != avatar->equip_head) {
        LOG_INFO("equip: avatar %08x mask=0x%x fins=%u head=%u", avatar->id, peer.suit_tech,
                 peer.fins_tech, peer.head_tech);
        apply_equip_mask(avatar, peer.suit_tech);
        restore_avatar_head_visibility(avatar->game_object,g_avatar_source,avatar->id);
        avatar->equip_suit = peer.suit_tech;
        avatar->equip_fins = peer.fins_tech;
        avatar->equip_head = peer.head_tech;
        avatar->equip_applied = true;
    }
    if (avatar->animator && peer.held_tech != avatar->equip_pose_held) {
        apply_held_pose(avatar->animator, peer.held_tech);
        avatar->equip_pose_held = peer.held_tech;
    }
}

void spawn_avatar(const PeerState& peer) {
    resolve_avatar_source();
    if (!g_avatar_source) {
        LOG_WARN("no avatar source object, cannot spawn");
        return;
    }
    for (size_t i = 0; i < kMaxAvatars; i++) {
        if (g_avatars[i].used) continue;

        // Claim the slot before instantiating so a re-entrant call cannot pick it again.
        g_avatars[i].used = true;
        g_avatars[i].id = peer.id;

        Vector3 position{peer.x, peer.y, peer.z};
        Quaternion rotation = euler_to_quaternion(peer.pitch, peer.yaw);
        void* go = g_instantiate.call(g_avatar_source, position, rotation, g_instantiate.method);
        if (!go) {
            LOG_ERROR("Instantiate returned nothing for peer %08x", peer.id);
            memset(&g_avatars[i], 0, sizeof(g_avatars[i]));
            return;
        }
        void* tf =
            g_get_go_transform ? g_get_go_transform.call(go, g_get_go_transform.method) : nullptr;
        activate_hierarchy(go, 0);
        // Must happen before the clone's own Player.Update ever runs, otherwise the script gets
        // one frame of the local player's input first.
        disable_clone_player(go);
        // Same reasoning, different components: these two are on player_view rather than on the
        // Player root, so disable_clone_player never sees them, and they keep overwriting the
        // peer's animation from the local player's state every frame.
        neutralize_clone_drivers(go);
        // Clone diagnostics for the equipment/head stage: second-camera check and the tank hunt.
        diagnose_clone_camera(go);
        log_bottom_subtree_once();
        log_tank_hunt_once();
        // Reports only: establishes whether the missing interior lighting on the clone is really
        // down to a SkyApplier that did not get cloned, before anything is injected to fix it.
        diagnose_clone_lighting(g_local_game_object, g_avatar_source, go);
        attach_sky_applier(go, &g_avatars[i].sky_applier);
        g_avatars[i].sky_reported = false;
        g_avatars[i].game_object = go;
        g_avatars[i].transform = tf;
        cache_avatar_hand(&g_avatars[i]);
        g_avatars[i].last_update = g_avatar_frame;
        g_avatars[i].animator = find_animator_in_hierarchy(go, 0);
        g_avatars[i].has_last_position = false;
        g_avatars[i].last_swim = false;
        g_avatars[i].last_underwater = false;
        g_avatars[i].last_surface = false;
        g_avatars[i].sample_frame = g_avatar_frame;
        g_avatars[i].dist_accum = 0.0f;
        g_avatars[i].speed = 0.0f;
        g_avatars[i].velocity = Vector3{0.0f, 0.0f, 0.0f};

        // An Animator without a controller cannot play a single state, so every SetFloat and
        // SetTrigger below would be accepted and then ignored by the state machine. Subnautica
        // assigns the player's controller from code, and Object.Instantiate only carries over
        // serialized fields, so the clone very likely starts with a null controller and stays
        // frozen in its spawn pose no matter what the peer is doing. Copy the live controller
        // across and say so, because a null here is the whole bug.
        if (g_avatars[i].animator && g_anim_get_controller && g_anim_set_controller) {
            void* local_animator =
                g_local_game_object ? find_animator_in_hierarchy(g_local_game_object, 0) : nullptr;
            g_local_animator = local_animator;
            void* local_controller =
                local_animator ? g_anim_get_controller.call(local_animator,
                                                            g_anim_get_controller.method)
                               : nullptr;
            void* clone_controller = g_anim_get_controller.call(g_avatars[i].animator,
                                                                g_anim_get_controller.method);
            LOG_INFO("anim controller: local=%p clone=%p (local animator=%p)",
                     local_controller, clone_controller, local_animator);
            if (local_controller && local_controller != clone_controller) {
                g_anim_set_controller.call(g_avatars[i].animator, local_controller,
                                           g_anim_set_controller.method);
                void* now = g_anim_get_controller.call(g_avatars[i].animator,
                                                       g_anim_get_controller.method);
                LOG_INFO("anim controller assigned to clone: %p -> %p", clone_controller, now);
            }
        }
        log_animator_health(g_local_animator, g_avatars[i].animator);
        g_mirror_nodes = 0;
        g_mirror_changes = 0;
        // The source, not the Player object: the clone is an Instantiate of the local "body", so
        // the two trees only line up from that node down. Pairing the whole Player object against
        // the body clone stops the walk on the first child and silently mirrors nothing.
        mirror_active_state(g_avatar_source, go, 0);
        LOG_INFO("clone active flags mirrored: src=%p nodes=%d changes=%d (setActive=%p "
                 "activeSelf=%p)",
                 g_avatar_source, g_mirror_nodes, g_mirror_changes, (void*)g_set_active.call,
                 (void*)g_go_get_active_self.call);

        LOG_INFO("spawned avatar for peer %08x from %s (go=%p tf=%p anim=%p)", peer.id,
                 peer.address, go, tf, g_avatars[i].animator);
        return;
    }
    LOG_WARN("no free avatar slot for peer %08x", peer.id);
}
const char* g_avatar_update_stage = "idle";
uint32_t g_avatar_update_peer = 0;

// Runs on the game thread. The wrapper below owns the re-entry guard even when
// one of the Unity calls throws and never reaches the end of this function.
void update_peer_avatars_inner(void* player_self) {
    if (!g_instantiate || !g_set_position || !g_set_euler) return;
    g_avatar_update_stage = "local-object";
    g_avatar_update_peer = 0;
    ++g_avatar_frame;

    // A new world means a new player object, so the old avatars are stale.
    if (player_self && player_self != g_last_player_object) {
        if (g_last_player_object) {
            LOG_INFO("player object changed (%p -> %p), clearing avatars", g_last_player_object,
                     player_self);
            for (size_t i = 0; i < kMaxAvatars; i++) {
                if (g_avatars[i].used) destroy_avatar(&g_avatars[i]);
            }
            // Same for driven vehicles: the scene died with every cached pointer.
            clear_driven_vehicles();
            g_avatar_source = nullptr;
            // The local player's GameObject and Animator belong to the object that just died.
            // Unity frees the managed side immediately, so any pointer kept across this line is
            // a dangling one and the next method call through it is a use-after-free - which is
            // what crashed hooked_player_update with SIGSEGV_ACCERR a minute into a session.
            g_local_game_object = nullptr;
            g_local_animator = nullptr;
        }
        g_last_player_object = player_self;
    }

    if (!g_local_game_object && player_self && g_get_game_object) {
        g_local_game_object = g_get_game_object.call(player_self, g_get_game_object.method);
        LOG_INFO("local player object = %p", g_local_game_object);
    }
    if (!g_local_game_object) {
        return;
    }

    // Reference reading of the local player's own float parameters, which the game keeps
    // correct. Only the cached float names are read - no walk over the parameter array - so
    // this costs 13 getter calls every two seconds instead of the 202 boxed values that used
    // to take the game down.
    // One line instead of thirteen: the previous version wrote a line per float parameter every
    // two seconds, which is log pressure on the game thread for no extra information. What
    // matters is the side-by-side with the clone, so print only the three parameters the pose
    // actually depends on.
    g_avatar_update_stage = "local-animator";
    if (g_anim_get_float && (g_avatar_frame % 120) == 0 && g_local_game_object) {
        // Re-walk the local hierarchy on every tick instead of caching the Animator pointer.
        // The local avatar is rebuilt on respawn, and the cached one keeps pointing into freed
        // managed memory, so reading it through a method pointer is what took the game down.
        void* local_animator = find_animator_in_hierarchy(g_local_game_object, 0);
        g_local_animator = local_animator;
        if (local_animator) {
            float speed = g_speed_param_str
                              ? g_anim_get_float.call(local_animator, g_speed_param_str,
                                                      g_anim_get_float.method)
                              : -1.0f;
            float pitch = g_pitch_param_str
                              ? g_anim_get_float.call(local_animator, g_pitch_param_str,
                                                      g_anim_get_float.method)
                              : -1.0f;
            float turn = g_turn_param_str
                             ? g_anim_get_float.call(local_animator, g_turn_param_str,
                                                     g_anim_get_float.method)
                             : -1.0f;
            LOG_INFO("anim compare local: move_speed=%.3f view_pitch=%.3f view_turn=%.3f", speed,
                     pitch, turn);
        }
    }

    g_avatar_update_stage = "peer-list";
    PeerState peers[kMaxPeers];
    size_t count = net_copy_peers(peers, kMaxPeers);

    for (size_t i = 0; i < count; i++) {
        const PeerState& peer = peers[i];
        // Discovery/HELLO is not a usable player pose. A loading guest publishes
        // no state until the local world guard is ready.
        if(!peer.seq_valid)continue;
        g_avatar_update_peer = peer.id;
        g_avatar_update_stage = "spawn";
        RemoteAvatar* avatar = find_avatar(peer.id);
        if (!avatar) {
            spawn_avatar(peer);
            avatar = find_avatar(peer.id);
        }
        if (!avatar || !avatar->transform) continue;
        g_avatar_update_stage = "liveness";
        if (!avatar_object_live(avatar)) {
            // A streamed-out interior may have destroyed its parented visual copy.
            memset(avatar,0,sizeof(*avatar));
            continue;
        }
        // Move the body before optional equipment/tool work. A visual component
        // failure must not prevent the next received position from being used.
        g_avatar_update_stage = "root-pose";
        Vector3 position{peer.x, peer.y, peer.z};
        g_set_position.call(avatar->transform, position, g_set_position.method);
        Vector3 euler{0.0f, peer.yaw, 0.0f};
        g_set_euler.call(avatar->transform, euler, g_set_euler.method);
        avatar->last_update = g_avatar_frame;

        g_avatar_update_stage = "environment";
        update_avatar_environment(avatar,peer);
        // One frame after the applier was added, Unity has run its Start(). Reports once per peer.
        verify_sky_applier(avatar);
        // Peer-driven equipment (item sync v3): the avatar wears what the peer really wears,
        // read from the peer's Body/Foots/Head slots. This replaces the local-mirror below it,
        // which painted every avatar in the local player's suit: each side saw the other dressed
        // as itself. The spawn-time mirror still normalises a fresh clone; after that the peer
        // owns its look, and the holding_* pose follows the peer's held item for free.
        g_avatar_update_stage = "equipment";
        apply_peer_equipment(avatar, peer);
        // The applier's own idea of which sky it is applying is what decides whether the clone is
        // actually lit, and that changes over time as the clone moves and the updater ticks. Sampled
        // on a timer so a later change is visible rather than inferred from the one-shot verdict.
        if (avatar->sky_applier && (g_avatar_frame % 120) == 0) {
            log_sky_applier_fields("periodic", avatar->sky_applier, -1);
        }

        // Piloting peer: the diver sits inside the vehicle (driven by tick_remote_vehicles
        // below), so the clone hides instead of standing where the seat is.
        g_avatar_update_stage = "visibility";
        bool driving = (peer.flags & kFlagPiloting) != 0;
        if (driving != avatar->piloting_hidden && g_set_active && avatar->game_object) {
            g_set_active.call(avatar->game_object, !driving, g_set_active.method);
            avatar->piloting_hidden = driving;
            LOG_INFO("veh: avatar %08x %s (peer piloting)", avatar->id,
                     driving ? "hidden" : "shown");
        }
        if (driving) continue;

        g_avatar_update_stage = "animation";
        drive_avatar_animator(avatar, peer, position);
        // The peer's held item as a prop on this clone, after the animator so a freshly spawned
        // prop lands on the pose it was created in. The follow re-glues it every frame after.
        g_avatar_update_stage = "held-item";
        update_avatar_held_prop(avatar, peer.held_tech);
        g_avatar_update_stage = "held-follow";
        follow_avatar_held_prop(avatar);
        g_avatar_update_stage = "held-rotation";
        apply_peer_held_rotation(avatar,peer);
        g_avatar_update_stage = "held-light";
        apply_peer_held_light(avatar,peer);
    }

    // Remote vehicles ride on the same peer list: teleport + kinematic + pilot tracking.
    g_avatar_update_peer = 0;
    g_avatar_update_stage = "vehicles";
    tick_remote_vehicles(peers, count);

    // Drop avatars whose peer has been gone for longer than the grace period.
    g_avatar_update_stage = "cleanup";
    for (size_t i = 0; i < kMaxAvatars; i++) {
        if (!g_avatars[i].used) continue;
        bool still_here = false;
        for (size_t j = 0; j < count; j++) {
            if (peers[j].id == g_avatars[i].id) still_here = true;
        }
        if (still_here) continue;
        if (g_avatar_frame - g_avatars[i].last_update < kAvatarGraceFrames) continue;
        LOG_INFO("removing avatar for peer %08x (grace expired)", g_avatars[i].id);
        destroy_avatar(&g_avatars[i]);
    }

    g_avatar_update_stage = "idle";
}

void update_peer_avatars(void* player_self) {
    run_avatar_update_frame(g_updating_avatars,
        [&] { update_peer_avatars_inner(player_self); },
        [] {
            static double next_report = 0;
            static unsigned failures = 0;
            ++failures;
            const double now = monotonic_seconds();
            if (now >= next_report) {
                LOG_WARN("avatar update: game exception stage=%s peer=%08x failures=%u; guard released, next frame will retry",
                         g_avatar_update_stage, g_avatar_update_peer, failures);
                next_report = now + 5.0;
            }
        });
}

void resolve_avatar_api() {
    struct Il2CppClass* game_object = find_class_in_domain("UnityEngine", "GameObject");
    struct Il2CppClass* transform = find_class_in_domain("UnityEngine", "Transform");
    struct Il2CppClass* component = find_class_in_domain("UnityEngine", "Component");
    struct Il2CppClass* object_class = find_class_in_domain("UnityEngine", "Object");

    g_get_game_object.bind(find_method(component, "get_gameObject", 0));
    g_get_go_transform.bind(find_method(game_object, "get_transform", 0));
    g_get_child.bind(find_method(transform, "GetChild", 1));
    g_get_child_count.bind(find_method(transform, "get_childCount", 0));
    // Needed to walk up from a renderer to the body root when mapping the renderers array.
    // Was declared but never bound, which made every mapping report the renderer as outside the
    // root without ever looking at the hierarchy.
    g_transform_get_parent.bind(find_method(transform, "get_parent", 0));
    g_get_name.bind(find_method(game_object, "get_name", 0));
    g_instantiate.bind(find_method(game_object, "Instantiate", 3));
    g_set_position.bind(find_method(transform, "set_position", 1));
    g_set_euler.bind(find_method(transform, "set_eulerAngles", 1));
    g_set_active.bind(find_method(game_object, "SetActive", 1));
    g_destroy.bind(find_method(object_class, "Destroy", 1));

    g_animator_class = find_class_in_domain("UnityEngine", "Animator");
    if (g_animator_class) {
        if (il2cpp_class_get_type && il2cpp_type_get_object) {
            const void* animator_type = il2cpp_class_get_type(g_animator_class);
            if (animator_type) {
                g_animator_type_obj = il2cpp_type_get_object(animator_type);
            }
        }
        if (il2cpp_string_new) {
            g_animator_name_str = il2cpp_string_new("Animator");
        }
    }
    g_get_component.bind(find_getcomponent(game_object, &g_get_component_is_string));

    // SkinnedMeshRenderer, to find out which skeleton the clone is actually rendered with.
    g_smr_name_str = cached_managed_string("SkinnedMeshRenderer");
    // Not in corlib: it lives in UnityEngine.CoreModule, so it has to be looked up the same way
    // Animator is, by scanning the domain's images rather than by asking mscorlib.
    g_smr_class = find_class_in_domain("UnityEngine", "SkinnedMeshRenderer");
    if (g_smr_class && il2cpp_class_get_type && il2cpp_type_get_object) {
        g_smr_type_obj = il2cpp_type_get_object(il2cpp_class_get_type(g_smr_class));
    }
    if (g_smr_class) {
        g_smr_get_root_bone.bind(find_method(g_smr_class, "get_rootBone", 0));
        g_smr_get_bones.bind(find_method(g_smr_class, "get_bones", 0));
    }
    g_go_get_active_hierarchy.bind(find_method(game_object, "get_activeInHierarchy", 0));
    LOG_INFO("smr api: class=%p typeObj=%p nameStr=%p rootBone=%p bones=%p", (void*)g_smr_class,
             g_smr_type_obj, g_smr_name_str, (void*)g_smr_get_root_bone.call,
             (void*)g_smr_get_bones.call);

    if (g_animator_class) {
        g_get_anim_params.bind(find_method(g_animator_class, "get_parameters", 0));
        g_anim_set_bool.bind(find_method_typed(g_animator_class, "SetBool", 2, "system.string"));
        g_anim_set_float.bind(
            find_method_typed(g_animator_class, "SetFloat", 2, "system.string"));
        g_anim_set_trigger.bind(
            find_method_typed(g_animator_class, "SetTrigger", 1, "system.string"));
        g_anim_reset_trigger.bind(
            find_method_typed(g_animator_class, "ResetTrigger", 1, "system.string"));
        g_anim_get_bool.bind(
            find_method_typed(g_animator_class, "GetBool", 1, "system.string"));
        g_anim_get_float.bind(
            find_method_typed(g_animator_class, "GetFloat", 1, "system.string"));
        g_anim_get_int.bind(
            find_method_typed(g_animator_class, "GetInteger", 1, "system.string"));
        g_anim_get_root_motion.bind(find_method(g_animator_class, "get_applyRootMotion", 0));
        g_anim_get_layer_count.bind(find_method(g_animator_class, "get_layerCount", 0));
    g_anim_get_controller.bind(
        find_method(g_animator_class, "get_runtimeAnimatorController", 0));
    g_anim_set_controller.bind(
        find_method_typed(g_animator_class, "set_runtimeAnimatorController", 1,
                          "unityengine.runtimeanimatorcontroller"));
    g_anim_get_culling.bind(find_method(g_animator_class, "get_cullingMode", 0));
    g_anim_get_update_mode.bind(find_method(g_animator_class, "get_updateMode", 0));
    g_anim_get_speed.bind(find_method(g_animator_class, "get_speed", 0));
    g_anim_get_avatar.bind(find_method(g_animator_class, "get_avatar", 0));
    g_anim_get_layer_weight.bind(find_method(g_animator_class, "GetLayerWeight", 1));

    // Prefer the int-id overloads. The string-based ones need an Il2CppString that nothing in
    // the managed heap references, so the garbage collector takes it away and every call through
    // it stops matching a parameter. The id forms take a plain index and cannot be collected.
    g_anim_set_float_id.bind(
        find_method_typed(g_animator_class, "SetFloat", 2, "system.int32"));
    g_anim_get_float_id.bind(
        find_method_typed(g_animator_class, "GetFloat", 1, "system.int32"));
    g_anim_set_trigger_id.bind(
        find_method_typed(g_animator_class, "SetTrigger", 1, "system.int32"));
    g_anim_reset_trigger_id.bind(
        find_method_typed(g_animator_class, "ResetTrigger", 1, "system.int32"));
    g_anim_set_bool_id.bind(find_method_typed(g_animator_class, "SetBool", 2, "system.int32"));
    LOG_INFO("anim int-id api: float=%d get=%d trigger=%d reset=%d bool=%d",
             g_anim_set_float_id ? 1 : 0, g_anim_get_float_id ? 1 : 0,
             g_anim_set_trigger_id ? 1 : 0, g_anim_reset_trigger_id ? 1 : 0,
             g_anim_set_bool_id ? 1 : 0);
        // Behaviour is not re-exported by Animator, so get_enabled is resolved on the base
        // class. Without it there is no way to tell a live animator from a disabled one.
        struct Il2CppClass* behaviour = find_class_in_domain("UnityEngine", "Behaviour");
        if (behaviour) {
            g_behaviour_get_enabled.bind(find_method(behaviour, "get_enabled", 0));
            // Used to switch off the Player script that comes along with the cloned body. See
            // disable_clone_player for why that script has to go.
            g_behaviour_set_enabled.bind(find_method(behaviour, "set_enabled", 1));
        }
        struct Il2CppClass* player_class = find_class_in_domain("", "Player");
        g_avatar_get_environment.bind(find_method(player_class,"GetSkyEnvironment",0));
        if (player_class && il2cpp_class_get_type && il2cpp_type_get_object) {
            g_player_type_obj = il2cpp_type_get_object(il2cpp_class_get_type(player_class));
        }
        g_player_name_str = cached_managed_string("Player");
        // Both live on player_view rather than on the Player root, so they come along with the
        // cloned body and have to be silenced explicitly. See neutralize_clone_drivers.
        struct Il2CppClass* arms_class = find_class_in_domain("", "ArmsController");
        if (arms_class && il2cpp_class_get_type && il2cpp_type_get_object) {
            g_arms_type_obj = il2cpp_type_get_object(il2cpp_class_get_type(arms_class));
        }
        struct Il2CppClass* rules_class = find_class_in_domain("", "ConditionRules");
        if (rules_class && il2cpp_class_get_type && il2cpp_type_get_object) {
            g_rules_type_obj = il2cpp_type_get_object(il2cpp_class_get_type(rules_class));
        }
        struct Il2CppClass* sky_class = find_class_in_domain("", "SkyApplier");
        if (sky_class && il2cpp_class_get_type && il2cpp_type_get_object) {
            g_sky_type_obj = il2cpp_type_get_object(il2cpp_class_get_type(sky_class));
            g_sky_refresh_dirty.bind(find_method(sky_class, "RefreshDirtySky", 0));
            g_avatar_apply_environment.bind(find_method(sky_class,"GetAndApplySkybox",1));
        }
        // Two Broadcast(GameObject, GameObject|Component) overloads share argc=2: pick by
        // first parameter type, the same way GetComponent/AddComponent overloads are told apart.
        struct Il2CppClass* sky_env_class = find_class_in_domain("", "SkyEnvironmentChanged");
        if (sky_env_class) {
            g_sky_env_broadcast.bind(
                find_method_typed(sky_env_class, "Broadcast", 2, "unityengine.gameobject"));
        }
        g_sky_name_str = cached_managed_string("SkyApplier");
        auto* subroot_class=find_class_in_domain("","SubRoot");
        if (subroot_class && il2cpp_class_get_type && il2cpp_type_get_object)
            g_avatar_subroot_type=il2cpp_type_get_object(il2cpp_class_get_type(subroot_class));
        // AddComponent(Type) is a managed wrapper around the internal MonoAddComponentWithType
        // call; only the wrapper is wanted here, since the internal one is a free function that
        // expects a different argument layout.
        g_add_component.bind(find_method_typed(game_object, "AddComponent", 1, "system.type"));
        struct Il2CppClass* renderer_class = find_class_in_domain("UnityEngine", "Renderer");
        if (renderer_class && il2cpp_class_get_type && il2cpp_type_get_object) {
            g_renderer_type_obj = il2cpp_type_get_object(il2cpp_class_get_type(renderer_class));
        }
        if (renderer_class) {
            g_head_get_shadow.bind(find_method(renderer_class,"get_shadowCastingMode",0));
            g_head_set_shadow.bind(find_method(renderer_class,"set_shadowCastingMode",1));
        }
        LOG_INFO("avatar head api: shadow_get=%p shadow_set=%p",(void*)g_head_get_shadow.call,
            (void*)g_head_set_shadow.call);
        g_renderer_name_str = cached_managed_string("Renderer");
        for (int i = 0; i < kWatchedParamCount; i++) {
            g_watch_params[i] = cached_managed_string(kWatchedParamNames[i]);
        }
        LOG_INFO("behaviour api: get_enabled=%p set_enabled=%p playerTypeObj=%p armsTypeObj=%p "
                 "rulesTypeObj=%p skyTypeObj=%p addComponent=%p",
                 (void*)g_behaviour_get_enabled.call, (void*)g_behaviour_set_enabled.call,
                 g_player_type_obj, g_arms_type_obj, g_rules_type_obj, g_sky_type_obj,
                 (void*)g_add_component.call);
        if (game_object) {
            g_go_get_active_self.bind(find_method(game_object, "get_activeSelf", 0));
        }
    }
    if (il2cpp_get_corlib && il2cpp_class_from_name) {
        struct Il2CppClass* array_class =
            il2cpp_class_from_name(il2cpp_get_corlib(), "System", "Array");
        if (array_class) {
            g_array_get_length.bind(find_method(array_class, "get_Length", 0));
            g_array_get_value.bind(find_method(array_class, "GetValue", 1));
        }
    }

    LOG_INFO("avatar api: gameObject=%p go_transform=%p GetChild=%p childCount=%p getParent=%p "
             "name=%p instantiate=%p set_position=%p set_euler=%p destroy=%p",
             (void*)g_get_game_object.call, (void*)g_get_go_transform.call, (void*)g_get_child.call,
             (void*)g_get_child_count.call, (void*)g_transform_get_parent.call, (void*)g_get_name.call,
             (void*)g_instantiate.call, (void*)g_set_position.call, (void*)g_set_euler.call,
             (void*)g_destroy.call);
    LOG_INFO("anim api: animator=%p typeObj=%p nameStr=%p getComponent=%p(%s) getParams=%p "
             "setBool=%p setFloat=%p setTrigger=%p resetTrigger=%p getLength=%p getValue=%p",
             (void*)g_animator_class, g_animator_type_obj, g_animator_name_str,
             (void*)g_get_component.call, g_get_component_is_string ? "string" : "type",
             (void*)g_get_anim_params.call, (void*)g_anim_set_bool.call,
             (void*)g_anim_set_float.call, (void*)g_anim_set_trigger.call,
             (void*)g_anim_reset_trigger.call, (void*)g_array_get_length.call,
             (void*)g_array_get_value.call);
}

// ---------------------------------------------------------------------------
// Item pickup and drop
//
// Pickupable.Pickup and Pickupable.Drop are the two moments the world actually changes, so they
// are the whole of the first sync pass. The object is identified by UniqueIdentifier.Id rather
// than by a managed pointer: pointers are per-process, and a prefab name is shared by every
// instance of it, so neither survives the trip between two phones.
//
// Applying a remote event calls Pickupable.Drop directly rather than going through the game's
// own drop path, and that call passes back through hooked_pickupable_drop. g_applying_remote is
// what stops the result from being queued and sent straight back out - without it one remote
// pickup would produce an endless echo between the two phones.
// ---------------------------------------------------------------------------

using PickupFn = void (*)(void* self, bool events, void* method);
using DropFn = void (*)(void* self, void* method);
using DropAtFn = void (*)(void* self, Vector3 position, Vector3 push, bool check, void* method);
using GetStringFn = void* (*)(void* self, void* method);
using FindObjectsFn = void* (*)(void* type, void* method);
using FindObjectsAllFn = void* (*)(void* type, bool include_inactive, void* method);
using GetVector3Fn = Vector3 (*)(void* self, void* method);
using SetQuaternionFn = void (*)(void* self, Quaternion value, void* method);
using GetQuaternionFn = Quaternion (*)(void* self, void* method);
using SetStringFn = void (*)(void* self, void* value, void* method);
// Pickupable.GetTechType returns the TechType enum, which crosses the ABI as a plain int.
using GetTechTypeFn = uint32_t (*)(void* self, void* method);
// Equipment.GetTechTypeInSlot(slot) -> TechType as int. The slot arrives as a managed string.
using GetTechInSlotFn = uint32_t (*)(void* self, void* slot, void* method);
// Transform.SetParent(parent, worldPositionStays).
using SetParentFn = void (*)(void* self, void* parent, bool stays, void* method);
// GameObject.set_layer(int). Held v3 moves every prop onto Default so inventory-sourced
// viewmodel copies stop inheriting the layer the main camera never draws.
using SetLayerFn = void (*)(void* self, int32_t value, void* method);
// Renderer.get_bounds -> world-space Bounds (center + extents, 24 bytes) for the prop
// auto-centering below.
struct Bounds {
    Vector3 center;
    Vector3 extents;
};
using GetBoundsFn = Bounds (*)(void* self, void* method);
// CraftData.GetClassIdForTechType(TechType) -> string. Static, so no self argument - the same
// convention as the FindObjectsOfType entries.
using GetClassIdFn = void* (*)(uint32_t tech, void* method);
// CraftData.TryGetScenePrefab(classId, out prefab) -> bool. The prefab comes back through the
// pointer slot; it is never activated or reparented, only cloned from.
using TryScenePrefabFn = bool (*)(void* class_id, void** prefab, void* method);
// CraftData.GetPrefabForTechTypeAsync(TechType, bool verbose): fire-and-forget bundle preload.
// The game pumps the coroutine itself; a later TryGetScenePrefab then hits for tools whose
// bundle was never touched on this phone.
using PrefabPreloadFn = void (*)(uint32_t tech, bool verbose, void* method);
// PrefabDatabase.GetPrefabAsync(classId) -> IPrefabRequest. Static. Unlike the coroutine
// loader, the request object is polled, which is what makes it usable from native code.
using GetPrefabAsyncFn = void* (*)(void* class_id, void* method);
// IPrefabRequest.TryGetPrefab(out prefab) -> bool. Bound on the CONCRETE request class
// (Loaded/LoadingPrefabRequest): Entry calls the raw methodPointer, and interface MethodInfos
// carry no code, so binding the interface method would jump into nowhere.
using TryGetPrefabFn = bool (*)(void* self, void** prefab, void* method);
// IEnumerator.MoveNext, which drives a LoadingPrefabRequest. Same bool convention as the
// PredicateFn entries.
using MoveNextFn = bool (*)(void* self, void* method);

Entry<PickupFn> g_pickup_original_slot;      // entry point captured before patching
Entry<DropFn> g_drop_original_slot;
PickupFn g_pickup_original = nullptr;
DropFn g_drop_original = nullptr;

Entry<GetStringFn> g_unique_get_id;
Entry<GetStringFn> g_unique_get_class_id;
Entry<GetPropBoolFn> g_pickup_get_attached;
Entry<SetInstanceBoolFn> g_player_drop_held_item;
struct Il2CppClass* g_inventory_class = nullptr;
void* read_static_object_field(struct Il2CppClass* klass, const char* name);

// Used by the inventory guard below. g_transform_get_parent already existed for the avatar
// lookup, so only the component-level getter is new. Note it belongs to Component and not to
// GameObject: the guard walks the chain by transforming each step's GameObject first.
Entry<GetTransformFn> g_gameobject_get_transform;
Entry<DestroyFn> g_object_destroy;
// Object.DestroyImmediate, game thread only: the prop strip has to land synchronously, because
// Destroy runs at end of frame and the doomed Pickupable/PlayerTool would still fire their
// OnEnable on the activation in between.
Entry<DestroyFn> g_object_destroy_immediate;
Entry<SetStringFn> g_uid_set_id;
bool object_in_local_inventory(void* game_object);
void* fresh_pinned_id(const char* prefix, unsigned long index);
struct Il2CppClass* g_gameobject_class = nullptr;

// Walks up from a GameObject to the root, calling the visitor on each. Returns false if the
// visitor says it is done.
//
// The step from a GameObject to its parent goes through GameObject.get_transform, not
// Component.get_transform: calling the latter on a GameObject is a call on an object of the wrong
// type, which is undefined behaviour. It worked while testing on the host and took the guest down
// the first time a remote pickup arrived, which is the worst possible way to find out.
template <typename Fn>
bool walk_parents(void* game_object, Fn visit) {
    // Bounded. A parent chain is finite in a healthy world, but this walks memory that a remote
    // event has already shown can be inconsistent, and an unbounded loop there hangs the game
    // rather than crashing it.
    for (int depth = 0; game_object && depth < 256; depth++) {
        if (visit(game_object)) return false;
        if (!g_gameobject_get_transform || !g_transform_get_parent) return false;
        void* transform =
            g_gameobject_get_transform.call(game_object, g_gameobject_get_transform.method);
        if (!transform) return false;
        // Up one level: the parent transform's GameObject. Calling get_gameObject on the
        // node's own transform just returns the same node forever (which is what this loop
        // did until now - every in_inventory answer was false and every equip broadcast as
        // a drop for the peer to spawn in the world).
        void* parent =
            g_transform_get_parent.call(transform, g_transform_get_parent.method);
        if (!parent) return true;
        game_object = g_get_game_object.call(parent, g_get_game_object.method);
    }
    return true;
}
Entry<DropAtFn> g_pickup_drop_at;
Entry<SetQuaternionFn> g_set_rotation;
Entry<GetQuaternionFn> g_get_rotation;
Entry<FindObjectsFn> g_find_objects_of_type;
Entry<FindObjectsAllFn> g_find_objects_all;
Entry<GetVector3Fn> g_rb_get_velocity;
void* g_rigidbody_type = nullptr;
Entry<SetPropBoolFn> g_pickup_set_visible;

// Held-item sync. GetHeld returns the Pickupable currently in the quickslot hand, GetTechType
// turns it into the enum value the state packet carries. SetParent plus the two rigidbody
// setters are what a held-item prop needs on the receiving side: reparent onto the avatar's
// hand, stop its physics from fighting the parenting.
Entry<GetAnimObjectFn> g_inventory_get_held;
Entry<GetTechTypeFn> g_pickup_get_tech;
// Vehicle sync: Player.GetVehicle hands the piloted Seamoth/Prawn (null on foot, or in a
// Cyclops chair which this pass skips). get_isKinematic reads the rigidbody flag the remote
// drive parks and restores.
Entry<GetAnimObjectFn> g_player_get_vehicle;
Entry<GetPropBoolFn> g_rb_get_kinematic;
// Equipment sync (item sync v3): Inventory.get_equipment gives the Equipment instance,
// GetTechTypeInSlot reads one worn slot. Slot names are cached managed strings.
Entry<GetAnimObjectFn> g_inventory_get_equipment;
Entry<GetTechInSlotFn> g_equip_get_tech_in_slot;
void* g_equip_body_str = nullptr;
void* g_equip_foots_str = nullptr;
void* g_equip_head_str = nullptr;
struct Il2CppClass* g_pickupable_class = nullptr;
Entry<SetParentFn> g_set_parent;
Entry<SetPropBoolFn> g_rb_set_kinematic;
Entry<SetPropBoolFn> g_rb_set_detect_colliders;
// The PlayerTool-derived component (Seaglide, Torch, ...) is destroyed on a held-item prop so
// the copy stays a pure visual: no tool update loop, no IK targets reaching into the clone.
void* g_playertool_type_obj = nullptr;
void* g_playertool_name_str = nullptr;
using PlugVisualFn=void (*)(void* model,void* plug,void* socket,void* method);
Entry<PlugVisualFn> g_plug_visual;
using HeldTransformPointFn=Vector3 (*)(void* self,Vector3 point,void* method);
Entry<HeldTransformPointFn> g_held_transform_point;
Entry<HeldTransformPointFn> g_held_inverse_point;
Entry<SetQuaternionFn> g_held_set_local_rotation;
void* g_plug_origin_field=nullptr;
void* g_tool_socket_field=nullptr;
void* g_fpmodel_type=nullptr;
void* g_fpmodel_world_field=nullptr;
void* g_fpmodel_view_field=nullptr;
Entry<GetBoundsFn> g_held_renderer_bounds;
void* g_tool_right_ik_field=nullptr;
void* g_tool_left_ik_field=nullptr;
void* g_arms_hand_field=nullptr;
void* g_arms_attach_field=nullptr;
// Held v3 prop cleanup. Colliders anywhere in the prop subtree are switched off through the
// Behaviour setter (only the root rigidbody was neutralised before, and a MeshCollider on a
// child still shoved the clone), and the subtree is moved onto the Default layer.
void* g_collider_type_obj = nullptr;
Entry<SetLayerFn> g_go_set_layer;
// UnityEngine.Light, stripped from held props like PlayerTool: a flashlight/seaglide copy
// would otherwise carry a live spotlight, and the beam popping with every prop switch reads
// as the tool turning itself off.
void* g_light_type_obj = nullptr;
// CraftData prefab fallback: the preloaded prefab database covers tools the receiver never
// possessed (no world copy, no inventory copy). Resolved in resolve_item_api.
Entry<GetClassIdFn> g_craft_get_class_id;
Entry<TryScenePrefabFn> g_craft_try_prefab;
Entry<PrefabPreloadFn> g_craft_preload;
Entry<GetPrefabAsyncFn> g_prefab_get_async;

// Pending prefab-database requests, one per missing TechType. The load runs in the background;
// MoveNext drives it and TryGetPrefab picks the result up, both polled from the avatar update
// without log spam so the prop pops in the moment the bundle arrives. The finished prefab stays
// cached through strong runtime GC handles. A raw pointer in this native array
// does not keep the managed request, its filename or its result alive.
struct PrefabRequest {
    uint32_t tech = 0;
    ManagedRoot request;
    ManagedRoot prefab;
    Entry<TryGetPrefabFn> try_get;
    Entry<MoveNextFn> move_next;
    bool iterator_done = false;
    unsigned long last_step = 0;
};
constexpr size_t kMaxPrefabRequests = 16;
PrefabRequest g_prefab_requests[kMaxPrefabRequests];
void reset_prefab_requests();

// Set while a remote event is being applied. A counter rather than a flag because a pickup
// applied remotely can drop something else locally in the same frame, and a plain bool would be
// cleared by the inner call.
int g_applying_remote = 0;

struct Il2CppClass* g_unique_identifier_class = nullptr;
void* g_unique_identifier_type = nullptr;
void* g_pickupable_type = nullptr;

// FNV-1a over the id string. Both phones run the same id through the same function, so the two
// hashes agree without anything having to be negotiated.
void guid_from_string(const char* text, uint8_t out[16]) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; p++) {
        hash ^= *p;
        hash *= 1099511628211ull;
    }
    for (int i = 0; i < 8; i++) {
        out[i] = static_cast<uint8_t>((hash >> (i * 8)) & 0xFF);
    }
    // The second half carries the length, which keeps an id and a longer id with the same tail
    // apart - a six digit id is a prefix of plenty of longer ones.
    size_t length = strlen(text);
    uint64_t tail = 0x9E3779B97F4A7C15ull ^ length;
    for (int i = 0; i < 8; i++) {
        out[8 + i] = static_cast<uint8_t>((tail >> (i * 8)) & 0xFF);
    }
}

// Reads UniqueIdentifier.Id off a Pickupable. Returns false when the object has none, which
// happens for anything spawned without a guid and cannot be addressed by the other phones.
bool pickup_guid(void* pickupable, uint8_t out[16]) {
    if (!pickupable || !g_unique_get_id || !g_get_component || !g_unique_identifier_type) {
        return false;
    }
    void* game_object = g_get_game_object.call(pickupable, g_get_game_object.method);
    if (!game_object) return false;

    void* identifier = g_get_component.call(game_object, g_unique_identifier_type,
                                            g_get_component.method);
    if (!identifier) return false;

    void* text = g_unique_get_id.call(identifier, g_unique_get_id.method);
    char ascii[128];
    if (!string_to_ascii(text, ascii, sizeof(ascii))) return false;
    guid_from_string(ascii, out);
    return true;
}

void pickup_pose(void* pickupable, Vector3* position, Quaternion* rotation) {
    if (!pickupable || !g_get_go_transform) return;
    void* transform = g_get_go_transform.call(
        g_get_game_object.call(pickupable, g_get_game_object.method), g_get_go_transform.method);
    if (!transform) return;
    if (position) *position = g_get_position.call(transform, g_get_position.method);
    if (rotation && g_get_rotation) *rotation = g_get_rotation.call(transform, g_get_rotation.method);
}

// Looks the object up on the receiving side. Object.FindObjectsOfType walks the live scene, and
// the ids are unique per world, so one pass over the result is enough to find exactly one match.
//
// includeInactive is required, not an optimisation. A pickup applied remotely sets the object
// invisible, and the one-argument overload skips inactive objects - so the very next drop would
// not find the object it is supposed to bring back.
void* find_pickupable_by_guid(const uint8_t guid[16]) {
    if (!g_find_objects_all || !g_pickupable_type || !g_array_get_length || !g_array_get_value) {
        return nullptr;
    }
    void* list = g_find_objects_all.call(g_pickupable_type, true, g_find_objects_all.method);
    if (!list) return nullptr;

    int32_t count = g_array_get_length.call(list, g_array_get_length.method);
    for (int32_t i = 0; i < count; i++) {
        void* item = g_array_get_value.call(list, i, g_array_get_value.method);
        uint8_t candidate[16];
        if (!pickup_guid(item, candidate)) continue;
        if (memcmp(candidate, guid, 16) == 0) return item;
    }
    // Silent on purpose: the caller retries for up to kEventApplyRetrySeconds while the chunk
    // holding the object streams in, and a warn per attempt would flood the log.
    return nullptr;
}

// Defined with the rest of the item sync, below apply_remote_event, which needs it.
void suppress_item(const uint8_t guid[16]);
bool reid_local_inventory();

// net_note_role_from_peer fires on the pump thread (it is called from the packet handler),
// but reid_local_inventory walks Unity objects and must run on the game thread: tombstone_11
// on the phone without root crashed in il2cpp the moment both phones claimed host and the
// loser re-id'd its inventory from pump_main. The note is parked here and consumed by the
// next Player.Update instead, which also delays the re-id until the world has finished
// restoring: running it mid-restore crashed the same way on the game thread (tombstone_14).
bool g_role_note_pending = false;
uint32_t g_role_note_peer = 0;
bool g_session_role_pending = false;
bool g_pending_session_is_host = false;
uint32_t g_pending_session_host_id = 0;
pthread_mutex_t g_session_role_lock = PTHREAD_MUTEX_INITIALIZER;
bool g_host_capture_pending = false;

// A remote pickup only has to leave the other phones' copy of the world: the object is hidden,
// not carried, because which inventory holds it is a separate question. Blowntobytes solves it
// with a full inventory mirror; here the honest first step is that a picked-up item disappears
// for everyone else and reappears where it was dropped.
// true - the event was applied or can never be applied (permanent refusal, missing API).
// false - the named object is not in memory yet; the caller keeps the event and retries while
// its chunk streams in, which is the one failure the transport cannot fix for us: it acks on
// receipt, so delivery is guaranteed but the scene may simply not contain the object yet.
bool apply_remote_event(const WorldEvent& event) {
    if (!g_pickup_set_visible || !g_set_position) {
        LOG_WARN("event: SetVisible/set_position unavailable, remote events ignored");
        return true;
    }
    void* pickupable = find_pickupable_by_guid(event.guid);
    if (!pickupable) return false;
    void* game_object = g_get_game_object.call(pickupable, g_get_game_object.method);
    if (!game_object) return false;

    // Refuse to touch anything this phone is holding.
    //
    // Only the local player's own inventory is off limits. Both phones play the same world and can
    // be looking at the same object at the same time, so an event naming something in this
    // phone's hands is a duplicate of an action this player already took, and applying it would
    // tear the item away from them. Anything else in the world is fair game: the old guard also
    // rejected objects that were merely attached to something, which in a loaded world is most of
    // the contents of every container, and refusing those events is what left the two sides
    // disagreeing about the world.
    if (object_in_local_inventory(game_object)) {
        LOG_WARN("event: %s ignored, that object is in the local player's inventory",
                 event.kind == kWorldEventPickup ? "pickup" : "drop");
        return true;
    }

    g_applying_remote++;
    // Held for the next few sweeps. g_applying_remote only covers this call, and the change made
    // here is exactly the kind of change the sweep reports - without this the other phone's own
    // pickup comes straight back as a fresh local one.
    suppress_item(event.guid);
    if (event.kind == kWorldEventPickup) {
        // Dormant, not destroyed. The deactivated copy is the clone source for the held-item
        // prop: there is no way to load a prefab by TechType from native code (CraftData's
        // loader is a CoroutineTask that needs managed coroutine machinery), but there is always
        // the very object that just left the world. SetActive(false) is the same transition the
        // game's own Pickup makes - renderers, colliders and behaviours all go with it - so it
        // does not repeat the SetVisible(false) failure, where a renderer-only hide left the
        // object alive, swept as active and half-changed by the next event.
        //
        // The cost is that the dormant object stays in this phone's scene. That is invisible to
        // the sweep: it goes inactive here and stays inactive until a drop event brings it back,
        // so no active->inactive transition is ever reported for it. find_pickupable_by_guid
        // finding it again on the next drop is exactly what Destroy was giving up.
        if (g_set_active) {
            g_set_active.call(game_object, false, g_set_active.method);
        } else if (g_object_destroy) {
            g_object_destroy.call(game_object, g_object_destroy.method);
        } else {
            g_pickup_set_visible.call(pickupable, false, g_pickup_set_visible.method);
        }
    } else if (g_pickup_drop_at) {
        // The game's own three-argument Drop, with the position and the push velocity handed over
        // rather than taken from whoever happens to be holding it here. This is the whole reason
        // the argument list matters: the parameterless Drop() would place the object at this
        // phone's local player, and doing the placement by hand - set_position, set_rotation,
        // SetVisible - is what left dropped objects hanging in the air with their rigidbodies
        // still disabled and their behaviours never restored. Drop() runs Unplace, reparents,
        // re-enables colliders, rigidbodies and behaviours, and then Places, which is the whole
        // transition. checkPosition is false so FindDropPosition cannot walk the object off the
        // spot the other phone reported.
        Vector3 position{event.x, event.y, event.z};
        Vector3 push{event.vx, event.vy, event.vz};
        g_pickup_drop_at.call(pickupable, position, push, false, g_pickup_drop_at.method);
    } else {
        LOG_WARN("event: Pickupable.Drop(pos,push,bool) unavailable, cannot place the object");
    }
    g_applying_remote--;
    return true;
}

// ---------------------------------------------------------------------------
// Day/night clock sync, ported from the zcode session's world sync v2
//
// DayNightCycle.main is a static field; timePassedAsDouble lives at offset 0x68 of the
// instance. The host publishes its clock through net_set_local_time (the pump ships it at
// 1 Hz), and the guest pulls its own clock towards the host's. Both clocks advance at the
// same 72 game-seconds per real second, so a sample is an anchor, not a stream: the guest
// interpolates between arrivals and only corrects when the drift grows past a few seconds.
// ---------------------------------------------------------------------------

constexpr size_t kDayNightTimeOffset = 0x68;
constexpr double kGameSecondsPerSecond = 72.0;
constexpr double kTimeFreshSeconds = 5.0;
constexpr double kTimeCorrectThreshold = 5.0;  // game seconds

struct Il2CppFieldInfo* g_daynight_main_field = nullptr;
bool g_daynight_resolved = false;

// Stamped when DayNightCycle exists (the world is loading) and cleared again in the menu.
// FindObjectsOfType crashes inside Unity while the world is still restoring its containers
// (tombstone_14), so the deferred inventory re-id waits for this stamp plus
// kReidSettleSeconds before it runs.
constexpr double kReidSettleSeconds = 10.0;
double g_world_live_since = 0.0;
RuntimeLoadGuard g_runtime_load_guard;
bool validate_gc_handle_api() {
    static bool attempted=false,ready=false;
    if(attempted)return ready;
    attempted=true;
    static_assert(sizeof(Il2CppGCHandle)==sizeof(void*));
    void* marker=il2cpp_string_new("snmp-gchandle-check");
    const Il2CppGCHandle handle=marker ? il2cpp_gchandle_new(reinterpret_cast<Il2CppObject*>(marker),false) : 0;
    ready=handle&&il2cpp_gchandle_get_target(handle)==marker;
    if(handle)il2cpp_gchandle_free(handle);
    LOG_INFO("managed roots: ABI check ready=%u handle_bits=%u handle=%llx create/get/free complete",
             unsigned(ready),unsigned(sizeof(handle)*8),static_cast<unsigned long long>(handle));
    return ready;
}
bool world_operations_ready() {
    void* manager=g_slm_get_main ? g_slm_get_main.call(nullptr,g_slm_get_main.method) : nullptr;
    const bool available=manager&&g_slm_is_loading;
    // SaveLoadManager can report false while uGUI_SceneLoading is still active.
    const bool loading=!available||g_slm_is_loading.call(manager,g_slm_is_loading.method)||
        loading_screen_blocks_world()||!validate_gc_handle_api();
    const bool before=g_runtime_load_guard.ready;
    const bool ready=g_runtime_load_guard.update(g_world_live_since>0,available,loading,
        g_world_context_generation,monotonic_seconds());
    if(before!=ready)LOG_INFO("runtime world: %s context=%u loading=%u",ready?"ready":"deferred",g_world_context_generation,unsigned(loading));
    return ready;
}

// Resolved lazily on the first Player.Update: il2cpp_class_from_name only sees types the
// runtime has materialised, and DayNightCycle is not touched until a world is loading, so
// asking at hook-install time (main menu) would report the class missing forever.
void resolve_daynight() {
    if (g_daynight_resolved) return;
    g_daynight_resolved = true;
    struct Il2CppClass* daynight = find_class_in_domain("", "DayNightCycle");
    if (!daynight || !il2cpp_class_get_fields || !il2cpp_field_get_name) {
        LOG_WARN("time sync: DayNightCycle unavailable, clock sync disabled");
        return;
    }
    void* iterator = nullptr;
    for (struct Il2CppFieldInfo* field = il2cpp_class_get_fields(daynight, &iterator); field;
         field = il2cpp_class_get_fields(daynight, &iterator)) {
        const char* name = il2cpp_field_get_name(field);
        if (name && strcmp(name, "main") == 0) {
            g_daynight_main_field = field;
            break;
        }
    }
    LOG_INFO("time sync: DayNightCycle.main %s", g_daynight_main_field ? "resolved" : "NOT found");
}

void* daynight_instance() {
    if (!g_daynight_main_field || !il2cpp_field_static_get_value) return nullptr;
    void* instance = nullptr;
    il2cpp_field_static_get_value(g_daynight_main_field, &instance);
    return instance;
}

double read_day_time(void* instance) {
    return *reinterpret_cast<double*>(reinterpret_cast<char*>(instance) + kDayNightTimeOffset);
}

void write_day_time(void* instance, double value) {
    *reinterpret_cast<double*>(reinterpret_cast<char*>(instance) + kDayNightTimeOffset) = value;
}

double monotonic_seconds() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + ts.tv_nsec * 1e-9;
}

// Called on the game thread from Player.Update. Only the host publishes and only the guest
// corrects, so neither phone can ever fight over the other's clock.
//
// The host also measures how fast its own clock actually advances and ships that rate with
// every packet. Assuming 72 game-seconds per real second was wrong: the guest's extrapolated
// target then drifted ~50 game-seconds per second away from reality, so the guest corrected
// ~10 times a second - the sun visibly sprinted forward and snapping back at every packet,
// which is what the "sun runs fast then returns home" flicker is.
void time_sync_tick() {
    resolve_daynight();
    void* instance = daynight_instance();
    bool in_game = instance != nullptr;
    if (in_game) {
        if (g_world_live_since == 0.0) {
            net_session_set_world_active(true);
            g_world_live_since = monotonic_seconds();
            LOG_INFO("re-id: world is live, inventory re-id settles for %.0fs",
                     kReidSettleSeconds);
        }
    } else {
        if (g_world_live_since > 0.0) {
            reset_prefab_requests();
            net_session_set_world_active(false);
            bool was_host = g_local_is_host;
            if(was_host) {
                // Player.Update can see the unload before SaveLoadManager.Update does.
                // Retire membership as well as authority, otherwise the next local
                // world is mistaken for a conflicting guest world.
                if(net_session_invalidate_host_world()) {
                    world_release();++g_world_context_generation;
                }
            } else {
                net_session_set_local_world(false, nullptr, nullptr);
            }
            g_local_is_host = net_session_is_host();
            if (was_host) { net_offer_world(nullptr, nullptr);g_role_known=false; }
        }
        g_world_live_since = 0.0;
    }
    double now = monotonic_seconds();
    if (g_local_is_host) {
        // Rate estimate: a pair of samples at least a second apart. A sample that does not
        // look like a steady advance (a load hitch, the one-off entry time smash) is
        // discarded rather than poisoning the average.
        static double rate = 0.0;
        static double last_sample_t = 0.0;
        static double last_sample_v = 0.0;
        if (in_game) {
            double value = read_day_time(instance);
            if (last_sample_t == 0.0) {
                last_sample_t = now;
                last_sample_v = value;
            } else if (now - last_sample_t >= 1.0) {
                double inst = (value - last_sample_v) / (now - last_sample_t);
                // The band keeps the smoothed rate from chasing a step: the `night`/`day`
                // console command ramps the host clock by thousands of game-seconds over
                // ~15s, and letting that into the EMA made the guest race and snap back
                // again until the average decayed. A step only re-baselines the sampler;
                // the guest follows the jump through host_value in the packets anyway.
                bool plausible = rate <= 0.0 ? (inst > 0.1 && inst < 500.0)
                                             : (inst > rate * 0.5 && inst < rate * 2.0);
                if (plausible) {
                    rate = (rate <= 0.0) ? inst : rate + 0.3 * (inst - rate);
                } else {
                    LOG_INFO("time rate: step %.1f outside [%.2f, %.2f], keeping %.2f",
                             inst, rate * 0.5, rate * 2.0, rate);
                }
                last_sample_t = now;
                last_sample_v = value;
            }
        } else {
            last_sample_t = 0.0;
        }
        net_set_local_time(in_game ? read_day_time(instance) : 0.0, in_game, rate);
        static double last_host_diag = 0.0;
        if (in_game && rate > 0.0 && now - last_host_diag >= 10.0) {
            last_host_diag = now;
            LOG_INFO("time diag: host local=%.1f rate=%.2f", read_day_time(instance), rate);
        }
        return;
    }
    net_set_local_time(0.0, in_game, 0.0);
    if (!in_game) return;

    double host_value = 0.0;
    double host_at = 0.0;
    double host_rate = 0.0;
    if (!net_host_time(&host_value, &host_at, &host_rate)) return;
    if (now - host_at >= kTimeFreshSeconds) return;
    // No corrections until the host has shipped a measured rate; guessing again is what
    // caused the flicker in the first place.
    if (host_rate <= 0.0) return;

    double expected = host_value + (now - host_at) * host_rate;
    double local = read_day_time(instance);

    static unsigned long corrections = 0;
    static double last_diag = 0.0;
    if (now - last_diag >= 10.0) {
        last_diag = now;
        LOG_INFO("time diag: guest local=%.1f expected=%.1f host=%.1f age=%.2f rate=%.2f corr=%lu",
                 local, expected, host_value, now - host_at, host_rate, corrections);
    }

    if (local < 0.0 || fabs(expected - local) <= kTimeCorrectThreshold) return;

    write_day_time(instance, expected);
    static double last_log = 0.0;
    corrections++;
    if (now - last_log > 10.0) {
        last_log = now;
        LOG_INFO("time sync: corrected by %.1f game-seconds (total=%lu)", expected - local,
                 corrections);
    }
}

// Events whose object is not in the scene yet: the chunk it lives in has not streamed in.
// The transport acks on receipt and guarantees delivery, so all that is left is to keep
// trying until the object appears or the deadline passes.
constexpr double kEventApplyRetrySeconds = 30.0;
constexpr double kEventRetryInterval = 0.5;

struct PendingEvent {
    WorldEvent ev;
    bool used;
    double deadline;
    double next_try;
};

PendingEvent g_pending_events[16];

// Build sync v1 (containment port from experimental/): outdoor Constructable snapshots
// only. Defines tick_build_sync/queue_remote_build and their tables; wired below from
// pump_remote_events and hooked_player_update. Not full base replication - ConstructableBase
// is refused before it can be instantiated (stage 3 of the live-world plan adds anchors).
#if defined(SNMOD_BASE_GEOMETRY_APPLY)
bool contents_locker_busy(const char* id);
#endif
#include "build_sync.inc"
#if defined(SNMOD_BASE_GEOMETRY_APPLY)
#include "locker_contents_sync.inc"
#include "creature_probe.inc"
#include "creature_motion_probe.inc"
#include "player_marker.inc"
#endif
#include "avatar_environment.inc"

// Runs on the game thread from Player.Update. The pump only queues; everything that touches
// Pickupable has to happen here.
void pump_remote_events() {
    WorldEvent events[8];
    size_t count = net_copy_world_events(events, 8);
    double now = monotonic_seconds();

    for (size_t i = 0; i < count; i++) {
#if defined(SNMOD_BASE_GEOMETRY_APPLY)
        if(events[i].kind==kWorldEventLockerContents){contents_receive_frame(events[i]);continue;}
#endif
        // Build kinds carry a full snapshot; the job table on the game thread owns the
        // apply, so they are queued straight away instead of going through pickup/drop
        // apply-retry. A job that cannot run yet (no prefab cache) is the draft's own
        // concern; the sender's 5 s repair snapshot covers a refused queue slot.
        // Remove acts immediately: events arrive in order per origin, so a remove that
        // overtakes its own build job cancels it rather than spawning the piece back.
        if (events[i].kind == kWorldEventBuild || events[i].kind == kWorldEventProgress) {
            queue_remote_build(events[i]);
            continue;
        }
        if (events[i].kind == kWorldEventRemove) {
            build_handle_remove(events[i]);
            continue;
        }
        if (apply_remote_event(events[i])) continue;
        bool queued = false;
        for (const PendingEvent& slot : g_pending_events) {
            if (slot.used && slot.ev.kind == events[i].kind &&
                memcmp(slot.ev.guid, events[i].guid, 16) == 0) {
                queued = true;
                break;
            }
        }
        if (queued) continue;
        for (PendingEvent& slot : g_pending_events) {
            if (slot.used) continue;
            slot.used = true;
            slot.ev = events[i];
            slot.deadline = now + kEventApplyRetrySeconds;
            slot.next_try = now + kEventRetryInterval;
            LOG_INFO("event: %s %u not in the scene yet, retrying for %.0f s",
                     events[i].kind == kWorldEventPickup ? "pickup" : "drop",
                     events[i].sequence, kEventApplyRetrySeconds);
            queued = true;
            break;
        }
        if (!queued) {
            LOG_WARN("event: retry queue full, %s %u dropped",
                     events[i].kind == kWorldEventPickup ? "pickup" : "drop",
                     events[i].sequence);
        }
    }

    for (PendingEvent& slot : g_pending_events) {
        if (!slot.used) continue;
        if (now > slot.deadline) {
            LOG_WARN("event: %s %u never appeared, dropped",
                     slot.ev.kind == kWorldEventPickup ? "pickup" : "drop", slot.ev.sequence);
            slot.used = false;
            continue;
        }
        if (now < slot.next_try) continue;
        slot.next_try = now + kEventRetryInterval;
        if (apply_remote_event(slot.ev)) {
            LOG_INFO("event: %s %u applied after retry",
                     slot.ev.kind == kWorldEventPickup ? "pickup" : "drop", slot.ev.sequence);
            slot.used = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Item sync by observation
//
// Pickupable.Pickup and Pickupable.Drop could not be hooked. They are not virtual, so the AOT
// code resolved them at compile time and calls the address directly - MethodInfo::methodPointer
// is never read and the patch reported ok=1 while the hook was unreachable. shadowhook, which
// redirects the prologue instead, cannot initialise on this Android version either
// (rc=12, SHADOWHOOK_ERRNO_INIT_LINKER: the linker no longer exports soinfo::call_constructors).
//
// So the change in the world is detected by watching it instead. Player.Update is already
// hooked and does run, so from there the scene is swept for Pickupables and compared with the
// previous sweep. Pickup calls Deactivate and Drop activates, so the active flag is the signal:
//
//   pickup - was active, now inactive
//   drop   - was inactive, now active, now at its new position
//
// Three things this must not get wrong, all of them learned from the first device run:
//
//   The sweep has to include inactive objects. With the one-argument FindObjectsOfType a hidden
//   object is simply absent, and "absent" cannot be told apart from "picked up". That turned
//   every remote pickup into a new local pickup: the object disappeared on the next sweep, the
//   disappearance was reported back, and the two phones echoed each other (rx climbed past 1800
//   while the log filled with consecutive pickup numbers).
//
//   An event applied locally has to be remembered. g_applying_remote covers the frame it is
//   applied on, not the sweep a moment later that observes the result, so the guid is held in a
//   suppression list and skipped while it is there.
//
//   Only the local player's own business is reported. Objects also leave and re-enter the
//   results for reasons that have nothing to do with a player - despawning, being eaten, scene
//   streaming - and each of those was becoming an event. A change is kept only if the object is
//   within arm's reach of the local player.
// ---------------------------------------------------------------------------

constexpr size_t kItemScanLimit = 512;
constexpr int kItemScanTick = 30;        // roughly once a second at 30 Hz
constexpr int kItemSuppressedFrames = 90;  // three sweeps, long enough to cover the ones after
constexpr float kItemReachLimit = 12.0f;   // metres; a pickup always happens within arm's reach

// One guid as last seen. Deliberately not the managed pointer: the pointers differ between two
// runs of the same world, and a pointer reused by a later allocation would look like the same
// object surviving.
struct ItemMark {
    uint8_t guid[16];
    float x;
    float y;
    float z;
    Quaternion rotation;
    Vector3 velocity;
    bool active;
    // True when the object sits anywhere under Inventory.main at the time of the sweep. Only
    // meaningful for active marks: an active item inside the inventory is the equipped viewmodel
    // riding the camera, an active item outside it is a real object in the world. This is what
    // tells the real transitions apart from the phantom ones - equipping a tool makes it go
    // inactive->active (looks like a drop) and putting it away makes it go active->inactive
    // (looks like a pickup), and neither of those is anybody else's business.
    bool in_inventory;
    // The live GameObject behind this mark. Valid only for the sweep that produced it; the
    // copies parked in g_items_seen may outlive the object and are never dereferenced there.
    void* live_go;
};

ItemMark g_items_seen[kItemScanLimit];
size_t g_items_seen_count = 0;
int g_item_scan_divider = 0;

// Measured in sweeps, not in frames: the outer divider already gates this to every 30th frame, so
// ten here is ten sweeps, about one every ten seconds. Written as 300 the first time round, which
// was thirty sweeps times thirty frames - over four minutes, so the count never appeared at all.
constexpr int kItemMeasureTick = 10;
int g_item_measure_divider = 0;
bool g_item_scan_ready = false;
unsigned long g_item_frame = 0;
Vector3 g_item_origin{};

// guids changed here by a remote event, held for a few sweeps so the change is not reported
// straight back out. Keyed on a frame counter of its own: the sweep divider wraps every
// kItemScanTick frames and could not be compared against an expiry.
struct ItemSuppress {
    uint8_t guid[16];
    unsigned long until;
};
ItemSuppress g_item_suppressed[32];
int g_item_suppress_next = 0;

void suppress_item(const uint8_t guid[16]) {
    ItemSuppress& slot = g_item_suppressed[g_item_suppress_next];
    g_item_suppress_next = (g_item_suppress_next + 1) %
                           (sizeof(g_item_suppressed) / sizeof(ItemSuppress));
    memcpy(slot.guid, guid, 16);
    slot.until = g_item_frame + kItemSuppressedFrames;
}

bool item_is_suppressed(const uint8_t guid[16]) {
    for (size_t i = 0; i < sizeof(g_item_suppressed) / sizeof(ItemSuppress); i++) {
        if (g_item_suppressed[i].until <= g_item_frame) continue;
        if (memcmp(g_item_suppressed[i].guid, guid, 16) == 0) return true;
    }
    return false;
}

bool mark_matches(const ItemMark& mark, const uint8_t guid[16]) {
    return memcmp(mark.guid, guid, 16) == 0;
}

bool within_reach(const ItemMark& mark) {
    float dx = mark.x - g_item_origin.x;
    float dy = mark.y - g_item_origin.y;
    float dz = mark.z - g_item_origin.z;
    return (dx * dx + dy * dy + dz * dz) <= (kItemReachLimit * kItemReachLimit);
}

// Reads the guid, the world pose, the throw velocity and the active flag of one Pickupable.
// Returns false for anything without a UniqueIdentifier, which cannot be addressed by the other
// phone anyway.
bool read_item_mark(void* pickupable, ItemMark* out) {
    if (!pickup_guid(pickupable, out->guid)) return false;
    if (!g_get_game_object || !g_go_get_active_self) return false;
    void* game_object = g_get_game_object.call(pickupable, g_get_game_object.method);
    if (!game_object) return false;
    out->live_go = game_object;
    out->active = g_go_get_active_self.call(game_object, g_go_get_active_self.method);
    // The parent walk only runs for active objects, which are few: inactive items sit in
    // backpacks and lockers by the dozen, and their flag is never read by the transition logic.
    out->in_inventory = out->active && object_in_local_inventory(game_object);

    Vector3 position{};
    pickup_pose(pickupable, &position, &out->rotation);
    out->x = position.x;
    out->y = position.y;
    out->z = position.z;

    // The velocity is what a thrown object carries. Left at zero the receiving phone would place
    // it and let go, and anything thrown would drop from where it was released rather than
    // travelling. Absent a rigidbody - plants and most scenery have none - zero is the right
    // answer anyway.
    out->velocity = Vector3{};
    if (g_rigidbody_type && g_get_component) {
        void* body = g_get_component.call(game_object, g_rigidbody_type, g_get_component.method);
        if (body && g_rb_get_velocity) {
            out->velocity = g_rb_get_velocity.call(body, g_rb_get_velocity.method);
        }
    }
    return true;
}

// Periodic breadcrumb for the item scan, at most every kItemLogInterval seconds. It used to also
// count get_attached() flags and warn when more than one looked attached, but get_attached() is
// true for everything inside a storage container, so on any real save the count was 17-23 and the
// warning fired every sweep. The scan's correctness rests on the parent-chain check
// (object_in_local_inventory), not on that flag, so the flag is not read at all any more.
constexpr double kItemLogInterval = 30.0;
void log_item_holding() {
    static double last_log = 0.0;
    double now = monotonic_seconds();
    if (now - last_log < kItemLogInterval) return;
    last_log = now;

    int suppressed = 0;
    for (size_t i = 0; i < sizeof(g_item_suppressed) / sizeof(ItemSuppress); i++) {
        if (g_item_suppressed[i].until > g_item_frame) suppressed++;
    }
    LOG_INFO("item scan: %zu objects tracked, %d suppressed, tx=%llu rx=%llu",
             g_items_seen_count, suppressed, net_world_event_tx(), net_world_event_rx());
}

// Gives every object in the local player's own inventory and equipment a brand new id.
//
// A joiner loads the host's save, so its starting inventory is a copy of the host's, down to the
// ids inside it. Two phones then hold different objects carrying the same id, and an event about
// "that torch" is ambiguous: the receiving phone finds the copy in its own hands and refuses it as
// a duplicate, or hides the wrong object. Re-iding the local inventory once at join removes the
// ambiguity at the source. The host must not do this - its ids are the ones the world is addressed
// by, and changing them would orphan every saved reference to them.
// True when the sweep ran to completion (whether or not anything was renamed); false means
// the world or the API is not ready and the caller may try again later.
bool reid_local_inventory() {
    // FindObjectsOfType takes a managed Type object, not an Il2CppClass*: passing the raw
    // class made the wrapper read a field of a fake object and crash at null+0x135 (the
    // tombstones from 00:08). g_unique_identifier_type is that Type object; g_pickupable_type
    // was already correct, which is why the item scan never crashed.
    if (g_local_is_host || !g_uid_set_id || !g_find_objects_all || !g_unique_identifier_type ||
        !g_gameobject_get_transform || !g_inventory_class) {
        return false;
    }
    void* inventory = read_static_object_field(g_inventory_class, "main");
    if (!inventory) return false;
    void* inventory_object = g_get_game_object.call(inventory, g_get_game_object.method);
    if (!inventory_object) return false;

    void* list = g_find_objects_all.call(g_unique_identifier_type, true,
                                         g_find_objects_all.method);
    if (!list) return false;
    int32_t count = g_array_get_length.call(list, g_array_get_length.method);

    unsigned long renamed = 0;
    for (int32_t i = 0; i < count; i++) {
        void* uid = g_array_get_value.call(list, i, g_array_get_value.method);
        void* game_object =
            uid ? g_get_game_object.call(uid, g_get_game_object.method) : nullptr;
        if (!game_object) continue;

        bool inside = false;
        walk_parents(game_object, [&](void* node) {
            if (node != inventory_object) return false;
            inside = true;
            return true;
        });
        if (!inside) continue;

        char prefix[64];
        snprintf(prefix, sizeof(prefix), "snmod-inv-%p", (void*)game_object);
        void* fresh = fresh_pinned_id(prefix, ++renamed);
        if (!fresh) break;
        g_uid_set_id.call(uid, fresh, g_uid_set_id.method);
    }
    if (renamed) {
        LOG_INFO("role: guest gave %lu inventory items fresh ids, so its copy of the host's "
                 "inventory no longer collides with the host's",
                 renamed);
    }
    return true;
}

// Reads a static object field by name. The game reaches its one live inventory through
// Inventory.main, and DropHeldItem belongs to Inventory rather than to Player.
void* read_static_object_field(struct Il2CppClass* klass, const char* name) {
    if (!klass || !il2cpp_class_get_fields || !il2cpp_field_get_name ||
        !il2cpp_field_static_get_value) {
        return nullptr;
    }
    void* iterator = nullptr;
    for (void* field = il2cpp_class_get_fields(klass, &iterator); field;
         field = il2cpp_class_get_fields(klass, &iterator)) {
        const char* field_name = il2cpp_field_get_name(field);
        if (!field_name || strcmp(field_name, name) != 0) continue;
        void* value = nullptr;
        il2cpp_field_static_get_value(field, &value);
        return value;
    }
    return nullptr;
}

// True when the object sits anywhere under the local player's own inventory or equipment.
//
// This replaces a get_attached() guard that was measuring the wrong thing entirely. Measured on a
// freshly loaded world, 17 to 23 objects reported attached=true at any moment - everything in a
// storage container is attached - so the old guard refused to apply most incoming events and the
// refusal left those objects half-changed. Whether an object is in a player's hands is a question
// about its parent chain, not a flag on the object.
bool object_in_local_inventory(void* game_object) {
    if (!game_object || !g_inventory_class || !g_gameobject_get_transform) return false;
    void* inventory = read_static_object_field(g_inventory_class, "main");
    if (!inventory) return false;

    // Inventory is itself a Component, so its GameObject comes from the same getter.
    void* inventory_object =
        g_get_game_object.call(inventory, g_get_game_object.method);
    if (!inventory_object) return false;

    bool found = false;
    walk_parents(game_object, [&](void* node) {
        if (node != inventory_object) return false;
        found = true;
        return true;
    });
    return found;
}

// One-shot diagnostic for the equip-filter question: is an equipped viewmodel actually under
// Inventory.main's GameObject? Logs that object's own name plus its chain to the root. If the
// answer is "Inventory Storage" or some other side object instead of the Player root, every
// equip broadcasts as a real drop and the peer spawns a copy in the world.
void log_inventory_topology_once() {
    static bool done = false;
    if (done) return;
    if (!g_inventory_class || !g_get_game_object || !g_get_name) return;
    void* inventory = read_static_object_field(g_inventory_class, "main");
    if (!inventory) return;
    void* inventory_object =
        g_get_game_object.call(inventory, g_get_game_object.method);
    if (!inventory_object) return;
    done = true;
    char name[64] = {0};
    read_managed_name(g_get_name.call(inventory_object, g_get_name.method), name,
                      sizeof(name));
    LOG_INFO("inventory topology: Inventory.main lives on GameObject '%s' (%p)", name,
             inventory_object);
    int depth = 0;
    walk_parents(inventory_object, [&](void* node) {
        char n[64] = {0};
        if (g_get_name) {
            read_managed_name(g_get_name.call(node, g_get_name.method), n, sizeof(n));
        }
        LOG_INFO("inventory topology: ancestor[%d] = '%s' (%p)", depth, n, node);
        return ++depth >= 8;
    });
}

// A fresh unique id string, pinned so the collector cannot take it while the setter reads it.
// The pinned handle is never released: re-id runs once per session over a bounded number of
// items, and a leaked handle is cheaper than a stale pointer.
void* fresh_pinned_id(const char* prefix, unsigned long index) {
    if (!il2cpp_string_new) return nullptr;
    char text[128];
    snprintf(text, sizeof(text), "%s-%lu", prefix, index);
    void* value = il2cpp_string_new(text);
    if (value && il2cpp_gchandle_new) {
        il2cpp_gchandle_new(reinterpret_cast<Il2CppObject*>(value), true);
    }
    return value;
}

void send_item_event(const ItemMark& mark, uint8_t kind) {
    WorldEvent event{};
    event.kind = kind;
    memcpy(event.guid, mark.guid, sizeof(event.guid));
    event.x = mark.x;
    event.y = mark.y;
    event.z = mark.z;
    event.qx = mark.rotation.x;
    event.qy = mark.rotation.y;
    event.qz = mark.rotation.z;
    event.qw = mark.rotation.w;
    event.vx = mark.velocity.x;
    event.vy = mark.velocity.y;
    event.vz = mark.velocity.z;
    // already_applied is true: this phone's own game just made the change, so it must not act
    // on the event when it comes back from a peer.
    net_post_world_event(event, true);
}

// One sweep. An object that went from active to inactive was picked up; one that went the other
// way was dropped, and the position it came back at is where it landed.
//
// With emit false the sweep only records what it sees. That mode exists because the count of
// objects the player is holding has to be readable with nobody connected, and that is exactly
// when a handful of items end up welded to the camera with no peer around to have caused it.
void scan_items(bool emit) {
    if (!g_find_objects_all || !g_pickupable_type || !g_array_get_length ||
        !g_array_get_value) {
        return;
    }
    void* list = g_find_objects_all.call(g_pickupable_type, true, g_find_objects_all.method);
    if (!list) return;

    ItemMark now[kItemScanLimit];
    size_t now_count = 0;
    int32_t count = g_array_get_length.call(list, g_array_get_length.method);
    if (count > static_cast<int32_t>(kItemScanLimit)) count = static_cast<int32_t>(kItemScanLimit);

    for (int32_t i = 0; i < count; i++) {
        void* item = g_array_get_value.call(list, i, g_array_get_value.method);
        ItemMark mark{};
        if (!read_item_mark(item, &mark)) continue;
        now[now_count++] = mark;
    }

    if (emit && g_item_scan_ready) {
        for (size_t i = 0; i < now_count; i++) {
            if (!now[i].active) continue;
            // Was it there before, and was it inactive then? Then this is a drop.
            bool known_inactive = false;
            for (size_t j = 0; j < g_items_seen_count; j++) {
                if (!mark_matches(g_items_seen[j], now[i].guid)) continue;
                known_inactive = !g_items_seen[j].active;
                break;
            }
            if (!known_inactive) continue;
            // Equipping a quickslot item also flips it from inactive to active - the viewmodel
            // comes alive at the camera - and without this guard every equip was broadcast as a
            // drop: the other phone spawned a copy of the item in the world wherever the holder
            // was standing. A drop is only a drop when the object ended up outside the inventory.
            if (now[i].in_inventory) continue;
            if (!within_reach(now[i]) || item_is_suppressed(now[i].guid)) continue;
            LOG_INFO("event: local drop detected");
            send_item_event(now[i], kWorldEventDrop);
        }
        for (size_t j = 0; j < g_items_seen_count; j++) {
            if (!g_items_seen[j].active) continue;
            // Still around and still active? Then nothing happened to it.
            bool still_active = false;
            for (size_t i = 0; i < now_count; i++) {
                if (mark_matches(now[i], g_items_seen[j].guid) && now[i].active) {
                    still_active = true;
                    break;
                }
            }
            if (still_active) continue;
            // The mirror image of the equip guard above. Putting a held item back into the
            // quickslots makes it go inactive, which reads as a pickup; but the item was riding
            // the camera when it was last seen, so it never left the holder's hands. A real
            // pickup was last seen active out in the world.
            if (g_items_seen[j].in_inventory) continue;
            if (!within_reach(g_items_seen[j]) || item_is_suppressed(g_items_seen[j].guid)) {
                continue;
            }
            LOG_INFO("event: local pickup detected");
            send_item_event(g_items_seen[j], kWorldEventPickup);
        }
    }

    memcpy(g_items_seen, now, sizeof(ItemMark) * now_count);
    g_items_seen_count = now_count;
    g_item_scan_ready = true;
}

// Called from hooked_player_update. The first sweep only records the baseline; reporting it
// would send the whole world to the peers as a stream of drops.
void tick_item_scan(const LocalState& local) {
    log_inventory_topology_once();
    if (g_applying_remote != 0) return;
    g_item_origin.x = local.x;
    g_item_origin.y = local.y;
    g_item_origin.z = local.z;
    if (local.x == 0.0f && local.y == 0.0f && local.z == 0.0f) return;

    g_item_frame++;
    if (++g_item_scan_divider < kItemScanTick) return;
    g_item_scan_divider = 0;

    if (net_peer_count() == 0) {
        // Nobody to tell, so nothing is emitted. The baseline is dropped so that the next sweep
        // after a peer appears does not report every object as newly dropped.
        g_item_scan_ready = false;

        // The holding count is still measured, on a much slower clock. Finding every Pickupable
        // costs real time - doing it several times a second was what made the frame rate sag - so
        // with no peer there is no reason to do it at the event rate.
        if (++g_item_measure_divider < kItemMeasureTick) return;
        g_item_measure_divider = 0;
        scan_items(false);
        log_item_holding();
        return;
    }

    scan_items(true);
    log_item_holding();
}

// ---------------------------------------------------------------------------
// Held item sync
//
// The state packet carries the TechType of whatever the sender is holding. The receiver turns
// that number into a prop on the peer's avatar: a visual copy of an item of the same type that
// exists in this phone's world, stripped down to renderers and transforms, parented to the
// clone's hand. Both players then see the same tool in the same hands.
//
// The reason for the copy-from-the-scene approach: loading a prefab by TechType from native code
// is not possible here. CraftData.GetPrefabForTechTypeAsync is a CoroutineTask that needs managed
// coroutine machinery to pump, and il2cpp_runtime_invoke takes this build down. But the world
// always contains items, and a picked-up item is guaranteed to have just left one behind as a
// deactivated copy (the remote pickup apply keeps it dormant on purpose), so the common case -
// "the item I picked up I now hold" - always has a source on the other phone.
//
// The strip-down matters. A live copy with its Pickupable would carry the source's guid into a
// second object, and the next pickup or drop event naming that guid could then be applied to the
// prop instead of the real item. The PlayerTool-derived component goes too so the copy has no
// update loop and no IK targets; the rigidbody is made kinematic so physics does not fight the
// parenting. What remains is a doll that follows the hand.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Vehicle sync (game thread side)
//
// Whoever pilots publishes guid + transform; the receiver teleports its local copy of the
// same vehicle and parks it kinematic while driven. The pilot's avatar hides (it sits
// inside). Table and caches below are game-thread local; the pump hands fresh copies over.
// ---------------------------------------------------------------------------

// Temporary Luna diagnostics, round 4: SkinnedMeshRenderer bones on prefab vs clone.
// If the copy's bones point at the prefab asset (or null) instead of the copy's own
// skeleton, the mesh collapses into an unlit blob: black silhouette that vanishes
// when the camera enters it (backfaces culled) - exactly the reported symptom.
// The applier config is already proven identical (baseline env=0x0 on the working sub),
// and materials are shared pointers, so bones are the remaining structural suspect.
void log_vehicle_bones_audit(void* prefab, void* vehicle_go) {
    if (!g_smr_type_obj || g_get_component_is_string || !g_get_component ||
        !g_smr_get_root_bone || !g_smr_get_bones || !g_get_go_transform || !g_get_child ||
        !g_get_child_count || !g_get_game_object || !g_get_name || !g_array_get_length ||
        !g_array_get_value) {
        LOG_INFO("veh bones audit: prerequisites missing");
        return;
    }
    for (int side = 0; side < 2; side++) {
        void* root = side ? vehicle_go : prefab;
        if (!root) continue;
        char detail[512] = {0};
        size_t pos = 0;
        int skinned = 0;
        void* stack[256];
        size_t top = 0;
        stack[top++] = root;
        size_t visited = 0;
        while (top > 0 && visited < sizeof(stack) / sizeof(stack[0]) && skinned < 4) {
            void* node = stack[--top];
            visited++;
            void* smr = g_get_component.call(node, g_smr_type_obj, g_get_component.method);
            if (smr) {
                skinned++;
                void* bones =
                    g_smr_get_bones.call(smr, g_smr_get_bones.method);
                int32_t nbones = bones ? g_array_get_length.call(bones,
                                                                 g_array_get_length.method)
                                       : -1;
                void* root_bone =
                    g_smr_get_root_bone.call(smr, g_smr_get_root_bone.method);
                void* bone0 = (bones && nbones > 0)
                                  ? g_array_get_value.call(bones, 0, g_array_get_value.method)
                                  : nullptr;
                char nname[40] = {0};
                void* ngo = g_get_game_object.call(smr, g_get_game_object.method);
                if (ngo) {
                    void* text = g_get_name.call(ngo, g_get_name.method);
                    if (!read_managed_name(text, nname, sizeof(nname))) {
                        snprintf(nname, sizeof(nname), "%s", "?");
                    }
                }
                pos += (size_t)snprintf(detail + pos,
                                        sizeof(detail) > pos ? sizeof(detail) - pos : 0,
                                        "%s%s:bones=%d,root=%s,b0=%s", pos ? " " : "", nname,
                                        nbones, root_bone ? "ok" : "NULL",
                                        bone0 ? "ok" : "NULL");
            }
            void* tf = g_get_go_transform.call(node, g_get_go_transform.method);
            if (!tf) continue;
            int32_t kids = g_get_child_count.call(tf, g_get_child_count.method);
            for (int32_t i = 0; i < kids && top < (int)(sizeof(stack) / sizeof(stack[0]));
                 i++) {
                void* ctf = g_get_child.call(tf, i, g_get_child.method);
                if (!ctf) continue;
                void* cgo = g_get_game_object.call(ctf, g_get_game_object.method);
                if (cgo) stack[top++] = cgo;
            }
        }
        LOG_INFO("veh bones audit: side=%s skinned=%d [%s]", side ? "clone" : "prefab",
                 skinned, detail);
    }
}

// Temporary Luna diagnostics, round 4: Renderer.get_enabled is Renderer's OWN property
// (not Behaviour's) - a disabled renderer is invisible, so a visible-but-black copy
// must have them on; verifying instead of assuming. GameObject.get_layer likewise.
using GetLayerFn = int32_t (*)(void* self, void* method);
Entry<GetPropBoolFn> g_rend_get_enabled;
Entry<GetLayerFn> g_go_get_layer;

// Temporary Luna diagnostics, round 4: structural state of the copy that no previous
// audit covered - its layer (we force 0; the real sub's layer is in the baseline)
// and how many of its own renderers are disabled (disabled = invisible, so a visible
// black copy must have them on; verifying instead of assuming).
void log_vehicle_visibility_audit(void* vehicle_go) {
    if (!vehicle_go) return;
    int layer = -1;
    if (g_go_get_layer) {
        layer = g_go_get_layer.call(vehicle_go, g_go_get_layer.method);
    }
    int renderers = 0;
    int disabled = 0;
    if (g_rend_get_enabled && g_get_go_transform && g_get_child && g_get_child_count &&
        g_get_game_object && g_get_component && g_renderer_type_obj) {
        void* stack[256];
        size_t top = 0;
        stack[top++] = vehicle_go;
        size_t visited = 0;
        while (top > 0 && visited < sizeof(stack) / sizeof(stack[0])) {
            void* node = stack[--top];
            visited++;
            void* renderer = g_get_component.call(node, g_renderer_type_obj,
                                                  g_get_component.method);
            if (renderer) {
                renderers++;
                if (!g_rend_get_enabled.call(renderer, g_rend_get_enabled.method)) {
                    disabled++;
                }
            }
            void* tf = g_get_go_transform.call(node, g_get_go_transform.method);
            if (!tf) continue;
            int32_t kids = g_get_child_count.call(tf, g_get_child_count.method);
            for (int32_t i = 0; i < kids && top < (int)(sizeof(stack) / sizeof(stack[0]));
                 i++) {
                void* ctf = g_get_child.call(tf, i, g_get_child.method);
                if (!ctf) continue;
                void* cgo = g_get_game_object.call(ctf, g_get_game_object.method);
                if (cgo) stack[top++] = cgo;
            }
        }
    }
    LOG_INFO("veh visibility audit: layer=%d renderers=%d disabledRenderers=%d", layer,
             renderers, disabled);
}

// Temporary Luna diagnostics, round 3: which Sky objects exist in the scene and how far
// each is from the copy. If the copy's Broadcast resolves nothing while a biome Sky sits
// next to it, the environment lookup (not the array) is the broken link.
void log_vehicle_sky_candidates(void* copy_go) {
    static void* s_sky_type = nullptr;
    static bool s_tried = false;
    if (!s_tried) {
        s_tried = true;
        struct Il2CppClass* klass = find_class_in_domain("mset", "Sky");
        if (klass && il2cpp_class_get_type && il2cpp_type_get_object) {
            s_sky_type = il2cpp_type_get_object(il2cpp_class_get_type(klass));
        }
    }
    if (!s_sky_type || !g_find_objects_all || !g_get_game_object || !g_get_name ||
        !g_get_go_transform || !g_get_position || !g_array_get_length || !g_array_get_value) {
        LOG_INFO("veh sky candidates: unavailable");
        return;
    }
    Vector3 copy_p{0.0f, 0.0f, 0.0f};
    bool have_copy = false;
    if (copy_go) {
        void* ctf = g_get_go_transform.call(copy_go, g_get_go_transform.method);
        if (ctf) {
            copy_p = g_get_position.call(ctf, g_get_position.method);
            have_copy = true;
        }
    }
    void* list = g_find_objects_all.call(s_sky_type, true, g_find_objects_all.method);
    if (!list) {
        LOG_INFO("veh sky candidates: none");
        return;
    }
    int32_t count = g_array_get_length.call(list, g_array_get_length.method);
    if (count > 10) count = 10;
    for (int32_t i = 0; i < count; i++) {
        void* comp = g_array_get_value.call(list, i, g_array_get_value.method);
        if (!comp) continue;
        void* go = g_get_game_object.call(comp, g_get_game_object.method);
        char name[48] = {0};
        if (go) {
            void* text = g_get_name.call(go, g_get_name.method);
            if (!read_managed_name(text, name, sizeof(name))) {
                snprintf(name, sizeof(name), "%s", "?");
            }
        } else {
            snprintf(name, sizeof(name), "%s", "?");
        }
        int enabled = -1;
        if (g_behaviour_get_enabled) {
            enabled =
                g_behaviour_get_enabled.call(comp, g_behaviour_get_enabled.method) ? 1 : 0;
        }
        Vector3 p{0.0f, 0.0f, 0.0f};
        if (go) {
            void* tf = g_get_go_transform.call(go, g_get_go_transform.method);
            if (tf) p = g_get_position.call(tf, g_get_position.method);
        }
        float dist = -1.0f;
        if (have_copy) {
            float dx = p.x - copy_p.x;
            float dy = p.y - copy_p.y;
            float dz = p.z - copy_p.z;
            dist = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        LOG_INFO("veh sky candidates: [go=%p name=%s enabled=%d position=(%.1f,%.1f,%.1f) "
                 "distanceToCopy=%.1f]",
                 go, name, enabled, p.x, p.y, p.z, dist);
    }
}

// Reads UniqueIdentifier.Id off any component's GameObject. Vehicles are not Pickupables, so
// pickup_guid cannot take them; the hash agrees on both ends like every other guid here.
bool component_guid(void* component, uint8_t out[16]) {
    if (!component || !g_unique_get_id || !g_get_component || !g_unique_identifier_type) {
        return false;
    }
    void* game_object = g_get_game_object.call(component, g_get_game_object.method);
    if (!game_object) return false;
    void* identifier = g_get_component.call(game_object, g_unique_identifier_type,
                                            g_get_component.method);
    if (!identifier) return false;
    void* text = g_unique_get_id.call(identifier, g_unique_get_id.method);
    char ascii[128];
    if (!string_to_ascii(text, ascii, sizeof(ascii))) return false;
    guid_from_string(ascii, out);
    return true;
}

// Finds any scene object by guid through its UniqueIdentifier (vehicles included). Heavy -
// walks every identifier - so callers resolve once per guid and cache the pointer.
void* find_object_by_guid(const uint8_t guid[16]) {
    if (!g_find_objects_all || !g_unique_identifier_type || !g_array_get_length ||
        !g_array_get_value || !g_get_game_object || !g_unique_get_id) {
        return nullptr;
    }
    void* list = g_find_objects_all.call(g_unique_identifier_type, true,
                                         g_find_objects_all.method);
    if (!list) return nullptr;
    int32_t count = g_array_get_length.call(list, g_array_get_length.method);
    for (int32_t i = 0; i < count; i++) {
        void* identifier = g_array_get_value.call(list, i, g_array_get_value.method);
        if (!identifier) continue;
        void* text = g_unique_get_id.call(identifier, g_unique_get_id.method);
        char ascii[128];
        if (!string_to_ascii(text, ascii, sizeof(ascii))) continue;
        uint8_t candidate[16];
        guid_from_string(ascii, candidate);
        if (memcmp(candidate, guid, 16) == 0) {
            return g_get_game_object.call(identifier, g_get_game_object.method);
        }
    }
    return nullptr;
}

struct DrivenVehicle {
    bool used;
    uint32_t pilot;
    uint8_t guid[16];
    // Prefab tag from the wire (see publish_local_vehicle). Kept so a future
    // spawn-from-prefab path can instantiate the type when the guid has no
    // local match; today it is diagnostic only.
    char classId[16];
    void* game_object;
    // True when game_object is our own prefab spawn (destroy with the slot).
    // False for guid-matched world objects, which must never be destroyed.
    bool spawned;
    // Last frame the peer's STATE still carried the piloting flag. A single
    // reordered STATE clears the bit and must not instantly destroy the copy
    // (spawn churn every few frames); the release below waits ~4 s. Memset-safe.
    unsigned long last_piloting_frame;
    bool was_kinematic;
    bool have_kinematic;
    uint32_t last_seq;
    unsigned long last_seq_frame;
    unsigned long next_resolve_frame;
    // SkyApplier state is checked only after the clone has been activated and had time to run
    // Awake/OnEnable/Start. Do not retain a managed array pointer across frames; only its numeric
    // address is snapshotted for identity comparison and is never dereferenced later.
    void* sky_applier;
    uintptr_t sky_initial_array_addr;
    int32_t sky_initial_array_len;
    size_t sky_initial_array_max;
    int32_t sky_expected_renderers;
    int32_t sky_anchor;
    bool sky_dynamic;
    bool sky_emissive;
    unsigned long sky_verify_frame;
    bool sky_verified;
};
static DrivenVehicle g_driven[4];
static uint8_t g_my_vehicle_guid[16];
static bool g_my_vehicle_valid = false;

void release_driven(DrivenVehicle* slot, const char* why) {
    if (!slot || !slot->used) return;
    if (slot->spawned && slot->game_object && g_object_destroy) {
        // Our own prefab copy: remove it. Matched world objects are never
        // destroyed here, only un-kinematic'd below.
        g_object_destroy.call(slot->game_object, g_object_destroy.method);
        slot->game_object = nullptr;
    }
    if (slot->game_object && slot->have_kinematic && g_rigidbody_type && g_get_component &&
        !g_get_component_is_string && g_rb_set_kinematic) {
        void* body =
            g_get_component.call(slot->game_object, g_rigidbody_type, g_get_component.method);
        if (body) {
            g_rb_set_kinematic.call(body, slot->was_kinematic, g_rb_set_kinematic.method);
        }
    }
    // Throttled: exit/gone churn would otherwise log every frame.
    static unsigned long s_last_release_log = 0;
    if (g_avatar_frame - s_last_release_log > 150) {
        s_last_release_log = g_avatar_frame;
        LOG_INFO("veh: released pilot %08x (%s)", slot->pilot, why);
    }
    memset(slot, 0, sizeof(*slot));
}

void clear_driven_vehicles() {
    // World change: the scene died with every cached pointer, so drop them without touching
    // anything (restoring flags on freed objects is a use-after-free).
    memset(g_driven, 0, sizeof(g_driven));
    g_my_vehicle_valid = false;
}

// Prefab pipeline pieces owned by the held-prop section below; declared here so the
// vehicle spawn can reuse the TechType -> classId -> prefab cache without moving code.
// (PrefabRequest itself is fully defined above, so field access needs no forward decl.)
void* poll_prefab_request(uint32_t tech);
PrefabRequest* prefab_slot_for(uint32_t tech, bool create);
void* find_prefab_source(uint32_t tech);
void sanitize_held_prop(void* prop, bool keep_prefab_layers);

// ---- temporary Luna diagnostics: prefab-sky + material audit on spawned copies ----
// Entries for Renderer.get_sharedMaterials / Material getters. Resolved once in
// resolve_item_api; all guarded at use.
struct ColorF {
    float r;
    float g;
    float b;
    float a;
};
using GetSharedMatsFn = void* (*)(void* self, void* method);
using GetShaderFn = void* (*)(void* self, void* method);
using GetMainTexFn = void* (*)(void* self, void* method);
using GetTextureByNameFn = void* (*)(void* self, void* name_str, void* method);
using GetColorFn = ColorF (*)(void* self, void* method);
using GetTexPropNamesFn = void* (*)(void* self, void* method);
Entry<GetSharedMatsFn> g_rend_get_shared_mats;
Entry<GetShaderFn> g_mat_get_shader;
Entry<GetMainTexFn> g_mat_get_main_tex;
Entry<GetTextureByNameFn> g_mat_get_texture;
Entry<GetColorFn> g_mat_get_color;
Entry<GetTexPropNamesFn> g_mat_get_tex_props;
void* g_bump_name_str = nullptr;

// Logs the prefab's own SkyApplier config BEFORE we touch the copy: anchor, custom
// prefab, flags and its renderer count. Tells Luna whether the prefab ships lighting.
void log_vehicle_prefab_sky(uint32_t tech, void* prefab) {
    void* sky = nullptr;
    int anchor = -1;
    int dyn = -1;
    int emi = -1;
    int rcount = -1;
    void* custom = nullptr;
    if (prefab && g_sky_type_obj && g_get_component && !g_get_component_is_string) {
        sky = find_component_in_hierarchy(prefab, g_sky_type_obj, g_sky_name_str, 0);
    }
    if (sky) {
        char* b = reinterpret_cast<char*>(sky);
        anchor = *reinterpret_cast<int32_t*>(b + kSkyAnchorSkyOffset);
        dyn = *reinterpret_cast<bool*>(b + kSkyDynamicOffset) ? 1 : 0;
        emi = *reinterpret_cast<bool*>(b + kSkyEmissiveFromPowerOffset) ? 1 : 0;
        custom = *reinterpret_cast<void**>(b + kSkyCustomPrefabOffset);
        void* arr = *reinterpret_cast<void**>(b + kSkyRenderersOffset);
        rcount = (arr && g_array_get_length)
                     ? g_array_get_length.call(arr, g_array_get_length.method)
                     : -1;
    }
    LOG_INFO("veh prefab sky: tech=%u prefab=%p applier=%p anchorSky=%d customPrefab=%p "
             "dynamic=%d emissive=%d rendererCount=%d",
             tech, prefab, sky, anchor, custom, dyn, emi, rcount);
}

// Single-line material audit over the copy's own renderers: shader names (deduped),
// null main/normal textures, near-black base colors. Read-only: sharedMaterials,
// never material (which would instance).
void log_vehicle_material_audit(void* vehicle_go) {
    if (!vehicle_go || !g_rend_get_shared_mats || !g_get_go_transform || !g_get_child ||
        !g_get_child_count || !g_get_game_object || !g_get_component || !g_renderer_type_obj ||
        !g_array_get_length || !g_array_get_value) {
        LOG_INFO("veh mat audit: prerequisites missing");
        return;
    }
    char shaders[8][64];
    int shader_count = 0;
    int renderers = 0;
    int slots = 0;
    int null_main = 0;
    int null_bump = 0;
    int black = 0;
    int unsupported = 0;
    void* stack[256];
    size_t top = 0;
    stack[top++] = vehicle_go;
    size_t visited = 0;
    while (top > 0 && visited < sizeof(stack) / sizeof(stack[0])) {
        void* node = stack[--top];
        visited++;
        void* renderer =
            g_get_component.call(node, g_renderer_type_obj, g_get_component.method);
        if (renderer) {
            renderers++;
            void* mats = g_rend_get_shared_mats.call(renderer, g_rend_get_shared_mats.method);
            int32_t count =
                mats ? g_array_get_length.call(mats, g_array_get_length.method) : 0;
            for (int32_t i = 0; i < count; i++) {
                void* mat = g_array_get_value.call(mats, i, g_array_get_value.method);
                if (!mat) continue;
                slots++;
                // Shader name.
                char sname[64] = {0};
                bool have_name = false;
                if (g_mat_get_shader) {
                    void* shader =
                        g_mat_get_shader.call(mat, g_mat_get_shader.method);
                    void* text = nullptr;
                    if (shader) {
                        // get_name lives on UnityEngine.Object; resolve per class below,
                        // fall back to the shared Object.get_name entry.
                        text = g_get_name ? g_get_name.call(shader, g_get_name.method)
                                          : nullptr;
                    }
                    if (text) have_name = string_to_ascii(text, sname, sizeof(sname));
                }
                if (!have_name) snprintf(sname, sizeof(sname), "%s", "?");
                bool known = false;
                for (int k = 0; k < shader_count; k++) {
                    if (strcmp(shaders[k], sname) == 0) {
                        known = true;
                        break;
                    }
                }
                if (!known) {
                    if (shader_count < 8) snprintf(shaders[shader_count], 64, "%s", sname);
                    shader_count++;
                    // Heuristic, stated as such: the game shades almost everything with
                    // Marmoset Uber. Anything else on a vehicle copy is worth Luna's look.
                    if (!strstr(sname, "Marmoset")) unsupported++;
                }
                if (g_mat_get_main_tex &&
                    !g_mat_get_main_tex.call(mat, g_mat_get_main_tex.method)) {
                    null_main++;
                }
                if (g_bump_name_str && g_mat_get_texture &&
                    !g_mat_get_texture.call(mat, g_bump_name_str,
                                            g_mat_get_texture.method)) {
                    null_bump++;
                }
                if (g_mat_get_color) {
                    ColorF c = g_mat_get_color.call(mat, g_mat_get_color.method);
                    if (c.r < 0.02f && c.g < 0.02f && c.b < 0.02f) black++;
                }
            }
        }
        void* tf = g_get_go_transform.call(node, g_get_go_transform.method);
        if (!tf) continue;
        int32_t kids = g_get_child_count.call(tf, g_get_child_count.method);
        for (int32_t i = 0; i < kids && top < sizeof(stack) / sizeof(stack[0]); i++) {
            void* ctf = g_get_child.call(tf, i, g_get_child.method);
            if (!ctf) continue;
            void* cgo = g_get_game_object.call(ctf, g_get_game_object.method);
            if (cgo) stack[top++] = cgo;
        }
    }
    char shader_list[8 * 66] = {0};
    int off = 0;
    int shown = shader_count < 8 ? shader_count : 8;
    for (int k = 0; k < shown; k++) {
        off += snprintf(shader_list + off, sizeof(shader_list) - (size_t)off, "%s%s", k ? "," : "",
                        shaders[k]);
    }
    LOG_INFO("veh mat audit: renderers=%d slots=%d shaders=[%s] unsupportedShaders=%d "
             "nullMainTex=%d nullNormalTex=%d blackColor=%d",
             renderers, slots, shader_list, unsupported, null_main, null_bump, black);
}

// Temporary Luna diagnostics, round 2: per-slot detail + prefab-vs-clone compare.
// Builds "root/a/b" paths by walking up through parent transforms (hand-rolled, not the
// avatar RelativePath helper, which is anchored at the player body).
bool vehicle_node_path(void* root_go, void* node_go, char* out, size_t cap) {
    if (!root_go || !node_go || !out || cap == 0 || !g_get_go_transform ||
        !g_transform_get_parent || !g_get_game_object || !g_get_name) {
        return false;
    }
    char parts[12][64];
    size_t depth = 0;
    void* current = node_go;
    while (current && current != root_go && depth < 12) {
        if (!read_managed_name(g_get_name.call(current, g_get_name.method), parts[depth],
                               sizeof(parts[depth]))) {
            return false;
        }
        depth++;
        void* tf = g_get_go_transform.call(current, g_get_go_transform.method);
        if (!tf) return false;
        void* parent_tf = g_transform_get_parent.call(tf, g_transform_get_parent.method);
        if (!parent_tf) break;
        current = g_get_game_object.call(parent_tf, g_get_game_object.method);
    }
    if (current != root_go) return false;
    size_t pos = 0;
    for (size_t i = depth; i > 0; i--) {
        pos += (size_t)snprintf(out + pos, cap > pos ? cap - pos : 0, "%s%s", pos ? "/" : "",
                                parts[i - 1]);
    }
    return depth > 0;
}

// Walks down from root following a "a/b/c" path, matching child names.
void* vehicle_find_by_path(void* root_go, const char* path) {
    if (!root_go || !path || !*path || !g_get_go_transform || !g_get_child ||
        !g_get_child_count || !g_get_game_object || !g_get_name) {
        return nullptr;
    }
    char copy[160];
    snprintf(copy, sizeof(copy), "%s", path);
    void* current = root_go;
    char* save = nullptr;
    // strtok_r may be absent on this libc; manual split on '/'.
    char* seg = copy;
    while (seg && *seg) {
        char* slash = strchr(seg, '/');
        if (slash) *slash = '\0';
        void* tf = g_get_go_transform.call(current, g_get_go_transform.method);
        if (!tf) return nullptr;
        int32_t kids = g_get_child_count.call(tf, g_get_child_count.method);
        void* next = nullptr;
        for (int32_t i = 0; i < kids && !next; i++) {
            void* ctf = g_get_child.call(tf, i, g_get_child.method);
            if (!ctf) continue;
            void* cgo = g_get_game_object.call(ctf, g_get_game_object.method);
            if (!cgo) continue;
            char name[64] = {0};
            if (!read_managed_name(g_get_name.call(cgo, g_get_name.method), name,
                                   sizeof(name))) {
                continue;
            }
            if (strcmp(name, seg) == 0) next = cgo;
        }
        if (!next) return nullptr;
        current = next;
        seg = slash ? slash + 1 : nullptr;
    }
    (void)save;
    return current;
}

// One material slot as "matName@ptr/shader/prop:tex@ptr|NULL;...". Texture properties
// are enumerated from the material itself (cap 6), so Luna sees every slot, not just
// _MainTex/_BumpMap.
void describe_material_slot(void* mat, char* out, size_t cap) {
    if (!mat || !out || cap == 0) {
        if (out && cap) snprintf(out, cap, "%s", "null");
        return;
    }
    char mname[48] = {0};
    if (g_get_name &&
        !read_managed_name(g_get_name.call(mat, g_get_name.method), mname, sizeof(mname))) {
        snprintf(mname, sizeof(mname), "%s", "?");
    }
    char sname[48] = {0};
    if (g_mat_get_shader) {
        void* shader = g_mat_get_shader.call(mat, g_mat_get_shader.method);
        void* text = (shader && g_get_name)
                         ? g_get_name.call(shader, g_get_name.method)
                         : nullptr;
        if (!text || !string_to_ascii(text, sname, sizeof(sname))) {
            snprintf(sname, sizeof(sname), "%s", "?");
        }
    } else {
        snprintf(sname, sizeof(sname), "%s", "?");
    }
    char props[320] = {0};
    size_t pos = 0;
    if (g_mat_get_tex_props && g_mat_get_texture) {
        void* names =
            g_mat_get_tex_props.call(mat, g_mat_get_tex_props.method);
        int32_t count =
            names && g_array_get_length
                ? g_array_get_length.call(names, g_array_get_length.method)
                : 0;
        if (count > 6) count = 6;
        for (int32_t i = 0; i < count; i++) {
            void* name_str = g_array_get_value.call(names, i, g_array_get_value.method);
            char pname[48] = {0};
            if (!read_managed_name(name_str, pname, sizeof(pname))) continue;
            void* key = cached_managed_string(pname);
            void* tex = key ? g_mat_get_texture.call(mat, key, g_mat_get_texture.method)
                            : nullptr;
            char tname[40] = {0};
            if (tex && g_get_name) {
                void* text = g_get_name.call(tex, g_get_name.method);
                if (!text || !read_managed_name(text, tname, sizeof(tname))) {
                    snprintf(tname, sizeof(tname), "%s", "?");
                }
            }
            pos += (size_t)snprintf(props + pos, sizeof(props) > pos ? sizeof(props) - pos : 0,
                                    "%s%s:%s@%p", pos ? ";" : "", pname,
                                    tex ? tname : "NULL", tex);
        }
    }
    snprintf(out, cap, "%s@%p/%s/%s", mname, mat, sname, props);
}

// Per-slot detail for every clone slot with an unsupported shader or an empty texture,
// then the same slots on the prefab for comparison. Called once per spawn.
void log_vehicle_material_detail(void* prefab, void* vehicle_go) {
    if (!prefab || !vehicle_go || !g_rend_get_shared_mats || !g_get_go_transform ||
        !g_get_child || !g_get_child_count || !g_get_game_object || !g_get_component ||
        !g_renderer_type_obj || !g_array_get_length || !g_array_get_value) {
        return;
    }
    struct BadSlot {
        char path[160];
        int slot;
    };
    BadSlot bad[12];
    int bad_count = 0;

    void* stack[256];
    size_t top = 0;
    stack[top++] = vehicle_go;
    size_t visited = 0;
    while (top > 0 && visited < sizeof(stack) / sizeof(stack[0]) &&
           bad_count < (int)(sizeof(bad) / sizeof(bad[0]))) {
        void* node = stack[--top];
        visited++;
        char path[160] = {0};
        bool have_path = vehicle_node_path(vehicle_go, node, path, sizeof(path));
        void* renderer =
            g_get_component.call(node, g_renderer_type_obj, g_get_component.method);
        if (renderer && have_path) {
            void* mats = g_rend_get_shared_mats.call(renderer, g_rend_get_shared_mats.method);
            int32_t count =
                mats ? g_array_get_length.call(mats, g_array_get_length.method) : 0;
            for (int32_t i = 0; i < count && bad_count < (int)(sizeof(bad) / sizeof(bad[0]));
                 i++) {
                void* mat = g_array_get_value.call(mats, i, g_array_get_value.method);
                if (!mat) continue;
                char sname[48] = {0};
                if (g_mat_get_shader) {
                    void* shader = g_mat_get_shader.call(mat, g_mat_get_shader.method);
                    void* text = (shader && g_get_name)
                                     ? g_get_name.call(shader, g_get_name.method)
                                     : nullptr;
                    if (!text || !string_to_ascii(text, sname, sizeof(sname))) {
                        snprintf(sname, sizeof(sname), "%s", "?");
                    }
                } else {
                    snprintf(sname, sizeof(sname), "%s", "?");
                }
                bool supported = strstr(sname, "Marmoset") != nullptr;
                bool main_null = g_mat_get_main_tex &&
                                 !g_mat_get_main_tex.call(mat, g_mat_get_main_tex.method);
                bool bump_null =
                    !(g_bump_name_str && g_mat_get_texture &&
                      g_mat_get_texture.call(mat, g_bump_name_str,
                                             g_mat_get_texture.method));
                if (supported && !main_null && !bump_null) continue;
                char desc[512] = {0};
                describe_material_slot(mat, desc, sizeof(desc));
                LOG_INFO("veh mat detail: [%s/%d/%s/%s]",
                         path, i, desc, supported ? "supported" : "UNSUPPORTED");
                snprintf(bad[bad_count].path, sizeof(bad[bad_count].path), "%s", path);
                bad[bad_count].slot = i;
                bad_count++;
            }
        }
        void* tf = g_get_go_transform.call(node, g_get_go_transform.method);
        if (!tf) continue;
        int32_t kids = g_get_child_count.call(tf, g_get_child_count.method);
        for (int32_t i = 0; i < kids && top < (int)(sizeof(stack) / sizeof(stack[0])); i++) {
            void* ctf = g_get_child.call(tf, i, g_get_child.method);
            if (!ctf) continue;
            void* cgo = g_get_game_object.call(ctf, g_get_game_object.method);
            if (cgo) stack[top++] = cgo;
        }
    }

    for (int b = 0; b < bad_count; b++) {
        char src_desc[512] = {0};
        snprintf(src_desc, sizeof(src_desc), "%s", "MISSING");
        void* src_node = vehicle_find_by_path(prefab, bad[b].path);
        if (src_node) {
            void* src_rend = g_get_component.call(src_node, g_renderer_type_obj,
                                                  g_get_component.method);
            if (src_rend) {
                void* src_mats = g_rend_get_shared_mats.call(
                    src_rend, g_rend_get_shared_mats.method);
                int32_t src_count =
                    src_mats ? g_array_get_length.call(src_mats, g_array_get_length.method)
                             : 0;
                if (bad[b].slot < src_count) {
                    void* src_mat =
                        g_array_get_value.call(src_mats, bad[b].slot,
                                               g_array_get_value.method);
                    if (src_mat) describe_material_slot(src_mat, src_desc, sizeof(src_desc));
                }
            }
        }
        // Clone side, re-read at compare time so both halves are same-frame.
        char clone_desc[512] = {0};
        snprintf(clone_desc, sizeof(clone_desc), "%s", "MISSING");
        void* clone_node = vehicle_find_by_path(vehicle_go, bad[b].path);
        if (clone_node) {
            void* clone_rend = g_get_component.call(clone_node, g_renderer_type_obj,
                                                    g_get_component.method);
            if (clone_rend) {
                void* clone_mats = g_rend_get_shared_mats.call(
                    clone_rend, g_rend_get_shared_mats.method);
                int32_t clone_count =
                    clone_mats
                        ? g_array_get_length.call(clone_mats, g_array_get_length.method)
                        : 0;
                if (bad[b].slot < clone_count) {
                    void* clone_mat =
                        g_array_get_value.call(clone_mats, bad[b].slot,
                                               g_array_get_value.method);
                    if (clone_mat) {
                        describe_material_slot(clone_mat, clone_desc, sizeof(clone_desc));
                    }
                }
            }
        }
        LOG_INFO("veh mat compare: [%s/%d/%s | %s]", bad[b].path, bad[b].slot, src_desc,
                 clone_desc);
    }
}

// Vehicle class name -> TechType (dump.cs, build 1.22.83315: Seamoth=2000,
// Exosuit=2001, Cyclops=2003). Static table instead of Enum.Parse: no reflection,
// no boxing, no managed-string lifetime hazards. Re-verify from dump.cs on update.
uint32_t vehicle_tech_for_class(const char* class_id) {
    if (!class_id || !class_id[0]) return 0;
    char lower[16];
    size_t i = 0;
    for (; class_id[i] && i + 1 < sizeof(lower); i++) {
        char c = class_id[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    }
    lower[i] = '\0';
    if (strcmp(lower, "seamoth") == 0) return 2000;
    if (strcmp(lower, "exosuit") == 0) return 2001;
    if (strcmp(lower, "cyclops") == 0) return 2003;
    return 0;
}

// Builds and assigns a fresh Renderer[] containing renderers from THIS vehicle hierarchy.
// The class is obtained from the game's own Renderer[] so the array element type is exact.
// The array is immediately stored in SkyApplier before its elements are written, matching the
// rooting pattern already used by populate_clone_sky_renderers.
bool rebuild_vehicle_sky_renderers(void* vehicle_go, void* sky, int32_t* out_count) {
    if (out_count) *out_count = 0;
    if (!vehicle_go || !sky || !g_local_game_object || !g_get_component ||
        !g_get_go_transform || !g_get_child || !g_get_child_count || !g_get_game_object ||
        !il2cpp_array_new || !il2cpp_object_get_class) {
        LOG_WARN("veh sky: renderer rebuild prerequisites missing");
        return false;
    }

    void* local_sky = find_component_in_hierarchy(g_local_game_object, g_sky_type_obj,
                                                  g_sky_name_str, 0);
    void* local_array = local_sky
                            ? *reinterpret_cast<void**>(reinterpret_cast<char*>(local_sky) +
                                                        kSkyRenderersOffset)
                            : nullptr;
    if (!local_array) {
        LOG_WARN("veh sky: local SkyApplier has no Renderer[]; cannot derive array class");
        return false;
    }
    struct Il2CppClass* array_class = il2cpp_object_get_class(
        reinterpret_cast<struct Il2CppObject*>(local_array));
    if (!array_class) {
        LOG_WARN("veh sky: Renderer[] class lookup failed");
        return false;
    }

    void* found[256] = {};
    void* stack[256] = {};
    size_t n = 0;
    size_t top = 0;
    size_t visited = 0;
    bool truncated = false;
    stack[top++] = vehicle_go;
    while (top > 0 && visited < sizeof(stack) / sizeof(stack[0])) {
        void* node = stack[--top];
        ++visited;
        void* renderer =
            g_get_component.call(node, g_renderer_type_obj, g_get_component.method);
        if (renderer && n < sizeof(found) / sizeof(found[0])) {
            found[n++] = renderer;
        } else if (renderer) {
            truncated = true;
        }

        void* tf = g_get_go_transform.call(node, g_get_go_transform.method);
        if (!tf) continue;
        int32_t children = g_get_child_count.call(tf, g_get_child_count.method);
        for (int32_t i = 0; i < children; ++i) {
            void* child_tf = g_get_child.call(tf, i, g_get_child.method);
            if (!child_tf) continue;
            void* child_go = g_get_game_object.call(child_tf, g_get_game_object.method);
            if (!child_go) continue;
            if (top < sizeof(stack) / sizeof(stack[0])) {
                stack[top++] = child_go;
            } else {
                truncated = true;
            }
        }
    }
    if (top > 0) truncated = true;
    if (!n) {
        LOG_WARN("veh sky: no Renderer components found under copy %p", vehicle_go);
        return false;
    }

    void* array = il2cpp_array_new(array_class, static_cast<unsigned int>(n));
    if (!array) {
        LOG_WARN("veh sky: Renderer[] allocation failed (count=%zu)", n);
        return false;
    }
    // Root the new managed array through the component before filling its reference slots.
    *reinterpret_cast<void**>(reinterpret_cast<char*>(sky) + kSkyRenderersOffset) = array;
    char* elements = reinterpret_cast<char*>(array) + kArrayDataOffset;
    for (size_t i = 0; i < n; ++i) {
        *reinterpret_cast<void**>(elements + i * sizeof(void*)) = found[i];
    }

    size_t max_length =
        *reinterpret_cast<size_t*>(reinterpret_cast<char*>(array) + kArrayMaxLengthOffset);
    LOG_INFO("veh sky: renderer array rebuilt=%p count=%zu maxLength=%zu traversal=%s",
             array, n, max_length, truncated ? "TRUNCATED" : "complete");
    if (max_length != n || truncated) {
        LOG_WARN("veh sky: renderer collection incomplete (count=%zu maxLength=%zu truncated=%d)",
                 n, max_length, truncated ? 1 : 0);
    }
    if (out_count) *out_count = static_cast<int32_t>(n);
    return max_length == n && !truncated;
}

// Attaches/configures the copy while it is inactive. Crucially, environment broadcast and
// RefreshDirtySky are deferred until verify_vehicle_sky(), after activation and Start(). The old
// implementation ran both while the target GameObject was inactive and before Start could finish.
void attach_vehicle_sky(void* vehicle_go, DrivenVehicle* slot) {
    if (!vehicle_go || !slot || !g_add_component || !g_sky_type_obj || !g_renderer_type_obj ||
        !g_get_go_transform || !g_get_child || !g_get_child_count || !g_get_game_object ||
        !g_get_component || !il2cpp_array_new || !il2cpp_object_get_class) {
        LOG_WARN("veh sky: prerequisites missing, copy stays unlit");
        return;
    }

    void* sky = find_component_in_hierarchy(vehicle_go, g_sky_type_obj, g_sky_name_str, 0);
    bool added = false;
    if (!sky) {
        sky = g_add_component.call(vehicle_go, g_sky_type_obj, g_add_component.method);
        added = true;
    }
    if (!sky) {
        LOG_WARN("veh sky: no applier available on copy %p", vehicle_go);
        return;
    }

    char* base = reinterpret_cast<char*>(sky);
    if (added) {
        // Nitrox's reference setup for a runtime-created applier: Auto sky, dynamic updates,
        // no power-derived emissive. Do not copy the local Player's context-dependent config.
        *reinterpret_cast<int32_t*>(base + kSkyAnchorSkyOffset) = 0;  // Skies.Auto
        *reinterpret_cast<bool*>(base + kSkyDynamicOffset) = true;
        *reinterpret_cast<bool*>(base + kSkyEmissiveFromPowerOffset) = false;
    }
    // If the prefab already owns a SkyApplier, preserve its serialized customSkyPrefab and
    // anchorSky configuration instead of transplanting player-specific values onto the vehicle.
    slot->sky_applier = sky;
    slot->sky_anchor = *reinterpret_cast<int32_t*>(base + kSkyAnchorSkyOffset);
    slot->sky_dynamic = *reinterpret_cast<bool*>(base + kSkyDynamicOffset);
    slot->sky_emissive = *reinterpret_cast<bool*>(base + kSkyEmissiveFromPowerOffset);

    if (g_behaviour_get_enabled && g_behaviour_set_enabled) {
        bool enabled = g_behaviour_get_enabled.call(sky, g_behaviour_get_enabled.method);
        if (!enabled) {
            g_behaviour_set_enabled.call(sky, true, g_behaviour_set_enabled.method);
            LOG_INFO("veh sky: enabled prefab SkyApplier %p", sky);
        }
    }

    int32_t renderer_count = 0;
    if (!rebuild_vehicle_sky_renderers(vehicle_go, sky, &renderer_count)) {
        LOG_WARN("veh sky: initial Renderer[] build did not fully complete");
    }
    slot->sky_expected_renderers = renderer_count;
    slot->sky_verify_frame = g_avatar_frame + 2;
    slot->sky_verified = false;

    void* prefab = *reinterpret_cast<void**>(base + kSkyCustomPrefabOffset);
    void* renderers = *reinterpret_cast<void**>(base + kSkyRenderersOffset);
    int32_t renderer_length = renderers && g_array_get_length
                                  ? g_array_get_length.call(renderers, g_array_get_length.method)
                                  : -1;
    size_t max_length = renderers
                            ? *reinterpret_cast<size_t*>(reinterpret_cast<char*>(renderers) +
                                                         kArrayMaxLengthOffset)
                            : 0;
    slot->sky_initial_array_addr = reinterpret_cast<uintptr_t>(renderers);
    slot->sky_initial_array_len = renderer_length;
    slot->sky_initial_array_max = max_length;
    LOG_INFO("veh sky pre-Start: applier=%p added=%d anchorSky=%d customPrefab=%p dynamic=%d "
             "emissive=%d renderers=%p expected=%d maxLength=%zu env=%p apply=%p updater=%d",
             sky, added ? 1 : 0, slot->sky_anchor, prefab, slot->sky_dynamic ? 1 : 0,
             slot->sky_emissive ? 1 : 0, renderers, renderer_count, max_length,
             *reinterpret_cast<void**>(base + kSkyEnvironmentSkyOffset),
             *reinterpret_cast<void**>(base + kSkyApplySkyOffset),
             *reinterpret_cast<int32_t*>(base + kSkyUpdaterIndexOffset));
}

// Runs only after the vehicle is active for two game updates. Rebuilds Renderer[] unconditionally
// so a SkyApplier.Start() overwrite (even one that kept the same array length) cannot leave stale
// prefab/local references in place. Broadcast/refresh now run against an active, initialized target.
void verify_vehicle_sky(DrivenVehicle* slot) {
    if (!slot || !slot->game_object || !slot->sky_applier || slot->sky_verified ||
        g_avatar_frame < slot->sky_verify_frame) {
        return;
    }

    char* base = reinterpret_cast<char*>(slot->sky_applier);
    void* before_array = *reinterpret_cast<void**>(base + kSkyRenderersOffset);
    int32_t before_count = before_array && g_array_get_length
                               ? g_array_get_length.call(before_array, g_array_get_length.method)
                               : -1;
    size_t before_max = before_array
                            ? *reinterpret_cast<size_t*>(reinterpret_cast<char*>(before_array) +
                                                         kArrayMaxLengthOffset)
                            : 0;
    bool start_replaced_array =
        reinterpret_cast<uintptr_t>(before_array) != slot->sky_initial_array_addr;
    void* before_env = *reinterpret_cast<void**>(base + kSkyEnvironmentSkyOffset);
    void* before_apply = *reinterpret_cast<void**>(base + kSkyApplySkyOffset);
    int32_t before_updater = *reinterpret_cast<int32_t*>(base + kSkyUpdaterIndexOffset);

    // Start/Awake may have restored prefab defaults. Reapply the saved per-applier config after
    // Start; customSkyPrefab remains the prefab's own managed asset reference and is not replaced.
    *reinterpret_cast<int32_t*>(base + kSkyAnchorSkyOffset) = slot->sky_anchor;
    *reinterpret_cast<bool*>(base + kSkyDynamicOffset) = slot->sky_dynamic;
    *reinterpret_cast<bool*>(base + kSkyEmissiveFromPowerOffset) = slot->sky_emissive;

    int32_t rebuilt_count = 0;
    bool rebuilt = rebuild_vehicle_sky_renderers(slot->game_object, slot->sky_applier,
                                                  &rebuilt_count);
    if (!rebuilt) {
        LOG_WARN("veh sky: post-Start renderer rebuild failed; retrying in 30 frames");
        slot->sky_verify_frame = g_avatar_frame + 30;
        return;
    }
    slot->sky_expected_renderers = rebuilt_count;

    bool broadcast = false;
    bool refreshed = false;
    void* pre_env = *reinterpret_cast<void**>(base + kSkyEnvironmentSkyOffset);
    void* pre_apply = *reinterpret_cast<void**>(base + kSkyApplySkyOffset);
    int32_t pre_updater = *reinterpret_cast<int32_t*>(base + kSkyUpdaterIndexOffset);
    if (g_sky_env_broadcast) {
        g_sky_env_broadcast.call(slot->game_object, nullptr, g_sky_env_broadcast.method);
        broadcast = true;
    }
    void* mid_env = *reinterpret_cast<void**>(base + kSkyEnvironmentSkyOffset);
    if (g_sky_refresh_dirty) {
        g_sky_refresh_dirty.call(slot->sky_applier, g_sky_refresh_dirty.method);
        refreshed = true;
    }

    void* after_array = *reinterpret_cast<void**>(base + kSkyRenderersOffset);
    int32_t after_count = after_array && g_array_get_length
                              ? g_array_get_length.call(after_array, g_array_get_length.method)
                              : -1;
    size_t after_max = after_array
                           ? *reinterpret_cast<size_t*>(reinterpret_cast<char*>(after_array) +
                                                        kArrayMaxLengthOffset)
                           : 0;
    void* after_env = *reinterpret_cast<void**>(base + kSkyEnvironmentSkyOffset);
    void* after_apply = *reinterpret_cast<void**>(base + kSkyApplySkyOffset);
    int32_t after_updater = *reinterpret_cast<int32_t*>(base + kSkyUpdaterIndexOffset);
    LOG_INFO("veh sky broadcast delta: beforeEnv=%p beforeApply=%p beforeUpdater=%d "
             "afterBroadcastEnv=%p afterRefreshEnv=%p afterApply=%p afterUpdater=%d",
             pre_env, pre_apply, pre_updater, mid_env, after_env, after_apply, after_updater);
    void* custom_prefab = *reinterpret_cast<void**>(base + kSkyCustomPrefabOffset);
    bool enabled = g_behaviour_get_enabled
                       ? g_behaviour_get_enabled.call(slot->sky_applier,
                                                      g_behaviour_get_enabled.method)
                       : true;

    LOG_INFO("veh sky post-Start: applier=%p enabled=%d startArrayChanged=%d "
             "initialArray=%p initialLen=%d initialMax=%zu currentArray=%p len=%d max=%zu "
             "envBefore=%p applyBefore=%p updaterBefore=%d expected=%d env=%p apply=%p "
             "updater=%d customPrefab=%p broadcast=%d refresh=%d",
             slot->sky_applier, enabled ? 1 : 0, start_replaced_array ? 1 : 0,
             reinterpret_cast<void*>(slot->sky_initial_array_addr), slot->sky_initial_array_len,
             slot->sky_initial_array_max, before_array, before_count, before_max, before_env,
             before_apply, before_updater, slot->sky_expected_renderers, after_env, after_apply,
             after_updater, custom_prefab, broadcast ? 1 : 0, refreshed ? 1 : 0);

    if (!enabled || after_count != slot->sky_expected_renderers ||
        after_max != static_cast<size_t>(slot->sky_expected_renderers)) {
        LOG_WARN("veh sky verify: NOT READY (enabled=%d count=%d max=%zu expected=%d)",
                 enabled ? 1 : 0, after_count, after_max, slot->sky_expected_renderers);
    } else {
        // environmentSky/applySky/updaterIndex are private runtime state. Their exact sentinel
        // semantics are not present in the supplied dump excerpt, so record them without treating
        // null/-1 as a proven failure. The confirmed facts are: enabled component, valid array,
        // and successful dispatch of the two methods (if their MethodInfo entries resolved).
        LOG_INFO("veh sky verify: renderer array OK; lifecycle fields recorded, not assumed "
                 "(broadcast=%d refresh=%d env=%p apply=%p updater=%d)",
                 broadcast ? 1 : 0, refreshed ? 1 : 0, after_env, after_apply, after_updater);
    }
    slot->sky_verified = true;
}

// Instantiates a visual copy of a vehicle the local save never had (diverged worlds:
// the peer built or drives something absent here). Mirrors spawn_held_prop: park
// inactive, disable the driver script, kinematic bodies, dead colliders - then the
// existing per-sequence teleport drives the copy. Returns true when settled
// (spawned, or hopeless so the caller stops retrying); false when an async prefab
// load is still in flight and the caller should try again later.
bool spawn_driven_from_prefab(DrivenVehicle* slot) {
    if (!slot || slot->game_object) return true;
    uint32_t tech = vehicle_tech_for_class(slot->classId);
    if (!tech) return true;  // unknown class; nothing will ever make this work
    if (!g_instantiate || !g_set_active || !g_get_go_transform || !g_get_component ||
        !g_behaviour_set_enabled) {
        return true;
    }
    void* prefab = find_prefab_source(tech);
    if (!prefab) return false;
    log_vehicle_prefab_sky(tech, prefab);
    Vector3 position{0.0f, 0.0f, 0.0f};
    Quaternion rotation{0.0f, 0.0f, 0.0f, 1.0f};
    void* go = g_instantiate.call(prefab, position, rotation, g_instantiate.method);
    if (!go) return false;
    g_set_active.call(go, false, g_set_active.method);
    // Kill the driver: a live Vehicle/SeaMoth Update would fight the teleport and
    // register with game managers. Disable (not Destroy), like the clone Player.
    // The concrete component carries every callback, so one disable is enough.
    // TESTED 2026-10-17: leaving the driver enabled makes the copy WORSE, not lit -
    // skipped Start() is not the cause. Keep disabled.
    constexpr bool kVehDisableDriver = true;
    if (slot->classId[0] && kVehDisableDriver) {
        struct Il2CppClass* vklass = find_class_in_domain("", slot->classId);
        void* driver = nullptr;
        int was_enabled = -1;
        if (vklass && il2cpp_class_get_type && il2cpp_type_get_object) {
            void* type_obj = il2cpp_type_get_object(il2cpp_class_get_type(vklass));
            if (type_obj) {
                driver = g_get_component.call(go, type_obj, g_get_component.method);
                if (driver && g_behaviour_get_enabled) {
                    was_enabled = g_behaviour_get_enabled.call(
                                      driver, g_behaviour_get_enabled.method)
                                      ? 1
                                      : 0;
                    g_behaviour_set_enabled.call(driver, false,
                                                 g_behaviour_set_enabled.method);
                }
            }
        }
        LOG_INFO("veh driver audit: class=%.15s component=%p enabled=%d->0", slot->classId,
                 driver, was_enabled);
    }
    sanitize_held_prop(go, true);  // keep prefab layers (see comment in sanitize)
    attach_vehicle_sky(go, slot);
    log_vehicle_material_audit(go);
    log_vehicle_material_detail(prefab, go);
    log_vehicle_bones_audit(prefab, go);
    log_vehicle_sky_candidates(go);
    log_vehicle_visibility_audit(go);
    g_set_active.call(go, true, g_set_active.method);
    slot->game_object = go;
    slot->spawned = true;
    LOG_INFO("veh: spawned visual %.15s for pilot %08x (go=%p)", slot->classId, slot->pilot,
             go);
    return true;
}

// Publishes the piloted vehicle, if any. Runs every frame; inactive publishes are gated so
// the pump lock is not churned while on foot.
void publish_local_vehicle(void* player_self, uint32_t flags) {
    if (!(flags & kFlagPiloting) || !g_player_get_vehicle) {
        if (g_my_vehicle_valid) {
            VehicleState out{};
            out.active = false;
            net_publish_vehicle(out);
            g_my_vehicle_valid = false;
        }
        return;
    }
    void* vehicle = g_player_get_vehicle.call(player_self, g_player_get_vehicle.method);
    if (!vehicle) {
        // Piloting without a Vehicle: Cyclops chair and friends, skipped by this pass.
        static bool s_logged_chair = false;
        if (!s_logged_chair) {
            s_logged_chair = true;
            LOG_INFO("veh: piloting without a Vehicle, vehicle sync skips");
        }
        if (g_my_vehicle_valid) {
            VehicleState out{};
            out.active = false;
            net_publish_vehicle(out);
            g_my_vehicle_valid = false;
        }
        return;
    }
    VehicleState out{};
    if (!component_guid(vehicle, out.guid)) return;  // keep last state on a guid miss
    // Prefab tag for peers whose save lacks this exact vehicle: the runtime class
    // name ("SeaMoth"/"Exosuit"/...) fits char[16] and is stable across saves,
    // unlike the per-save guid. Read-only, no Unity mutation.
    if (il2cpp_object_get_class && il2cpp_class_get_name) {
        struct Il2CppClass* vklass = il2cpp_object_get_class(
            reinterpret_cast<struct Il2CppObject*>(vehicle));
        const char* vname = vklass ? il2cpp_class_get_name(vklass) : nullptr;
        if (vname && vname[0]) snprintf(out.classId, sizeof(out.classId), "%s", vname);
    }
    void* go = g_get_game_object.call(vehicle, g_get_game_object.method);
    if (!go || !g_get_go_transform) return;
    void* tf = g_get_go_transform.call(go, g_get_go_transform.method);
    if (!tf) return;
    Vector3 p = g_get_position.call(tf, g_get_position.method);
    out.x = p.x;
    out.y = p.y;
    out.z = p.z;
    if (g_get_euler) {
        Vector3 e = g_get_euler.call(tf, g_get_euler.method);
        out.yaw = e.y;
        out.pitch = e.x;
        out.roll = e.z;
    }
    out.pilot = net_local_id();
    out.active = true;
    memcpy(g_my_vehicle_guid, out.guid, sizeof(g_my_vehicle_guid));
    g_my_vehicle_valid = true;
    // Temporary Luna diagnostics, round 3: reference config from a WORKING real sub,
    // logged once per piloted vehicle. If its env is non-null while the copy's stays
    // null, the copy is missing exactly the environment resolution step.
    {
        static uint8_t s_base_guid[16] = {0};
        static bool s_base_done = false;
        if (!s_base_done || memcmp(s_base_guid, out.guid, sizeof(s_base_guid)) != 0) {
            memcpy(s_base_guid, out.guid, sizeof(s_base_guid));
            s_base_done = true;
            int benabled = -1;
            int banchor = -1;
            void* bcustom = nullptr;
            void* benv = nullptr;
            void* bapply = nullptr;
            int bupdater = -1;
            void* bsky = (go && g_sky_type_obj && g_get_component && !g_get_component_is_string)
                             ? find_component_in_hierarchy(go, g_sky_type_obj,
                                                           g_sky_name_str, 0)
                             : nullptr;
            if (bsky) {
                char* b = reinterpret_cast<char*>(bsky);
                benabled =
                    (g_behaviour_get_enabled &&
                     g_behaviour_get_enabled.call(bsky, g_behaviour_get_enabled.method))
                        ? 1
                        : 0;
                banchor = *reinterpret_cast<int32_t*>(b + kSkyAnchorSkyOffset);
                bcustom = *reinterpret_cast<void**>(b + kSkyCustomPrefabOffset);
                benv = *reinterpret_cast<void**>(b + kSkyEnvironmentSkyOffset);
                bapply = *reinterpret_cast<void**>(b + kSkyApplySkyOffset);
                bupdater = *reinterpret_cast<int32_t*>(b + kSkyUpdaterIndexOffset);
            }
            LOG_INFO("veh sky baseline: go=%p enabled=%d anchorSky=%d customPrefab=%p env=%p "
                     "apply=%p updater=%d",
                     go, benabled, banchor, bcustom, benv, bapply, bupdater);
            // Temporary Luna diagnostics, round 4: the layer we force to 0 on copies.
            if (g_go_get_layer) {
                LOG_INFO("veh layer baseline: real=%d",
                         g_go_get_layer.call(go, g_go_get_layer.method));
            }
        }
    }
    net_publish_vehicle(out);
}

void tick_remote_vehicles(const PeerState* peers, size_t peer_count) {
    VehicleState remote[8];
    size_t remote_count = net_copy_vehicles(remote, 8);

    for (size_t r = 0; r < remote_count; r++) {
        DrivenVehicle* slot = nullptr;
        DrivenVehicle* free_slot = nullptr;
        for (size_t i = 0; i < 4; i++) {
            if (g_driven[i].used && g_driven[i].pilot == remote[r].pilot &&
                memcmp(g_driven[i].guid, remote[r].guid, 16) == 0) {
                slot = &g_driven[i];
                break;
            }
            if (!free_slot && !g_driven[i].used) free_slot = &g_driven[i];
        }
        if (!slot) {
            if (!free_slot) continue;  // table full (4 concurrent remote vehicles)
            slot = free_slot;
            memset(slot, 0, sizeof(*slot));
            slot->used = true;
            slot->pilot = remote[r].pilot;
            memcpy(slot->guid, remote[r].guid, sizeof(slot->guid));
            memcpy(slot->classId, remote[r].classId, sizeof(slot->classId));
            slot->last_seq_frame = g_avatar_frame;
            slot->last_piloting_frame = g_avatar_frame;  // grace starts at claim
            // Throttled: a ghost entry re-tracks every frame until the pump expiry drops it.
            static uint32_t s_last_track_pilot = 0;
            static uint8_t s_last_track_guid[16] = {0};
            if (slot->pilot != s_last_track_pilot ||
                memcmp(slot->guid, s_last_track_guid, 16) != 0) {
                LOG_INFO("veh: tracking pilot %08x", slot->pilot);
                s_last_track_pilot = slot->pilot;
                memcpy(s_last_track_guid, slot->guid, sizeof(s_last_track_guid));
            }
        }
        // I drive this one myself: the remote copy is bogus, never touch it.
        if (g_my_vehicle_valid && memcmp(g_my_vehicle_guid, remote[r].guid, 16) == 0) {
            release_driven(slot, "locally piloted");
            continue;
        }
        if (!slot->game_object) {
            uint32_t tech = vehicle_tech_for_class(slot->classId);
            PrefabRequest* preq =
                tech ? prefab_slot_for(tech, false) : nullptr;
            if (preq && preq->request.get() && !preq->prefab.get()) {
                // A prefab load fired earlier is still in flight: poll it every frame
                // (cheap slot lookup, no scene scan). The copy pops in on arrival.
                bool settled = spawn_driven_from_prefab(slot);
                if (slot->game_object) {
                    // spawned — fall through to the teleport below
                } else {
                    if (settled) slot->next_resolve_frame = g_avatar_frame + 900;
                    continue;
                }
            } else {
                if (g_avatar_frame < slot->next_resolve_frame) continue;
                void* go = find_object_by_guid(slot->guid);
                if (!go) {
                    slot->next_resolve_frame = g_avatar_frame + 900;  // ~30 s
                    if (!spawn_driven_from_prefab(slot) || !slot->game_object) {
                        LOG_INFO("veh: no local vehicle for pilot %08x yet (class %.15s), "
                                 "retrying",
                                 slot->pilot, slot->classId);
                        continue;
                    }
                    // spawned — fall through to the teleport below
                } else {
                    slot->game_object = go;
                    if (g_rigidbody_type && g_get_component && !g_get_component_is_string) {
                        void* body = g_get_component.call(go, g_rigidbody_type,
                                                         g_get_component.method);
                        if (body && g_rb_get_kinematic && g_rb_set_kinematic) {
                            slot->was_kinematic =
                                g_rb_get_kinematic.call(body, g_rb_get_kinematic.method);
                            slot->have_kinematic = true;
                            g_rb_set_kinematic.call(body, true, g_rb_set_kinematic.method);
                        }
                    }
                    LOG_INFO("veh: driving for pilot %08x", slot->pilot);
                }
            }
        }
        // The prefab copy's SkyApplier is verified only after activation and Start().
        verify_vehicle_sky(slot);

        // Teleport on every new sequence; the motor reads the local player, who is elsewhere.
        // Temporary Luna diagnostics, round 4: a non-finite pose (NaN from a torn read or a
        // corrupt packet) would collapse bounds/culling and blacken the screen until a valid
        // pose arrives. Drop it loudly instead of teleporting into it.
        if (remote[r].sequence != slot->last_seq && slot->game_object && g_get_go_transform &&
            g_set_position && g_set_euler) {
            if (!isfinite(remote[r].x) || !isfinite(remote[r].y) || !isfinite(remote[r].z) ||
                !isfinite(remote[r].yaw) || !isfinite(remote[r].pitch) ||
                !isfinite(remote[r].roll)) {
                static unsigned long s_last_nan_log = 0;
                if (g_avatar_frame - s_last_nan_log > 150) {
                    s_last_nan_log = g_avatar_frame;
                    LOG_WARN("veh: non-finite pose for pilot %08x seq=%u, teleport skipped",
                             slot->pilot, remote[r].sequence);
                }
            } else {
                void* tf = g_get_go_transform.call(slot->game_object, g_get_go_transform.method);
                if (tf) {
                    Vector3 p;
                    p.x = remote[r].x;
                    p.y = remote[r].y;
                    p.z = remote[r].z;
                    g_set_position.call(tf, p, g_set_position.method);
                    Vector3 e;
                    e.x = remote[r].pitch;
                    e.y = remote[r].yaw;
                    e.z = remote[r].roll;
                    g_set_euler.call(tf, e, g_set_euler.method);
                }
            }
            slot->last_seq = remote[r].sequence;
            slot->last_seq_frame = g_avatar_frame;
        }
    }

    // Release: pilot exited (flags), pilot gone, or link stale (~6 s without a sequence).
    for (size_t i = 0; i < 4; i++) {
        DrivenVehicle* slot = &g_driven[i];
        if (!slot->used) continue;
        bool pilot_here = false;
        bool piloting = false;
        for (size_t j = 0; j < peer_count; j++) {
            if (peers[j].id == slot->pilot) {
                pilot_here = true;
                if ((peers[j].flags & kFlagPiloting) != 0) piloting = true;
                break;
            }
        }
        if (!pilot_here) {
            release_driven(slot, "pilot gone");
        } else {
            if (piloting) slot->last_piloting_frame = g_avatar_frame;
            if (!piloting &&
                g_avatar_frame - slot->last_piloting_frame > 120) {  // ~4 s grace
                release_driven(slot, "exited");
            } else if (g_avatar_frame - slot->last_seq_frame > 180) {
                release_driven(slot, "stale");
            }
        }
    }
}

#include "vehicle_seat_guard.inc"

// The TechType of the item in the local player's hand, as the enum's integer value, 0 for bare
// hands. Two cheap calls, safe every frame; runs on the game thread from Player.Update.
uint32_t read_held_tech() {
    if (!g_inventory_get_held || !g_pickup_get_tech || !g_inventory_class) return 0;
    void* inventory = read_static_object_field(g_inventory_class, "main");
    if (!inventory) return 0;
    void* held = g_inventory_get_held.call(inventory, g_inventory_get_held.method);
    if (!held) return 0;
    return g_pickup_get_tech.call(held, g_pickup_get_tech.method);
}

// Finds which male_geo children and suit leaves the game lit and emits them as a bitmask
// (see the layout above). Whatever the game does for the worn suit arrives intact, so unknown
// suits cannot fall back to the wrong body. Runs at ~2 Hz from read_equipment; equipment
// changes are rare. Logs the Body tech alongside, which is how suits without a TechType entry
// get identified.
void read_suit_mask(void* equipment, uint32_t* mask, uint32_t* body_tech) {
    uint32_t tech = 0;
    if (g_equip_body_str) {
        tech = g_equip_get_tech_in_slot.call(equipment, g_equip_body_str,
                                             g_equip_get_tech_in_slot.method);
    }
    uint32_t found = 0;
    if (g_avatar_source && g_get_go_transform && g_get_child && g_get_child_count &&
        g_get_game_object && g_get_name && g_go_get_active_self) {
        void* male = find_child_by_name(g_avatar_source, "male_geo", 0);
        if (male) {
            for (int t = 0; t < 8; t++) {
                void* top = find_child_by_name(male, kEquipTops[t], 0);
                if (!top) continue;
                if (g_go_get_active_self.call(top, g_go_get_active_self.method)) {
                    found |= (1u << (uint32_t)t);
                }
                void* ttf = g_get_go_transform.call(top, g_get_go_transform.method);
                if (!ttf) continue;
                int32_t lc = g_get_child_count.call(ttf, g_get_child_count.method);
                for (int32_t li = 0; li < lc; li++) {
                    int bit = kEquipLeafBase[t] + li;
                    if (bit >= 32) continue;
                    void* ltf = g_get_child.call(ttf, li, g_get_child.method);
                    if (!ltf) continue;
                    void* lgo = g_get_game_object.call(ltf, g_get_game_object.method);
                    if (!lgo) continue;
                    if (g_go_get_active_self.call(lgo, g_go_get_active_self.method)) {
                        found |= (1u << (uint32_t)bit);
                    }
                }
            }
        }
    }
    if (tech != *body_tech || found != *mask) {
        LOG_INFO("equip local: body=%u mask=0x%x", tech, found);
    }
    *body_tech = tech;
    *mask = found;
}

// Worn equipment for the Body/Foots/Head slots. The suit travels as a male_geo children
// bitmask (see read_suit_mask); fins/head stay TechTypes for logging, the mask already covers
// their nodes. Slot reads stay per-frame, the hierarchy walk runs at ~2 Hz; equipment changes
// are discrete rare events.
void read_equipment(uint32_t* suit, uint32_t* fins, uint32_t* head) {
    static uint32_t s_suit = 0;
    static uint32_t s_fins = 0;
    static uint32_t s_head = 0;
    static uint32_t s_body_tech = 0;
    static int s_walk_div = 0;
    if (!g_inventory_get_equipment || !g_equip_get_tech_in_slot || !g_inventory_class) {
        *suit = 0;
        *fins = 0;
        *head = 0;
        return;
    }
    void* inventory = read_static_object_field(g_inventory_class, "main");
    if (!inventory) {
        *suit = 0;
        *fins = 0;
        *head = 0;
        return;
    }
    void* equipment =
        g_inventory_get_equipment.call(inventory, g_inventory_get_equipment.method);
    if (!equipment) {
        *suit = 0;
        *fins = 0;
        *head = 0;
        return;
    }
    uint32_t fins_now = 0;
    uint32_t head_now = 0;
    if (g_equip_foots_str) {
        fins_now = g_equip_get_tech_in_slot.call(equipment, g_equip_foots_str,
                                                 g_equip_get_tech_in_slot.method);
    }
    if (g_equip_head_str) {
        head_now = g_equip_get_tech_in_slot.call(equipment, g_equip_head_str,
                                                 g_equip_get_tech_in_slot.method);
    }
    if (fins_now != s_fins || head_now != s_head) {
        LOG_INFO("equip local: foots=%u head=%u", fins_now, head_now);
    }
    s_fins = fins_now;
    s_head = head_now;
    if (++s_walk_div >= 15) {
        s_walk_div = 0;
        read_suit_mask(equipment, &s_suit, &s_body_tech);
    }
    *suit = s_suit;
    *fins = s_fins;
    *head = s_head;
}

// Preloaded-prefab fallback for find_prop_source below. TechType -> classId -> scene prefab,
// all synchronous: the async loader needs managed coroutines, but the prefab database is
// preloaded at boot, so this hits for tools the receiver never possessed (crafted elsewhere,
// picked up solo, wiped by the old Destroy-era remote pickup). The prefab itself is never
// touched - only cloned from, exactly like a dormant world copy.
// Binds a fresh database request to its concrete class. TryGetPrefab/MoveNext are looked up
// on LoadedPrefabRequest or LoadingPrefabRequest, whichever the request actually is.
ManagedRoot::Handle prefab_root_create(void* object) {
    return il2cpp_gchandle_new(reinterpret_cast<Il2CppObject*>(object),false);
}
void* prefab_root_get(ManagedRoot::Handle handle) { return il2cpp_gchandle_get_target(handle); }
bool prefab_retain(ManagedRoot& root,void* object) {
    return root.retain(object,prefab_root_create,prefab_root_get,il2cpp_gchandle_free);
}
void clear_prefab_request(PrefabRequest* slot) {
    slot->request.clear();slot->prefab.clear();slot->tech=0;
    slot->try_get={};slot->move_next={};slot->iterator_done=false;slot->last_step=0;
}
void reset_prefab_requests() {
    for(auto& slot:g_prefab_requests)clear_prefab_request(&slot);
    LOG_INFO("prefab requests: cleared GC roots for world transition");
}
bool bind_prefab_request(PrefabRequest* slot, void* request) {
    if(!prefab_retain(slot->request,request))return false;
    slot->prefab.clear();
    slot->try_get = Entry<TryGetPrefabFn>();
    slot->move_next = Entry<MoveNextFn>();
    slot->iterator_done=false;slot->last_step=0;
    struct Il2CppClass* klass =
        (il2cpp_object_get_class && request)
            ? il2cpp_object_get_class(reinterpret_cast<struct Il2CppObject*>(request))
            : nullptr;
    if (klass) {
        const char* name=il2cpp_class_get_name(klass);
        if(!name||(strcmp(name,"LoadingPrefabRequest")&&strcmp(name,"LoadedPrefabRequest"))) {
            LOG_WARN("prefab request: refused unexpected type %s",name?name:"?");
            clear_prefab_request(slot);return false;
        }
        slot->try_get.bind(find_method(klass, "TryGetPrefab", 1));
        slot->move_next.bind(find_method(klass, "MoveNext", 0));
    }
    if(!slot->try_get||!slot->move_next){clear_prefab_request(slot);return false;}
    LOG_INFO("prefab request: rooted tech=%u type=%s",slot->tech,il2cpp_class_get_name(klass));
    return true;
}

PrefabRequest* prefab_slot_for(uint32_t tech, bool create) {
    PrefabRequest* free_slot = nullptr;
    PrefabRequest* completed = nullptr;
    for (size_t i = 0; i < kMaxPrefabRequests; i++) {
        if (g_prefab_requests[i].tech == tech) return &g_prefab_requests[i];
        if (!free_slot && !g_prefab_requests[i].tech) free_slot = &g_prefab_requests[i];
        if (!completed && g_prefab_requests[i].prefab.get()) completed=&g_prefab_requests[i];
    }
    if (!create) return nullptr;
    // Never discard an in-flight request merely because another TechType arrived.
    if (!free_slot) free_slot=completed;
    if (!free_slot) return nullptr;
    clear_prefab_request(free_slot);
    free_slot->tech = tech;
    return free_slot;
}

// Polls a pending database request: drives the load, returns the prefab the moment it is
// ready (and caches it in the slot for instant re-equips).
void* poll_prefab_request(uint32_t tech) {
    if(!world_operations_ready())return nullptr;
    PrefabRequest* slot = prefab_slot_for(tech, false);
    if (!slot) return nullptr;
    if (void* cached=slot->prefab.get())return cached;
    void* request=slot->request.get();
    if (!request) return nullptr;
    // Multiple consumers may ask for the same prefab in one Player.Update.
    // Advance the iterator once per frame, and never after it returned false.
    const auto frame=static_cast<unsigned long>(g_update_calls);
    if (!slot->iterator_done&&slot->move_next&&slot->last_step!=frame) {
        slot->last_step=frame;
        slot->iterator_done=!slot->move_next.call(request,slot->move_next.method);
    }
    void* prefab = nullptr;
    if (slot->try_get &&
        slot->try_get.call(request, &prefab, slot->try_get.method) && prefab) {
        if(!prefab_retain(slot->prefab,prefab))return nullptr;
        LOG_INFO("held prop: prefab for tech %u arrived (%p)", tech, prefab);
        return prefab;
    }
    if(slot->iterator_done) {
        LOG_WARN("prefab request: completed without prefab tech=%u; retry after next lookup",tech);
        clear_prefab_request(slot);
    }
    return nullptr;
}

void* find_prefab_source(uint32_t tech) {
    if (!tech||!world_operations_ready()) return nullptr;
    // A load already in flight (or finished): take the prefab when ready.
    void* prefab = poll_prefab_request(tech);
    if (prefab) return prefab;
    if (!g_craft_get_class_id || !g_prefab_get_async) return nullptr;
    PrefabRequest* slot = prefab_slot_for(tech, true);
    if (!slot || slot->request.get()) return nullptr;  // loading already, wait for the poll
    void* class_id = g_craft_get_class_id.call(tech, g_craft_get_class_id.method);
    if (!class_id) return nullptr;
    // Synchronous scene-prefab query first: free when it hits.
    if (g_craft_try_prefab) {
        void* scene_prefab = nullptr;
        if (g_craft_try_prefab.call(class_id, &scene_prefab, g_craft_try_prefab.method) &&
            scene_prefab) {
            return scene_prefab;
        }
    }
    void* request = g_prefab_get_async.call(class_id, g_prefab_get_async.method);
    if (!request) return nullptr;
    if(!bind_prefab_request(slot, request))return nullptr;
    char name[64] = {0};
    read_managed_name(class_id, name, sizeof(name));
    LOG_INFO("held prop: prefab request for tech %u ('%s') fired, polling", tech, name);
    return poll_prefab_request(tech);  // instant hit when already cached (LoadedPrefabRequest)
}

// Finds a GameObject that can serve as the clone source for one TechType. Preference order:
// a dormant world item first (the deactivated copy a remote pickup left behind - it is inactive,
// so its components are at rest and its pose is wherever the item was picked up), then an active
// world item, then an item in this phone's own inventory, and last the preloaded scene prefab,
// which covers tools that never existed in this phone's world at all.
void* find_prop_source(uint32_t tech) {
    if (!tech || !g_find_objects_all || !g_pickupable_type || !g_pickup_get_tech ||
        !g_array_get_length || !g_array_get_value || !g_get_game_object || !g_go_get_active_self) {
        return nullptr;
    }
    void* list = g_find_objects_all.call(g_pickupable_type, true, g_find_objects_all.method);
    if (!list) return nullptr;
    int32_t count = g_array_get_length.call(list, g_array_get_length.method);

    void* dormant = nullptr;
    void* world_active = nullptr;
    void* inventory_fallback = nullptr;
    for (int32_t i = 0; i < count; i++) {
        void* item = g_array_get_value.call(list, i, g_array_get_value.method);
        if (!item) continue;
        if (g_pickup_get_tech.call(item, g_pickup_get_tech.method) != tech) continue;
        void* game_object = g_get_game_object.call(item, g_get_game_object.method);
        if (!game_object) continue;
        if (!g_go_get_active_self.call(game_object, g_go_get_active_self.method)) {
            if (!dormant) dormant = game_object;
        } else if (!object_in_local_inventory(game_object)) {
            if (!world_active) world_active = game_object;
        } else if (!inventory_fallback) {
            inventory_fallback = game_object;
        }
        if (dormant) break;
    }
    if (dormant) return dormant;
    if (world_active) return world_active;
    if (inventory_fallback) return inventory_fallback;
    return find_prefab_source(tech);
}

void clear_held_body_ik(RemoteAvatar* avatar);
void destroy_held_prop(RemoteAvatar* avatar) {
    clear_held_body_ik(avatar);
    if (avatar->held_prop && g_destroy) {
        g_destroy.call(avatar->held_prop, g_destroy.method);
    }
    avatar->held_prop = nullptr;
    avatar->held_lights_parent=nullptr;
    avatar->held_light_count=0;
    avatar->held_body_pose=false;
    avatar->held_arms=avatar->held_left_ik=avatar->held_right_ik=nullptr;
    avatar->held_plug_origin=nullptr;
    avatar->held_native_grip=false;
    avatar->held_grip_socket=nullptr;
    avatar->held_grip_offset=Vector3{};
    avatar->held_palm_local_valid=false;
    avatar->held_received_rotation=false;
}

Vector3 vec_sub(const Vector3& a, const Vector3& b) {
    Vector3 out;
    out.x = a.x - b.x;
    out.y = a.y - b.y;
    out.z = a.z - b.z;
    return out;
}

// Strips a fresh prop down to a pure visual. Every collider in its subtree goes off (only the
// root rigidbody was neutralised before, and a MeshCollider on a child still shoved the clone),
// and the whole subtree moves onto the Default layer (inventory-sourced viewmodel copies render
// on a layer the main camera never draws, which is why those props existed but stayed
// invisible). Runs while the prop is parked inactive, so nothing it carries can fire.
void sanitize_held_prop(void* prop, bool keep_prefab_layers) {
    if (!prop || !g_get_go_transform || !g_get_child || !g_get_child_count ||
        !g_get_game_object) {
        return;
    }
    // The Type overload of GetComponent is confirmed in use (see the getcomp candidate log),
    // so the base Collider type matches every collider kind.
    void* collider_arg =
        g_get_component_is_string ? nullptr : g_collider_type_obj;
    // Every rigidbody in the subtree goes kinematic with no collider contacts. Neutralising
    // only the root left child rigidbodies simulating: on compound prefabs (Seaglide!) physics
    // kept integrating the parts in world space and tore the prop off the hand while the
    // avatar swam away.
    bool do_bodies = g_rigidbody_type && g_get_component && !g_get_component_is_string;
    void* stack[64];
    size_t top = 0;
    stack[top++] = prop;
    size_t visited = 0;
    while (top > 0 && visited < 64) {
        void* node = stack[--top];
        visited++;
        if (collider_arg && g_get_component && g_behaviour_set_enabled) {
            void* collider = g_get_component.call(node, collider_arg, g_get_component.method);
            if (collider) {
                g_behaviour_set_enabled.call(collider, false, g_behaviour_set_enabled.method);
            }
        }
        if (do_bodies) {
            void* body = g_get_component.call(node, g_rigidbody_type, g_get_component.method);
            if (body) {
                if (g_rb_set_kinematic) {
                    g_rb_set_kinematic.call(body, true, g_rb_set_kinematic.method);
                }
                if (g_rb_set_detect_colliders) {
                    g_rb_set_detect_colliders.call(body, false,
                                                   g_rb_set_detect_colliders.method);
                }
            }
        }
        // Layers: held props join the avatar's hand (Default). Vehicle copies keep the
        // prefab's layers instead: the real sub lives on layer 17, and forcing Default
        // takes the copy out of the lights' culling masks - a black silhouette in daylight.
        if (!keep_prefab_layers && g_go_set_layer) {
            g_go_set_layer.call(node, 0, g_go_set_layer.method);
        }
        void* tf = g_get_go_transform.call(node, g_get_go_transform.method);
        if (!tf) continue;
        int32_t count = g_get_child_count.call(tf, g_get_child_count.method);
        for (int32_t i = 0; i < count && top < 64; i++) {
            void* child_tf = g_get_child.call(tf, i, g_get_child.method);
            if (!child_tf) continue;
            void* child_go = g_get_game_object.call(child_tf, g_get_game_object.method);
            if (child_go) stack[top++] = child_go;
        }
    }
}

// ---------------------------------------------------------------------------
// Per-tool grip corrections (item sync v3, polish stage)
//
// Some prefabs carry their mesh far from the root origin: snapping the root onto the hand
// then parks the visible tool beside the palm instead of in it (Seaglide, StasisRifle,
// Welder, PropulsionCannon). The correction below is applied in hand space after the snap,
// identically on the spawn path and the per-frame follow so the two never disagree. Values
// are tuned from screenshots; entries stay identity until measured - a wrong offset looks
// worse than none.
// ---------------------------------------------------------------------------
struct GripOffset {
    uint32_t tech;
    float px, py, pz;  // meters, hand space
    float rx, ry, rz;  // degrees, hand space (XYZ order)
};

static const GripOffset kGripTable[] = {
    {751, 0, 0, 0, 0, 0, 0},  // Seaglide
    {755, 0, 0, 0, 0, 0, 0},  // StasisRifle
    {750, 0, 0, 0, 0, 0, 0},  // Welder
    {757, 0, 0, 0, 0, 0, 0},  // PropulsionCannon
};

const GripOffset* grip_for_tech(uint32_t tech) {
    for (size_t i = 0; i < sizeof(kGripTable) / sizeof(kGripTable[0]); i++) {
        if (kGripTable[i].tech == tech) return &kGripTable[i];
    }
    return nullptr;
}

Quaternion quat_mul(const Quaternion& a, const Quaternion& b) {
    Quaternion q;
    q.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    q.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    q.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    q.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return q;
}

// Rotates v by unit quaternion q (q * v * q^-1 expanded).
Vector3 quat_rot(const Quaternion& q, const Vector3& v) {
    float tx = 2.0f * (q.y * v.z - q.z * v.y);
    float ty = 2.0f * (q.z * v.x - q.x * v.z);
    float tz = 2.0f * (q.x * v.y - q.y * v.x);
    Vector3 out;
    out.x = v.x + q.w * tx + (q.y * tz - q.z * ty);
    out.y = v.y + q.w * ty + (q.z * tx - q.x * tz);
    out.z = v.z + q.w * tz + (q.x * ty - q.y * tx);
    return out;
}

Quaternion euler_xyz_deg(float rx, float ry, float rz) {
    constexpr float kD = 0.01745329252f;
    float cx = cosf(rx * 0.5f * kD);
    float sx = sinf(rx * 0.5f * kD);
    float cy = cosf(ry * 0.5f * kD);
    float sy = sinf(ry * 0.5f * kD);
    float cz = cosf(rz * 0.5f * kD);
    float sz = sinf(rz * 0.5f * kD);
    Quaternion qx;
    qx.x = sx;
    qx.y = 0.0f;
    qx.z = 0.0f;
    qx.w = cx;
    Quaternion qy;
    qy.x = 0.0f;
    qy.y = sy;
    qy.z = 0.0f;
    qy.w = cy;
    Quaternion qz;
    qz.x = 0.0f;
    qz.y = 0.0f;
    qz.z = sz;
    qz.w = cz;
    return quat_mul(quat_mul(qx, qy), qz);
}

Vector3 vec_add(const Vector3& a, const Vector3& b) {
    Vector3 out;
    out.x = a.x + b.x;
    out.y = a.y + b.y;
    out.z = a.z + b.z;
    return out;
}

// Snaps the prop onto the hand anchor plus the per-tech grip correction. Shared by spawn and
// follow.
void snap_prop_to_hand(void* prop_tf, void* hand_tf, uint32_t tech) {
    if (!prop_tf || !hand_tf || !g_get_position || !g_set_position) return;
    Vector3 anchor = g_get_position.call(hand_tf, g_get_position.method);
    Quaternion anchor_rot;
    anchor_rot.x = 0.0f;
    anchor_rot.y = 0.0f;
    anchor_rot.z = 0.0f;
    anchor_rot.w = 1.0f;
    if (g_get_rotation) {
        anchor_rot = g_get_rotation.call(hand_tf, g_get_rotation.method);
    }
    Vector3 target = anchor;
    Quaternion rot = anchor_rot;
    const GripOffset* grip = grip_for_tech(tech);
    if (grip) {
        Vector3 off;
        off.x = grip->px;
        off.y = grip->py;
        off.z = grip->pz;
        target = vec_add(anchor, quat_rot(anchor_rot, off));
        rot = quat_mul(anchor_rot, euler_xyz_deg(grip->rx, grip->ry, grip->rz));
    }
    g_set_position.call(prop_tf, target, g_set_position.method);
    if (g_set_rotation) {
        g_set_rotation.call(prop_tf, rot, g_set_rotation.method);
    }
}

#include "held_grip.inc"
#include "held_mesh_hand.inc"
#include "held_equipped_model.inc"
#include "held_palm.inc"
#include "held_rotation_apply.inc"
#include "held_light.inc"
#include "held_body_pose.inc"

// Camera-mounted tools define the wrist pose with rightHandIKTarget. Hand-mounted
// tools instead use plugOrigin and the palm socket. Keep both clone-owned anchors.
void cache_avatar_hand(RemoteAvatar* avatar) {
    avatar->held_hand = avatar->transform;
    avatar->held_hand_bone = nullptr;
    void* arms = avatar->game_object ? find_component_in_hierarchy(
        avatar->game_object,g_arms_type_obj,g_arms_name_str,0) : nullptr;
    void* bone=nullptr;
    void* attach=nullptr;
    if (arms && il2cpp_field_get_value) {
        if (g_arms_hand_field) il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(arms),g_arms_hand_field,&bone);
        if (g_arms_attach_field) il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(arms),g_arms_attach_field,&attach);
    }
    void* visual_bone=find_visual_right_hand(avatar->game_object,avatar->transform,bone);
    avatar->held_hand_bone=visual_bone;
    avatar->held_controller_socket=prop_owns_transform(avatar->transform,attach) ? attach : nullptr;
    avatar->held_hand=visual_hand_anchor(avatar->transform,visual_bone,attach);
    char name[128]{};
    if (visual_bone) {
        void* go=g_get_game_object.call(visual_bone,g_get_game_object.method);
        if (go) read_managed_name(g_get_name.call(go,g_get_name.method),name,sizeof(name));
    }
    LOG_INFO("held hand: avatar %08x socket=%p meshBone=%p name=%s controllerBone=%p controllerAttach=%p root=%p",
        avatar->id,avatar->held_hand,visual_bone,name,bone,attach,avatar->transform);
}

bool place_avatar_prop(RemoteAvatar* avatar,void* prop_tf,void* grip) {
    void* wrist=avatar->held_grip_socket;
    if (!avatar->held_use_palm) {
        bool aligned=plug_visual_to_hand(prop_tf,grip,wrist);
        avatar->held_palm_local=Vector3{}; // authored IK target is the wrist
        avatar->held_palm_local_valid=aligned;
        avatar->held_received_rotation=false;
        return aligned;
    }
    Vector3 palm=g_get_position.call(wrist,g_get_position.method);
    unsigned samples=0;
    bool measured=visual_palm_center(avatar->game_object,wrist,&palm,&samples);
    bool aligned=plug_visual_to_palm(prop_tf,grip,wrist,avatar->held_controller_socket,palm,avatar->held_grip_offset);
    avatar->held_palm_local_valid=aligned && bool(g_held_inverse_point);
    avatar->held_received_rotation=false;
    if (avatar->held_palm_local_valid)
        avatar->held_palm_local=g_held_inverse_point.call(wrist,palm,g_held_inverse_point.method);
    LOG_INFO("held palm: peer=%08x tech=%u measured=%u fingers=%u point=(%.3f,%.3f,%.3f) aligned=%u",
        avatar->id,avatar->held_tech,unsigned(measured),samples,palm.x,palm.y,palm.z,unsigned(aligned));
    return aligned;
}

void read_local_held_rotation(void* player,LocalState* local) {
    local->held_rotation_valid=0;
    local->flags&=~kFlagHeldWorldPose;
    local->held_offset={};
    if (!player || !local->held_tech || !g_get_rotation || !g_get_position || !g_get_go_transform ||
        !g_get_game_object || !g_inventory_get_held || !g_pickup_get_tech ||
        !g_inventory_class || !il2cpp_field_get_value) return;
    void* inventory=read_static_object_field(g_inventory_class,"main");
    void* held=inventory ? g_inventory_get_held.call(inventory,g_inventory_get_held.method) : nullptr;
    if (!held || g_pickup_get_tech.call(held,g_pickup_get_tech.method)!=local->held_tech) return;
    void* tool_go=g_get_game_object.call(held,g_get_game_object.method);
    void* tool=tool_go ? g_get_go_transform.call(tool_go,g_get_go_transform.method) : nullptr;
    if (!tool) return;
    if (local->held_tech==751 && local->valid) {
        auto p=g_get_position.call(tool,g_get_position.method);
        auto q=g_get_rotation.call(tool,g_get_rotation.method);
        local->held_offset={p.x-local->x,p.y-local->y,p.z-local->z};
        local->held_rotation_valid=held_offset_valid(local->held_offset) &&
            held_rotation_normalize({q.x,q.y,q.z,q.w},&local->held_rotation);
        if (local->held_rotation_valid) local->flags|=kFlagHeldWorldPose;
        return;
    }
    if (!g_arms_hand_field || !g_arms_type_obj) return;
    void* player_go=g_get_game_object.call(player,g_get_game_object.method);
    void* root=player_go ? g_get_go_transform.call(player_go,g_get_go_transform.method) : nullptr;
    if (!root) return;
    void* arms=find_component_in_hierarchy(player_go,g_arms_type_obj,g_arms_name_str,0);
    void* wrist=nullptr;
    if (arms) il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(arms),g_arms_hand_field,&wrist);
    if (!prop_owns_transform(root,wrist)) return;
    Quaternion hand_q=g_get_rotation.call(wrist,g_get_rotation.method);
    Quaternion tool_q=g_get_rotation.call(tool,g_get_rotation.method);
    local->held_rotation_valid=held_rotation_relative({hand_q.x,hand_q.y,hand_q.z,hand_q.w},
        {tool_q.x,tool_q.y,tool_q.z,tool_q.w},&local->held_rotation);
    if ((g_avatar_frame%120)==0 && local->held_rotation_valid)
        LOG_INFO("held rotation tx: tech=%u q=(%.4f,%.4f,%.4f,%.4f)",local->held_tech,
            local->held_rotation.x,local->held_rotation.y,local->held_rotation.z,local->held_rotation.w);
}

void apply_peer_held_rotation(RemoteAvatar* avatar,const PeerState& peer) {
    if (avatar->held_prop && avatar->held_body_pose) {apply_held_body_pose(avatar,peer);return;}
    if (peer.held_tech==751 && (peer.flags&kFlagHeldWorldPose)) return;
    if (!avatar->held_prop || !avatar->held_native_grip ||
        !avatar->held_palm_local_valid || !peer.held_rotation_valid || peer.held_tech!=avatar->held_tech) return;
    HeldRotation q{};
    if (!held_rotation_normalize(peer.held_rotation,&q)) return;
    auto old=avatar->held_last_rotation;
    float dot=q.x*old.x+q.y*old.y+q.z*old.z+q.w*old.w;
    bool changed=!avatar->held_received_rotation || std::fabs(dot)<0.999999f;
    if (changed) {
        void* model=g_get_go_transform.call(avatar->held_prop,g_get_go_transform.method);
        if (!apply_held_rotation_pose(model,avatar->held_plug_origin,avatar->held_grip_socket,
            avatar->held_palm_local,avatar->held_grip_offset,q)) return;
        avatar->held_last_rotation=q;
        avatar->held_received_rotation=true;
    }
    if ((g_avatar_frame%120)==0)
        LOG_INFO("held rotation rx: peer=%08x tech=%u applied=1 q=(%.4f,%.4f,%.4f,%.4f)",
            avatar->id,avatar->held_tech,q.x,q.y,q.z,q.w);
}

// Report real mesh geometry separately from beam/particle bounds. This is
// diagnostic only: bounds never choose or continuously move the grip.
void report_held_visual(RemoteAvatar* avatar) {
    if (!avatar->held_prop || !g_held_renderer_bounds || !g_renderer_type_obj ||
        g_get_component_is_string) return;
    void* stack[128]; size_t top=0,visited=0; stack[top++]=avatar->held_prop;
    while (top && visited++<128) {
        void* go=stack[--top];
        void* renderer=g_get_component.call(go,g_renderer_type_obj,g_get_component.method);
        if (renderer) {
            auto* klass=il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(renderer));
            const char* kind=klass ? il2cpp_class_get_name(klass) : nullptr;
            if (kind && (strcmp(kind,"MeshRenderer")==0 || strcmp(kind,"SkinnedMeshRenderer")==0)) {
                Bounds b=g_held_renderer_bounds.call(renderer,g_held_renderer_bounds.method);
                char name[128]{}; read_managed_name(g_get_name.call(go,g_get_name.method),name,sizeof(name));
                bool active=g_go_get_active_hierarchy && g_go_get_active_hierarchy.call(go,g_go_get_active_hierarchy.method);
                Vector3 hand=g_get_position.call(avatar->held_grip_socket,g_get_position.method);
                LOG_INFO("held mesh: peer=%08x tech=%u name=%s kind=%s active=%u center=(%.3f,%.3f,%.3f) size=(%.3f,%.3f,%.3f) hand=(%.3f,%.3f,%.3f)",
                    avatar->id,avatar->held_tech,name,kind,unsigned(active),b.center.x,b.center.y,b.center.z,
                    2*b.extents.x,2*b.extents.y,2*b.extents.z,hand.x,hand.y,hand.z);
            }
        }
        void* tf=g_get_go_transform.call(go,g_get_go_transform.method);
        int32_t n=tf ? g_get_child_count.call(tf,g_get_child_count.method) : 0;
        for (int32_t i=0;i<n && top<128;++i) {
            void* child=g_get_child.call(tf,i,g_get_child.method);
            void* child_go=child ? g_get_game_object.call(child,g_get_game_object.method) : nullptr;
            if (child_go) stack[top++]=child_go;
        }
    }
}

// Authored grips follow the hand through Unity's hierarchy, including Animator/IK
// updates later in the frame. Root fallback is only for props without an owned grip.
void follow_avatar_held_prop(RemoteAvatar* avatar) {
    if (avatar->held_body_pose) return;
    if (!avatar->held_prop || !g_get_go_transform || !g_get_position || !g_set_position) return;
    void* hand_tf = avatar->held_grip_socket ? avatar->held_grip_socket : avatar->held_hand;
    if (!hand_tf) return;
    void* prop_tf = g_get_go_transform.call(avatar->held_prop, g_get_go_transform.method);
    if (!prop_tf) return;
    if (avatar->held_native_grip && g_transform_get_parent.call(prop_tf,g_transform_get_parent.method)!=hand_tf) {
        avatar->held_native_grip=place_avatar_prop(avatar,prop_tf,avatar->held_plug_origin);
    }
    if (!avatar->held_native_grip) {
        snap_prop_to_hand(prop_tf, hand_tf, avatar->held_tech);
    }
    // Propwatch: avatar root vs hand anchor vs prop, plus whether the prop is really parented
    // under the hand. Answers "stuck prop" vs "stale world copy" from the log alone.
    if ((g_avatar_frame % 120) == 0 && avatar->transform && g_transform_get_parent) {
        if (avatar->held_hand_bone && avatar->held_controller_socket) {
            Vector3 wrist=g_get_position.call(avatar->held_hand_bone,g_get_position.method);
            Vector3 socket=g_get_position.call(avatar->held_controller_socket,g_get_position.method);
            Vector3 delta=vec_sub(socket,wrist);
            LOG_INFO("held anchors: peer=%08x wrist=(%.3f,%.3f,%.3f) controllerSocket=(%.3f,%.3f,%.3f) distance=%.4f",
                avatar->id,wrist.x,wrist.y,wrist.z,socket.x,socket.y,socket.z,
                sqrtf(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z));
        }
        if (!avatar->held_visual_reported && avatar->held_grip_socket) {
            report_held_visual(avatar); avatar->held_visual_reported=true;
        }
        Vector3 ap = g_get_position.call(avatar->transform, g_get_position.method);
        Vector3 hp = g_get_position.call(hand_tf, g_get_position.method);
        Vector3 pp = g_get_position.call(prop_tf, g_get_position.method);
        void* parent = g_transform_get_parent.call(prop_tf, g_transform_get_parent.method);
        LOG_INFO("propwatch: avatar %08x root=(%.1f,%.1f,%.1f) hand=(%.1f,%.1f,%.1f) "
                 "prop=(%.1f,%.1f,%.1f) parent_is_hand=%d",
                 avatar->id, ap.x, ap.y, ap.z, hp.x, hp.y, hp.z, pp.x, pp.y, pp.z,
                 (parent && parent == hand_tf) ? 1 : 0);
        if (avatar->held_native_grip && avatar->held_plug_origin) {
            Vector3 grip=visual_grip_position(prop_tf,avatar->held_plug_origin,avatar->held_grip_offset);
            const Vector3 error=vec_sub(grip,hp);
            LOG_INFO("held grip: peer=%08x tech=%u native=1 wrist_distance=%.4f",
                avatar->id,avatar->held_tech,sqrtf(error.x*error.x+error.y*error.y+error.z*error.z));
        }
    }
}

// Instantiates the prop for one avatar. True when it is in place, false when no source exists
// yet (the caller retries on a cooldown) or the API is incomplete.
bool spawn_held_prop(RemoteAvatar* avatar, uint32_t tech) {
    if (!g_instantiate || !g_set_active || !g_object_destroy || !g_get_go_transform ||
        !g_set_position || !g_get_component) {
        return true;  // nothing will ever make this work; do not retry forever
    }
    void* source = find_prop_source(tech);
    if (!source) return false;

    Vector3 position{0.0f, 0.0f, 0.0f};
    Quaternion rotation{0.0f, 0.0f, 0.0f, 1.0f};
    void* prop = g_instantiate.call(source, position, rotation, g_instantiate.method);
    if (!prop) return false;
    // Park it inactive first. The clone inherits the source's active state, so an active source
    // used to yield a live prop: Awake/OnEnable ran on the not-yet-stripped copy and the tool
    // behaviour fired for a frame - the extra "throw" the holder's model seemed to do. The
    // strip-down below now happens with every component at rest, before any Awake runs.
    g_set_active.call(prop, false, g_set_active.method);
    void* prop_tf=g_get_go_transform.call(prop,g_get_go_transform.method);
    void* fpmodel=find_component_in_hierarchy(prop,g_fpmodel_type,nullptr,0);
    void* world_model=nullptr; void* view_model=nullptr;
    bool equipped_visual=false;
    if (fpmodel && g_fpmodel_world_field && g_fpmodel_view_field) {
        auto* object=reinterpret_cast<Il2CppObject*>(fpmodel);
        il2cpp_field_get_value(object,g_fpmodel_world_field,&world_model);
        il2cpp_field_get_value(object,g_fpmodel_view_field,&view_model);
        equipped_visual=set_equipped_visual(prop_tf,world_model,view_model);
    }
    LOG_INFO("held model: peer=%08x tech=%u fp=%p world=%p view=%p equipped=%u",
        avatar->id,tech,fpmodel,world_model,view_model,unsigned(equipped_visual));
    if (fpmodel && equipped_visual) {
        if (g_object_destroy_immediate) g_object_destroy_immediate.call(fpmodel,g_object_destroy_immediate.method);
        else g_object_destroy.call(fpmodel,g_object_destroy.method);
    }
    // Equipped FP models can carry the source builder's last beam state. The
    // inert remote visual must not inherit an always-on construction effect.
    if (tech==509) {
        for (const char* name : {"beamLeft","beamRight"}) {
            void* beam=find_child_by_name(prop,name,0);
            if (beam) g_set_active.call(beam,false,g_set_active.method);
        }
    }
    avatar->held_visual_reported=false;
    void* plug=nullptr;
    void* right_ik=nullptr;
    void* left_ik=nullptr;
    int32_t socket=-1;
    // Read the authored references before removing either gameplay component.
    void* tool=find_component_in_hierarchy(prop,g_playertool_type_obj,g_playertool_name_str,0);
    if (tool && il2cpp_field_get_value) {
        auto* object=reinterpret_cast<Il2CppObject*>(tool);
        if (g_plug_origin_field) il2cpp_field_get_value(object,g_plug_origin_field,&plug);
        if (g_tool_right_ik_field) il2cpp_field_get_value(object,g_tool_right_ik_field,&right_ik);
        if (g_tool_left_ik_field) il2cpp_field_get_value(object,g_tool_left_ik_field,&left_ik);
        if (g_tool_socket_field) il2cpp_field_get_value(object,g_tool_socket_field,&socket);
    }
    void* grip=select_visual_grip(prop_tf,plug,right_ik,socket,tech);
    bool wrist_grip=(right_ik && grip==right_ik && prop_owns_transform(prop_tf,right_ik));
    LOG_INFO("held source: peer=%08x tech=%u tool=%p socket=%d plug=%p rightIK=%p selected=%p wrist=%u",
        avatar->id,tech,tool,socket,plug,right_ik,grip,unsigned(wrist_grip));
    bool controlled_light=prepare_remote_held_light(avatar,prop,tech);
    avatar->held_body_pose=prepare_held_body_pose(avatar,prop_tf,left_ik,right_ik,tech);
    if (g_pickupable_type) {
        void* pickupable=g_get_component.call(prop,g_pickupable_type,g_get_component.method);
        if (pickupable && g_object_destroy_immediate) g_object_destroy_immediate.call(pickupable,g_object_destroy_immediate.method);
        else if (pickupable) g_object_destroy.call(pickupable,g_object_destroy.method);
    }
    if (tool && g_object_destroy_immediate) g_object_destroy_immediate.call(tool,g_object_destroy_immediate.method);
    else if (tool) g_object_destroy.call(tool,g_object_destroy.method);
    // Lights die too: a flashlight/seaglide prop would otherwise carry a live spotlight into
    // the clone's hand. Bounded loop because a deferred Destroy would return the same light.
    if (!controlled_light && g_light_type_obj && g_get_component && !g_get_component_is_string) {
        for (int k = 0; k < 8; k++) {
            void* lamp = find_component_in_hierarchy(prop, g_light_type_obj, nullptr, 0);
            if (!lamp) break;
            if (g_object_destroy_immediate) {
                g_object_destroy_immediate.call(lamp, g_object_destroy_immediate.method);
            } else if (g_object_destroy) {
                g_object_destroy.call(lamp, g_object_destroy.method);
            } else {
                break;
            }
        }
    }
    // Colliders off, every rigidbody kinematic, layer to Default, while still parked
    // inactive (see sanitize_held_prop).
    sanitize_held_prop(prop, false);

    // Parent onto the cached hand anchor (right hand when the clone has one, otherwise the
    // body root). The prop is placed in world space afterwards because the source's local
    // transform was relative to wherever it lay in the world, not relative to a hand. The
    // per-frame follow re-asserts this every frame, so even a missed SetParent stays glued.
    void* parent_tf = avatar->held_body_pose ? avatar->transform : wrist_grip && avatar->held_hand_bone ? avatar->held_hand_bone : avatar->held_hand;
    avatar->held_grip_socket=parent_tf;
    if (prop_tf && parent_tf && g_set_parent) {
        g_set_parent.call(prop_tf, parent_tf, false, g_set_parent.method);
    }
    if (prop_tf && parent_tf && !avatar->held_body_pose) {
        avatar->held_use_palm=(!wrist_grip && socket==0 && avatar->held_hand_bone==parent_tf);
        // Builder FP root lies inside its wide housing, not inside the handle.
        // Rest-skinned stock asset places the narrow handle at +X, 6-15 cm.
        // Apply this measured point only to that viewmodel without an authored plug.
        avatar->held_grip_offset=Vector3{};
        if (tech==509 && avatar->held_use_palm && equipped_visual && grip==prop_tf &&
            view_model && name_contains(view_model,"builder_FP")) {
            avatar->held_grip_offset=Vector3{0.10f,0.0f,0.0f};
        }
        LOG_INFO("held handle: peer=%08x tech=%u local=(%.3f,%.3f,%.3f) transformPoint=%p",
            avatar->id,tech,avatar->held_grip_offset.x,avatar->held_grip_offset.y,
            avatar->held_grip_offset.z,(void*)g_held_transform_point.call);
        avatar->held_native_grip=place_avatar_prop(avatar,prop_tf,grip);
        avatar->held_plug_origin=avatar->held_native_grip ? grip : nullptr;
        if (!avatar->held_native_grip) snap_prop_to_hand(prop_tf,parent_tf,tech);
        LOG_INFO("held grip: peer=%08x tech=%u native=%u plug=%p",avatar->id,tech,
                 unsigned(avatar->held_native_grip),avatar->held_plug_origin);
    }
    g_set_active.call(prop, !avatar->held_body_pose, g_set_active.method);
    avatar->held_prop = prop;
    LOG_INFO("held prop: peer %08x now carries tech %u (source=%p prop=%p parent=%p)", avatar->id,
             tech, source, prop, parent_tf);
    return true;
}

// Keeps the avatar's prop in step with the peer's held_tech. Called every frame; the actual work
// happens only on a debounced change, and a TechType with no scene source is remembered so the
// expensive full-Pickupable scan does not run every second for it.
constexpr unsigned long kHeldShowFrames = 5;    // ~0.2 s of agreement before the prop appears
constexpr unsigned long kHeldHideFrames = 15;   // ~0.5 s of empty hands before it goes away
constexpr unsigned long kHeldNoSourceFrames = 300;  // ~10 s before a missing type rescans
void update_avatar_held_prop(RemoteAvatar* avatar, uint32_t tech) {
    if (tech != avatar->held_pending) {
        avatar->held_pending = tech;
        avatar->held_stable = 0;
    } else if (avatar->held_stable < 1000000ul) {
        avatar->held_stable++;
    }
    uint32_t want = avatar->held_tech;
    if (avatar->held_pending != avatar->held_tech) {
        unsigned long need = avatar->held_pending ? kHeldShowFrames : kHeldHideFrames;
        if (avatar->held_stable >= need) want = avatar->held_pending;
    }
    if (want == avatar->held_tech && avatar->held_prop) return;
    if (want == avatar->held_tech && !want) return;
    if (want == avatar->held_tech && g_avatar_frame < avatar->held_retry) return;
    if (want && want == avatar->held_nosource && g_avatar_frame < avatar->held_nosource_until) {
        // Cheap fast path: poll the pending database load every frame (no scan, no log spam).
        // The prop pops in the moment the bundle arrives instead of at the next cooldown.
        if (!poll_prefab_request(want)) return;
        avatar->held_nosource = 0;
    }
    avatar->held_tech = want;
    avatar->held_nosource = 0;
    destroy_held_prop(avatar);
    if (!want) return;
    if (!spawn_held_prop(avatar, want)) {
        avatar->held_retry = g_avatar_frame + 30;
        avatar->held_nosource = want;
        avatar->held_nosource_until = g_avatar_frame + kHeldNoSourceFrames;
        LOG_INFO("held prop: no source object for tech %u yet, will retry", want);
    }
}

void resolve_item_api() {
    struct Il2CppClass* pickupable = find_class_in_domain("", "Pickupable");
    g_unique_identifier_class = find_class_in_domain("", "UniqueIdentifier");
    if (!pickupable || !g_unique_identifier_class) {
        LOG_WARN("item api: Pickupable=%p UniqueIdentifier=%p, item sync disabled",
                 (void*)pickupable, (void*)g_unique_identifier_class);
        return;
    }

    g_unique_get_id.bind(find_method(g_unique_identifier_class, "get_Id", 0));
    g_unique_get_class_id.bind(find_method(g_unique_identifier_class, "get_ClassId", 0));
    g_pickup_get_attached.bind(find_method(pickupable, "get_attached", 0));
    // Pickupable.GetTechType is how the held-item sync reads what the quickslot hand carries.
    g_pickup_get_tech.bind(find_method(pickupable, "GetTechType", 0));
    g_pickupable_class = pickupable;
    // Drop() has two overloads and the parameterless one is what the game calls from the
    // inventory; the three-argument version is bound as well because it is the only one that
    // accepts a position, and a lookup by argument count alone cannot tell them apart reliably.
    g_pickup_drop_at.bind(find_method(pickupable, "Drop", 3));
    g_pickup_set_visible.bind(find_method(pickupable, "SetVisible", 1));

    if (il2cpp_class_get_type && il2cpp_type_get_object) {
        g_unique_identifier_type =
            il2cpp_type_get_object(il2cpp_class_get_type(g_unique_identifier_class));
        g_pickupable_type = il2cpp_type_get_object(il2cpp_class_get_type(pickupable));
        // Unity 6 renamed Rigidbody.velocity to linearVelocity. Both are looked up and the one
        // that binds is used, because a missed velocity means every thrown object drops from
        // where it was let go.
        struct Il2CppClass* rigidbody = find_class_in_domain("UnityEngine", "Rigidbody");
        if (rigidbody) {
            g_rigidbody_type = il2cpp_type_get_object(il2cpp_class_get_type(rigidbody));
            g_rb_get_velocity.bind(find_method(rigidbody, "get_linearVelocity", 0));
            if (!g_rb_get_velocity.call) {
                g_rb_get_velocity.bind(find_method(rigidbody, "get_velocity", 0));
            }
            // Held-item prop physics: a copy that keeps the source's rigidbody settings would
            // fall through the hand or push the clone around.
            g_rb_set_kinematic.bind(find_method(rigidbody, "set_isKinematic", 1));
            g_rb_set_detect_colliders.bind(find_method(rigidbody, "set_detectColliders", 1));
            // Vehicle sync parks remote-driven subs kinematic and restores the flag on release.
            g_rb_get_kinematic.bind(find_method(rigidbody, "get_isKinematic", 0));
        }
    }

    // Transform.SetParent(parent, bool) puts the prop under the avatar's hand.
    struct Il2CppClass* prop_transform = find_class_in_domain("UnityEngine", "Transform");
    if (prop_transform) {
        g_set_parent.bind(find_method(prop_transform, "SetParent", 2));
    }
    // PlayerTool is the base of every held tool; destroying that component on the prop removes
    // the tool behaviour without touching the renderers.
    struct Il2CppClass* playertool = find_class_in_domain("", "PlayerTool");
    if (playertool && il2cpp_class_get_type && il2cpp_type_get_object) {
        g_playertool_type_obj = il2cpp_type_get_object(il2cpp_class_get_type(playertool));
    }
    g_playertool_name_str = cached_managed_string("PlayerTool");
    auto* model_plug=find_class_in_domain("","ModelPlug");
    g_plug_origin_field=build_field_on(model_plug,"plugOrigin",0x20);
    g_plug_visual.bind(find_method(model_plug,"PlugIntoSocket",3));
    auto* fpmodel=find_class_in_domain("","FPModel");
    if (fpmodel) g_fpmodel_type=il2cpp_type_get_object(il2cpp_class_get_type(fpmodel));
    g_fpmodel_world_field=build_field_on(fpmodel,"propModel",0x20);
    g_fpmodel_view_field=build_field_on(fpmodel,"viewModel",0x28);
    g_tool_socket_field=build_field_on(playertool,"socket",0x84);
    g_tool_right_ik_field=build_field_on(playertool,"rightHandIKTarget",0x90);
    g_tool_left_ik_field=build_field_on(playertool,"leftHandIKTarget",0xA0);
    auto* arms_class=find_class_in_domain("","ArmsController");
    g_arms_hand_field=build_field_on(arms_class,"rightHand",0x80);
    g_arms_attach_field=build_field_on(arms_class,"rightHandAttach",0x70);
    // UnityEngine.Collider for the held v3 subtree walk: GetComponent(Type) with the base type
    // matches BoxCollider, MeshCollider and friends, which the string overload would miss.
    struct Il2CppClass* collider = find_class_in_domain("UnityEngine", "Collider");
    if (collider && il2cpp_class_get_type && il2cpp_type_get_object) {
        g_collider_type_obj = il2cpp_type_get_object(il2cpp_class_get_type(collider));
    }
    struct Il2CppClass* renderer = find_class_in_domain("UnityEngine", "Renderer");
    if (renderer) {
        g_held_renderer_bounds.bind(find_method(renderer,"get_bounds",0));
        g_rend_get_shared_mats.bind(find_method(renderer, "get_sharedMaterials", 0));
        g_rend_get_enabled.bind(find_method(renderer, "get_enabled", 0));
    }
    // Temporary Luna diagnostics: material audit needs Material/Shader getters.
    struct Il2CppClass* audit_material = find_class_in_domain("UnityEngine", "Material");
    if (audit_material) {
        g_mat_get_shader.bind(find_method(audit_material, "get_shader", 0));
        g_mat_get_main_tex.bind(find_method(audit_material, "get_mainTexture", 0));
        g_mat_get_texture.bind(
            find_method_typed(audit_material, "GetTexture", 1, "system.string"));
        g_mat_get_color.bind(find_method(audit_material, "get_color", 0));
        g_mat_get_tex_props.bind(find_method(audit_material, "GetTexturePropertyNames", 0));
    }
    g_bump_name_str = cached_managed_string("_BumpMap");
    struct Il2CppClass* light = find_class_in_domain("UnityEngine", "Light");
    if (light && il2cpp_class_get_type && il2cpp_type_get_object) {
        g_light_type_obj = il2cpp_type_get_object(il2cpp_class_get_type(light));
    }
    auto* toggle_lights=find_class_in_domain("","ToggleLights");
    if (toggle_lights && il2cpp_class_get_type && il2cpp_type_get_object)
        g_toggle_lights_type=il2cpp_type_get_object(il2cpp_class_get_type(toggle_lights));
    g_toggle_lights_parent=build_field_on(toggle_lights,"lightsParent",0x20);
    g_toggle_lights_active=build_field_on(toggle_lights,"lightsActive",0x5C);
    // GameObject.set_layer for the same walk (see the SetLayerFn typedef).
    struct Il2CppClass* prop_go = find_class_in_domain("UnityEngine", "GameObject");
    if (prop_go) {
        g_go_set_layer.bind(find_method(prop_go, "set_layer", 1));
        g_go_get_layer.bind(find_method(prop_go, "get_layer", 0));
    }
    // CraftData.GetClassIdForTechType -> TryGetScenePrefab: synchronous prefab lookup for the
    // held-prop fallback. The async loader needs managed coroutines, but the prefab database
    // is preloaded at boot (PreparePrefabIDCache), so the sync scene-prefab query hits.
    struct Il2CppClass* craftdata = find_class_in_domain("", "CraftData");
    if (craftdata) {
        g_craft_get_class_id.bind(find_method(craftdata, "GetClassIdForTechType", 1));
    }
    // TryGetScenePrefab lives on ScenePrefabDatabase, not on CraftData (dump line ~473292).
    struct Il2CppClass* prefabdb = find_class_in_domain("", "ScenePrefabDatabase");
    if (prefabdb) {
        g_craft_try_prefab.bind(find_method(prefabdb, "TryGetScenePrefab", 2));
    }
    // PrefabDatabase.GetPrefabAsync is the pollable loader behind the prefab fallback.
    // Note the UWE namespace (dump: "Namespace: UWE, public static class PrefabDatabase").
    struct Il2CppClass* prefab_database = find_class_in_domain("UWE", "PrefabDatabase");
    if (prefab_database) {
        g_prefab_get_async.bind(find_method(prefab_database, "GetPrefabAsync", 1));
    }
    // Optional preload trigger: if the tool's bundle was never touched here, the scene query
    // above misses until something loads it. The 2-arg async overload starts the load without
    // needing a result sink.
    if (craftdata) {
        g_craft_preload.bind(find_method(craftdata, "GetPrefabForTechTypeAsync", 2));
    }
    if (!g_craft_get_class_id || !g_prefab_get_async) {
        LOG_WARN("item api: CraftData/PrefabDatabase path unresolved, never-possessed tools stay invisible");
    }

    // Object.FindObjectsOfType(Type) and Transform.set_rotation.
    struct Il2CppClass* object_class = find_class_in_domain("UnityEngine", "Object");
    g_find_objects_of_type.bind(find_method(object_class, "FindObjectsOfType", 1));
    // The two-argument overload, used to find objects that a previous remote pickup hid.
    g_find_objects_all.bind(find_method(object_class, "FindObjectsOfType", 2));
    struct Il2CppClass* transform = find_class_in_domain("UnityEngine", "Transform");
    if (transform) {
        g_set_rotation.bind(find_method(transform, "set_rotation", 1));
        g_get_rotation.bind(find_method(transform, "get_rotation", 0));
        g_held_transform_point.bind(find_method(transform,"TransformPoint",1));
        g_held_inverse_point.bind(find_method(transform,"InverseTransformPoint",1));
        g_held_set_local_rotation.bind(find_method(transform,"set_localRotation",1));
    }
    // The local player's own "put the held item down", used when a remote drop names an object
    // this phone is holding. It lives on Inventory, not on Player, which is why the first
    // attempt to bind it by name on Player logged as unresolved.
    struct Il2CppClass* inventory = find_class_in_domain("", "Inventory");
    g_inventory_class = inventory;
    g_player_drop_held_item.bind(find_method(inventory, "DropHeldItem", 1));
    if (!g_player_drop_held_item) {
        LOG_WARN("item api: Inventory.DropHeldItem unresolved, a held object cannot be released");
    }
    // The held-item read: Inventory.GetHeld() -> Pickupable -> GetTechType().
    g_inventory_get_held.bind(find_method(inventory, "GetHeld", 0));
    if (!g_inventory_get_held) {
        LOG_WARN("item api: Inventory.GetHeld unresolved, held-item sync disabled");
    }
    // The vehicle read: Player.GetVehicle() hands the piloted Seamoth/Prawn.
    struct Il2CppClass* player_class = find_class_in_domain("", "Player");
    if (player_class) {
        g_player_get_vehicle.bind(find_method(player_class, "GetVehicle", 0));
    }
    if (!g_player_get_vehicle) {
        LOG_WARN("item api: Player.GetVehicle unresolved, vehicle sync disabled");
    }
    // The equipment read: Inventory.get_equipment -> Equipment.GetTechTypeInSlot(slot).
    g_inventory_get_equipment.bind(find_method(inventory, "get_equipment", 0));
    struct Il2CppClass* equipment = find_class_in_domain("", "Equipment");
    if (equipment) {
        g_equip_get_tech_in_slot.bind(find_method(equipment, "GetTechTypeInSlot", 1));
    }
    g_equip_body_str = cached_managed_string("Body");
    g_equip_foots_str = cached_managed_string("Foots");
    g_equip_head_str = cached_managed_string("Head");
    if (!g_inventory_get_equipment || !g_equip_get_tech_in_slot) {
        LOG_WARN("item api: equipment slots unresolved, peer avatars keep the local suit");
    }

    // Walking the parent chain, which is how "is this object in the player's own inventory"
    // is answered. GetComponentInParent cannot be used: it skips inactive objects, and an item
    // being carried is inactive.
    g_gameobject_class = find_class_in_domain("UnityEngine", "GameObject");
    g_gameobject_get_transform.bind(
        find_method(g_gameobject_class, "get_transform", 0));
    // Removal. A remote pickup deletes the object from this phone's world.
    g_object_destroy.bind(find_method(object_class, "Destroy", 1));
    g_object_destroy_immediate.bind(find_method(object_class, "DestroyImmediate", 1));
    g_uid_set_id.bind(find_method(g_unique_identifier_class, "set_Id", 1));

    int guard_missing = 0;
    if (!g_gameobject_get_transform) { LOG_WARN("item api: GameObject.get_transform unresolved"); guard_missing++; }
    if (!g_transform_get_parent) { LOG_WARN("item api: Transform.get_parent unresolved"); guard_missing++; }
    if (!g_object_destroy) { LOG_WARN("item api: Object.Destroy unresolved, pickups will only be hidden"); guard_missing++; }
    if (!g_uid_set_id) { LOG_WARN("item api: UniqueIdentifier.set_Id unresolved, guest inventory keeps the host's ids"); guard_missing++; }
    if (guard_missing) LOG_WARN("item api: %d guard entry point(s) missing", guard_missing);

    LOG_INFO("item api: get_Id=%p attached=%p dropAt=%p setVisible=%p findObjects=%p findAll=%p "
             "uniqType=%p pickupType=%p setRot=%p getRot=%p rbType=%p rbVel=%p getHeld=%p "
             "getTech=%p setParent=%p rbKin=%p rbColl=%p toolType=%p collType=%p setLayer=%p "
             "getClassId=%p tryPrefab=%p preload=%p getAsync=%p nativeGrip=%p",
             (void*)g_unique_get_id.call, (void*)g_pickup_get_attached.call,
             (void*)g_pickup_drop_at.call, (void*)g_pickup_set_visible.call,
             (void*)g_find_objects_of_type.call, (void*)g_find_objects_all.call,
             g_unique_identifier_type, g_pickupable_type,
             (void*)g_set_rotation.call, (void*)g_get_rotation.call,
             g_rigidbody_type, (void*)g_rb_get_velocity.call,
             (void*)g_inventory_get_held.call, (void*)g_pickup_get_tech.call,
             (void*)g_set_parent.call, (void*)g_rb_set_kinematic.call,
             (void*)g_rb_set_detect_colliders.call, g_playertool_type_obj, g_collider_type_obj,
             (void*)g_go_set_layer.call, (void*)g_craft_get_class_id.call,
             (void*)g_craft_try_prefab.call, (void*)g_craft_preload.call,
             (void*)g_prefab_get_async.call, (void*)g_plug_visual.call);
}

bool reid_local_inventory();

// Set by a parked role note or the one-shot timer in hooked_player_update; the re-id itself
// only runs once the world has settled (see the gate there).
bool g_reid_wanted = false;

// Keeps the transport fed and mirrors the local player onto the wire.
void hooked_player_update(void* self, void* method) {
    unsigned long n = __atomic_add_fetch(&g_update_calls, 1, __ATOMIC_RELAXED);

    // A cloned avatar is a Player too, so only the original one may publish state.
    if (!g_local_player) g_local_player = self;
    if (self != g_local_player) {
        if (g_player_update_original) g_player_update_original(self, method);
        return;
    }

    install_seamoth_seat_hooks();

    bool has_session_role_note = false;
    bool pending_session_is_host = false;
    uint32_t pending_session_host_id = 0;
    if (__atomic_load_n(&g_session_role_pending, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&g_session_role_lock);
        if (__atomic_load_n(&g_session_role_pending, __ATOMIC_RELAXED)) {
            pending_session_is_host = g_pending_session_is_host;
            pending_session_host_id = g_pending_session_host_id;
            __atomic_store_n(&g_session_role_pending, false, __ATOMIC_RELAXED);
            has_session_role_note = true;
        }
        pthread_mutex_unlock(&g_session_role_lock);
    }
    if (has_session_role_note) {
        g_local_is_host = pending_session_is_host;
        g_role_known = true;
        if (g_local_is_host) {
            LOG_INFO("role: promoted to host %08x", pending_session_host_id);
            g_host_capture_pending = true;
            if (g_slm_get_main)
                enforce_host_autosave(
                    g_slm_get_main.call(nullptr, g_slm_get_main.method));
        } else {
            LOG_INFO("role: demoted to guest; authority=%08x", pending_session_host_id);
            net_offer_world(nullptr, nullptr);
            g_reid_wanted = true;
            if (g_slm_get_main)
                enforce_guest_no_autosave(
                    g_slm_get_main.call(nullptr, g_slm_get_main.method));
        }
    }
    if (g_host_capture_pending && g_local_is_host) {
        g_host_capture_pending = false;
        log_save_subsystem("host migration", true);
    }

    LocalState local{};
    if (read_player_transform(self, &local)) {
        local.flags = read_player_flags(self);
    }
    local.flags |= read_using_tool_flag();
    // What the quickslot hand carries, so the peers can hang a copy of it on their avatar.
    local.held_tech = read_held_tech();
    read_local_held_rotation(self,&local);
    read_local_held_light(&local);
    read_avatar_environment(self,&local);
    // What the body wears, so the peers can dress their avatar in the real suit/fins/head.
    read_equipment(&local.suit_tech, &local.fins_tech, &local.head_tech);
    // The piloted vehicle, if any, so the peers can drive their copy of the same sub.
    publish_local_vehicle(self, local.flags);
    // Sampled before the clone is updated, so the payload describes the local player exactly as
    // its own Animator sees it this frame.
    read_local_anim_state(&local);
    // Animator-set diagnostic: which local Animator actually hears tool use (body vs viewmodel).
    diag_anim_ensure();
    diag_anim_poll();
    LocalState published=local;
    if(!world_operations_ready())published.valid=false;
    net_publish(published);
    if(published.valid)update_peer_avatars(self);

    // Remote pickups and drops, applied on this thread because Pickupable belongs to Unity.
    pump_remote_events();
    // Host publishes its clock, guest corrects against it (day/night sync); also stamps
    // g_world_live_since, the anchor the re-id gate below waits on.
    time_sync_tick();

    // The inventory re-id is requested by a role note parked by the pump thread (see
    // g_role_note_pending) or by the one-shot timer below, but where it runs is decided by the
    // gate at the end of this block: it walks Unity objects with FindObjectsOfType, which
    // crashes inside Unity while the world is still restoring its containers (tombstone_14),
    // so it waits until the world has been live for kReidSettleSeconds and the player
    // transform is readable. Running it again later is harmless: the fresh ids it generates
    // are deterministic, so a second sweep renames every item to the same value.
    if (__atomic_load_n(&g_role_note_pending, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_role_note_pending, false, __ATOMIC_RELAXED);
        g_reid_wanted = true;
    }
    static bool inventory_reid = false;
    if (!inventory_reid && g_role_known && !g_local_is_host && (n % 900) == 3) {
        g_reid_wanted = true;
    }
    bool world_settled = g_world_live_since > 0.0 &&
                         monotonic_seconds() - g_world_live_since >= kReidSettleSeconds &&
                         (local.x != 0.0f || local.y != 0.0f || local.z != 0.0f);
    if (g_reid_wanted && !g_local_is_host && world_settled) {
        inventory_reid = reid_local_inventory();  // false = not ready yet, try again next frame
        if (inventory_reid) g_reid_wanted = false;
    }
    // The sweep runs after the pump so a remote event is applied first and is part of this
    // tick's snapshot rather than being seen as a change on the next one.
    tick_item_scan(local);

    // Build sync v1: guests apply queued snapshots, the host scans and publishes its own.
    // Resolves its API lazily on the first call, so nothing extra happens at install time.
    tick_build_sync(g_local_player, local);
#if defined(SNMOD_BASE_GEOMETRY_APPLY)
    tick_locker_contents(local);
    if(!tick_creature_motion_probe(local))tick_creature_probe(local);
    tick_player_marker(local);
#endif

    // API binding or slot files may not be ready when this newly installed hook
    // first runs. Retry until capture succeeds, then report once more five seconds
    // later. A new player/world gets its own capture instead of reusing stale state.
    static WorldStartupReports startup_reports;
    if(g_world_live_since>0.0)
        tick_world_startup_reports(&startup_reports,self,monotonic_seconds(),
            g_world_context_watch.slot,g_world_context_generation);

    // The guest's no-autosave rule is re-checked on a timer instead of being set once. The role
    // is not known until the world has been offered, which happens after the first report, and
    // the game can flip the flag back. Both reads are a pair of bools.
    if ((n % 300) == 2 && g_role_known && !g_local_is_host && g_slm_get_main) {
        void* manager = g_slm_get_main.call(nullptr, g_slm_get_main.method);
        enforce_guest_no_autosave(manager);
    }
    if ((n % 300) == 2 && g_role_known && g_local_is_host && g_slm_get_main) {
        void* manager = g_slm_get_main.call(nullptr, g_slm_get_main.method);
        enforce_host_autosave(manager);
        // Host-side offer refresh: catch autosaves that rewrote the slot files and
        // re-publish the world so guests can pull a fresh generation.
        host_offer_refresh_tick();
    }

    // The guest side of the handoff. Reported only on a change of state so a 24 kB transfer
    // does not print a line every frame, and it runs on the game thread only to read what the
    // pump thread already decided - all the network and file work happens over there.
    static WorldHandoffState last_handoff = kWorldHandoffIdle;
    WorldHandoffState handoff = net_world_handoff_state();
    if (handoff != last_handoff) {
        char slot[24];
        char detail[128];
        net_world_handoff_slot(slot, sizeof(slot));
        net_world_handoff_detail(detail, sizeof(detail));
        static const char* names[] = {"idle", "receiving", "verified", "written", "failed"};
        LOG_INFO("world handoff: %s slot=%s (%s)", names[handoff], slot, detail);
        last_handoff = handoff;
    }

    if (n % 300 == 1) {
        LOG_INFO("Player.Update intercepted (call #%lu, self=%p, peers=%zu)", n, self,
                 net_peer_count());
    }
    if (g_player_update_original) {
        g_player_update_original(self, method);
    }
}

}  // namespace

// Outside the anonymous namespace: net.cpp calls this when a session role changes. The
// inventory re-id is wanted as soon as the role is known - a guest that keeps the host's ids
// answers a host pickup by matching its own copy of the item, which is exactly the "no
// Pickupable matches" failure the re-id is meant to remove - but this runs on the pump thread
// and the re-id walks Unity objects, so the request is parked here; hooked_player_update runs
// it once the world has finished restoring (see g_reid_wanted).
void net_note_role_from_peer(uint32_t peer_id) {
    LOG_INFO("role: guest inventory re-id scheduled for authority %08x", peer_id);
    // Parked, not executed: this runs on the pump thread. Consumed by hooked_player_update.
    g_role_note_peer = peer_id;
    __atomic_store_n(&g_role_note_pending, true, __ATOMIC_RELEASE);
}

void net_note_session_role(bool is_host, uint32_t host_id) {
    // Session/election code runs on the pump; role effects and Unity save toggles stay on
    // Player.Update. The mutex prevents a second pump update racing the consumer's snapshot.
    pthread_mutex_lock(&g_session_role_lock);
    g_pending_session_is_host = is_host;
    g_pending_session_host_id = host_id;
    __atomic_store_n(&g_session_role_pending, true, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_session_role_lock);
    if (!is_host) net_note_role_from_peer(host_id);
}

#include "menu_autoload.inc"

// Read-only loading diagnostics. The persistent native coroutine owns completion;
// never force-hide a screen or enable input while the engine may still be loading.
using GetActiveSelfFn = bool (*)(void* self, void* method);
// Static getter uGUI.main: no instance, so the trailing MethodInfo is the only argument.
using GetUguiMainFn = void* (*)(void* method);

struct Il2CppClass* g_ugui_class = nullptr;
struct Il2CppClass* g_scene_loading_class = nullptr;
Entry<GetUguiMainFn> g_ugui_get_main;
Entry<GetActiveSelfFn> g_gameobject_get_active_self;
void* g_ugui_loading_field = nullptr;      // uGUI.loading (uGUI_SceneLoading) 0x38
void* g_loading_is_loading_field = nullptr;  // uGUI_SceneLoading.isLoading 0x4C
void* g_loading_progress_field = nullptr;    // uGUI_SceneLoading.progress 0x48
bool g_loader_diag_attempted = false;
bool g_loader_diag_ready = false;
double g_loader_diag_next = 0.0;

// Field lookup by name AND dump offset, quiet: the diag simply reports less when the
// layout moved, it never changes game state.
void* autoload_field_on(struct Il2CppClass* klass, const char* name, int expected_offset) {
    if (!klass || !il2cpp_class_get_fields || !il2cpp_field_get_name) return nullptr;
    void* it = nullptr;
    for (void* f = il2cpp_class_get_fields(klass, &it); f;
         f = il2cpp_class_get_fields(klass, &it)) {
        const char* n = il2cpp_field_get_name(f);
        if (n && strcmp(n, name) == 0 && il2cpp_field_get_offset(f) == expected_offset) {
            return f;
        }
    }
    return nullptr;
}

bool resolve_loading_screen_api() {
    if(g_loader_diag_ready)return true;
    static double next_attempt=0;
    if(monotonic_seconds()<next_attempt)return false;
    next_attempt=monotonic_seconds()+1;
    {
        g_loader_diag_attempted = true;
        g_ugui_class = find_class_in_domain("", "uGUI");
        g_scene_loading_class = find_class_in_domain("", "uGUI_SceneLoading");
        struct Il2CppClass* game_object = find_class_in_domain("UnityEngine", "GameObject");
        if (g_ugui_class) {
            g_ugui_get_main.bind(find_method(g_ugui_class, "get_main", 0));
            g_ugui_loading_field = autoload_field_on(g_ugui_class, "loading", 0x38);
        }
        if (g_scene_loading_class) {
            g_loading_is_loading_field =
                autoload_field_on(g_scene_loading_class, "isLoading", 0x4C);
            g_loading_progress_field =
                autoload_field_on(g_scene_loading_class, "progress", 0x48);
        }
        if (game_object) {
            g_gameobject_get_active_self.bind(find_method(game_object, "get_activeSelf", 0));
        }
        g_loader_diag_ready = g_ugui_get_main && g_ugui_loading_field &&
                              g_loading_is_loading_field && g_loading_progress_field &&
                              g_gameobject_get_active_self;
        LOG_INFO("loader-diag: api ready=%d (uGUI=%p sceneLoading=%p activeSelf=%d)",
                 g_loader_diag_ready ? 1 : 0, (void*)g_ugui_class,
                 (void*)g_scene_loading_class, g_gameobject_get_active_self ? 1 : 0);
    }
    return g_loader_diag_ready&&g_gameobject_get_active_self;
}

bool loading_screen_blocks_world() {
    if(!resolve_loading_screen_api())return true;
    void* ugui=g_ugui_get_main.call(g_ugui_get_main.method);
    if(!ugui)return true;
    void* loading=nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(ugui),g_ugui_loading_field,&loading);
    if(!loading)return true;
    bool busy=true;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(loading),g_loading_is_loading_field,&busy);
    void* go=g_get_game_object.call(loading,g_get_game_object.method);
    return busy||!go||g_gameobject_get_active_self.call(go,g_gameobject_get_active_self.method);
}

void loader_diag_tick() {
    if (g_autoload_started_at <= 0.0) return;
    double now = monotonic_seconds();
    if (now - g_autoload_started_at > 240.0) {
        LOG_WARN("autoload: completion unconfirmed for exact slot=%s; no forced UI changes",g_autoload_slot);
        g_autoload_started_at=0.0;return;
    }
    if (now < g_loader_diag_next) return;
    g_loader_diag_next = now + 1.0;
    if (!resolve_loading_screen_api()) return;

    void* ugui = g_ugui_get_main.call(g_ugui_get_main.method);
    if (!ugui) {
        LOG_INFO("loader-diag: uGUI.main absent (%.0f s after load start)",
                 now - g_autoload_started_at);
        return;
    }
    void* loading = nullptr;
    il2cpp_field_get_value(reinterpret_cast<struct Il2CppObject*>(ugui), g_ugui_loading_field,
                           &loading);
    if (!loading) {
        LOG_INFO("loader-diag: uGUI.main.loading null (%.0f s after load start)",
                 now - g_autoload_started_at);
        return;
    }
    bool is_loading = false;
    float progress = -1.0f;
    il2cpp_field_get_value(reinterpret_cast<struct Il2CppObject*>(loading),
                           g_loading_is_loading_field, &is_loading);
    il2cpp_field_get_value(reinterpret_cast<struct Il2CppObject*>(loading),
                           g_loading_progress_field, &progress);
    bool active = false;
    bool have_active = false;
    if (g_gameobject_get_active_self) {
        void* loading_go =
            g_get_game_object.call(loading, g_get_game_object.method);
        if (loading_go) {
            active = g_gameobject_get_active_self.call(loading_go,
                                                       g_gameobject_get_active_self.method);
            have_active = true;
        }
    }
    void* menu = nullptr;
    if (g_menu_main_field) il2cpp_field_static_get_value(g_menu_main_field, &menu);
    uint8_t starting = 0;
    if (menu && g_menu_starting_field) {
        il2cpp_field_get_value(reinterpret_cast<struct Il2CppObject*>(menu),
                               g_menu_starting_field, &starting);
    }
    LOG_INFO("loader-diag: t=%.0fs isLoading=%d progress=%.3f loadingActive=%d menu=%d "
             "isStartingNewGame=%d",
             now - g_autoload_started_at, is_loading ? 1 : 0, progress,
             have_active ? (active ? 1 : 0) : -1, menu ? 1 : 0, starting);

    if (!is_loading && have_active && !active && net_session_state()==kSessionGuest) {
        char current_slot[64]{};
        void* manager=g_slm_get_main ? g_slm_get_main.call(nullptr,g_slm_get_main.method) : nullptr;
        bool exact=manager && g_slm_get_current_slot &&
            string_to_ascii(g_slm_get_current_slot.call(manager,g_slm_get_current_slot.method),
                current_slot,sizeof(current_slot)) && !strcmp(current_slot,g_autoload_slot);
        if (exact) {
            LOG_INFO("autoload: completed exact slot=%s gen=%u; native loading screen dismissed",
                g_autoload_slot,g_autoload_generation);
            g_autoload_started_at=0.0;
        }
    }

}

void world_context_tick(void* manager) {
    if(!manager || !g_slm_get_current_slot || !g_slm_is_loading) return;
    char slot[24]{};
    string_to_ascii(g_slm_get_current_slot.call(manager,g_slm_get_current_slot.method),slot,sizeof(slot));
    const bool loading=g_slm_is_loading.call(manager,g_slm_is_loading.method);
    void* menu=nullptr;uint8_t starting=0;
    if(resolve_menu_autoload_api()) {
        il2cpp_field_static_get_value(g_menu_main_field,&menu);
        if(menu) il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(menu),g_menu_starting_field,&starting);
    }
    const bool at_menu=menu && !loading && !starting && !daynight_instance();
    const bool transition=world_context_transition(&g_world_context_watch,slot,loading);
#if defined(SNMOD_BASE_GEOMETRY_APPLY)
    if(transition||at_menu||loading)hud_marker_destroy("world transition/menu");
#endif
    if(transition)reset_prefab_requests();
    if((transition || at_menu) && net_session_invalidate_host_world()) {
        net_offer_world(nullptr,nullptr);world_release();
        g_local_is_host=false;g_role_known=false;g_world_live_since=0;
        ++g_world_context_generation;
        LOG_INFO("world: invalidated previous host offer context=%u slot=%s loading=%u menu=%u",
            g_world_context_generation,slot,unsigned(loading),unsigned(at_menu));
    }
    if(at_menu) net_session_set_world_active(false);
    launch_role_bridge_tick(at_menu);
}
void world_notice_tick() {
    static Entry<void (*)(void*,void*)> add_message;
    static void* message_main = nullptr;
    static double next_check = 0.0;
    const double now = monotonic_seconds();
    if (now < next_check) return;
    next_check = now + 0.15;
    if (!add_message || !message_main) {
        auto* klass = find_class_in_domain("", "ErrorMessage");
        add_message.bind(find_method_typed(klass,"AddMessage",1,"system.string"));
        message_main = autoload_field_on(klass,"main",0);
        if (!add_message || !message_main) {
            next_check = now + 2.0;
            return;
        }
    }
    void* instance = nullptr;
    il2cpp_field_static_get_value(message_main,&instance);
    if (!instance) return;
    WorldNotice notice;
    if (!net_take_world_notice(&notice)) return;
    const char* text = nullptr;
    switch (notice) {
        case WorldNotice::HostFound: text = "Кооп: хост найден. Подключаемся…"; break;
        case WorldNotice::GuestFound: text = "Кооп: напарник найден. Готовим передачу мира…"; break;
        case WorldNotice::Sending: text = "Кооп: отправляем мир напарнику…"; break;
        case WorldNotice::Receiving: text = "Кооп: получаем мир от хоста…"; break;
        case WorldNotice::Received: text = "Кооп: мир получен. Готовим загрузку…"; break;
        case WorldNotice::Sent: text = "Кооп: напарник получил мир"; break;
        case WorldNotice::ReceiveFailed: text = "Кооп: не удалось получить мир"; break;
        case WorldNotice::SendFailed: text = "Кооп: передача мира прервалась"; break;
    }
    try {
        if (void* message = cached_managed_string(text)) {
            add_message.call(message,add_message.method);
            LOG_INFO("world notice: %s",text);
        }
    } catch (...) { LOG_WARN("world notice: game message unavailable"); }
}

void hooked_slm_update(void* self, void* method) {
    if (g_slm_update_original) g_slm_update_original(self, method);
    world_context_tick(self);
    world_notice_tick();
    menu_autoload_tick();
    loader_diag_tick();
}

bool install_managed_hooks() {
    if (!il2cpp_class_get_method_from_name) {
        LOG_ERROR("il2cpp_class_get_method_from_name unavailable");
        return false;
    }

    struct Il2CppClass* player = find_class_in_domain("", "Player");
    if (!player) {
        LOG_ERROR("class Player not found, managed hooks skipped");
        return false;
    }

    struct Il2CppMethodInfo* update =
        il2cpp_class_get_method_from_name(player, "Update", 0);
    if (!update) {
        LOG_ERROR("Player.Update not found");
        return false;
    }

    // Il2CppMethodInfo is only forward declared, so the raw bytes are read through char*.
    char* info = reinterpret_cast<char*>(update);
    void* direct = *reinterpret_cast<void**>(info + kMethodPointerOffset);
    void* virt = *reinterpret_cast<void**>(info + kVirtualPointerOffset);
    void* invoker = *reinterpret_cast<void**>(info + kMethodInvokerOffset);
    const char* method_name = *reinterpret_cast<const char**>(info + 0x18);
    unsigned short slot = *reinterpret_cast<unsigned short*>(info + kMethodSlotOffset);
    unsigned short vtable_count =
        *reinterpret_cast<unsigned short*>(reinterpret_cast<char*>(player) + kClassVtableCountOffset);

    LOG_INFO("MethodInfo at %p: name=%s ptr=%p virtual=%p invoker=%p", (void*)update,
             method_name ? method_name : "?", direct, virt, invoker);
    LOG_INFO("Player.Update slot=%u vtable_count=%u", slot, vtable_count);

    void* replacement = reinterpret_cast<void*>(&hooked_player_update);
    if (!write_pointer(info + kMethodPointerOffset, replacement)) return false;
    if (virt && !write_pointer(info + kVirtualPointerOffset, replacement)) return false;

    // Virtual dispatch goes through klass->vtable[slot], so patch that entry too.
    if (slot != 0xFFFF && slot < vtable_count) {
        void* slot_addr = reinterpret_cast<char*>(player) + kClassVtableOffset + slot * kVirtualInvokeSize;
        void* slot_original = *(void**)slot_addr;
        if (slot_original && !write_pointer(slot_addr, replacement)) return false;
        if (slot_original) g_player_update_original = reinterpret_cast<ManagedFn>(slot_original);
        LOG_INFO("vtable[%u] %p -> hook", slot, slot_addr);
    }

    if (!g_player_update_original && direct) {
        g_player_update_original = reinterpret_cast<ManagedFn>(direct);
    }

    // Component.transform, Transform.get_position and Transform.get_rotation give the
    // local pose without allocating anything on the game thread.
    struct Il2CppClass* component = find_class_in_domain("UnityEngine", "Component");
    struct Il2CppClass* transform = find_class_in_domain("UnityEngine", "Transform");
    g_get_transform.bind(find_method(component, "get_transform", 0));
    g_get_position.bind(find_method(transform, "get_position", 0));
    g_get_euler.bind(nullptr);
    if (transform && il2cpp_class_get_methods) {
        void* iterator = nullptr;
        for (struct Il2CppMethodInfo* m = il2cpp_class_get_methods(transform, &iterator);
             m; m = il2cpp_class_get_methods(transform, &iterator)) {
            const char* name = il2cpp_method_get_name(m);
            if (!name || strcmp(name, "get_eulerAngles") != 0) continue;
            g_get_euler.bind(m);
            break;
        }
    }
    LOG_INFO("pose getters: transform=%p position=%p euler=%p", (void*)g_get_transform.call,
             (void*)g_get_position.call, (void*)g_get_euler.call);

    // Player state predicates drive the flags field of the outgoing state packet.
    g_is_swimming.bind(find_method(player, "IsSwimming", 0));
    g_is_inside.bind(find_method(player, "IsInside", 0));
    g_is_piloting.bind(find_method(player, "IsPiloting", 0));
    g_is_underwater_for_swimming.bind(find_method(player, "IsUnderwaterForSwimming", 0));
    LOG_INFO("player predicates: swimming=%p inside=%p piloting=%p underwaterForSwimming=%p",
             (void*)g_is_swimming.call, (void*)g_is_inside.call, (void*)g_is_piloting.call,
             (void*)g_is_underwater_for_swimming.call);

    resolve_avatar_api();

    // Item sync is allowed to fail on its own: the avatars and the handoff are useful without
    // it, and a build that cannot resolve Pickupable should still install.
    resolve_item_api();

    // Bound separately from the avatar API and allowed to fail on its own: nothing about
    // rendering a peer depends on the save subsystem, so a build that cannot resolve it is
    // still a working build.
    resolve_save_api();

    install_seamoth_seat_hooks();

    // SaveLoadManager.Update is the menu-scene game-thread tick: the manager exists in the
    // main menu, where Player.Update does not, and its class is materialised at install
    // time (the save API above resolved it). The slot auto-load consumer rides on it.
    struct Il2CppClass* slm_class = find_class_in_domain("", "SaveLoadManager");
    struct Il2CppMethodInfo* slm_update =
        slm_class ? il2cpp_class_get_method_from_name(slm_class, "Update", 0) : nullptr;
    if (slm_update) {
        char* slm_info = reinterpret_cast<char*>(slm_update);
        void* slm_direct = *reinterpret_cast<void**>(slm_info + kMethodPointerOffset);
        void* slm_virt = *reinterpret_cast<void**>(slm_info + kVirtualPointerOffset);
        g_slm_update_original = reinterpret_cast<ManagedFn>(slm_direct);
        void* slm_replacement = reinterpret_cast<void*>(&hooked_slm_update);
        bool slm_ok = write_pointer(slm_info + kMethodPointerOffset, slm_replacement);
        if (slm_virt) {
            slm_ok = write_pointer(slm_info + kVirtualPointerOffset, slm_replacement) && slm_ok;
        }
        unsigned short slm_slot =
            *reinterpret_cast<unsigned short*>(slm_info + kMethodSlotOffset);
        unsigned short slm_vtable_count = *reinterpret_cast<unsigned short*>(
            reinterpret_cast<char*>(slm_class) + kClassVtableCountOffset);
        if (slm_slot != 0xFFFF && slm_slot < slm_vtable_count) {
            void* slm_slot_addr = reinterpret_cast<char*>(slm_class) + kClassVtableOffset +
                                  slm_slot * kVirtualInvokeSize;
            void* slm_slot_original = *(void**)slm_slot_addr;
            if (slm_slot_original) {
                if (!g_slm_update_original) {
                    g_slm_update_original = reinterpret_cast<ManagedFn>(slm_slot_original);
                }
                slm_ok = write_pointer(slm_slot_addr, slm_replacement) && slm_ok;
            }
        }
        LOG_INFO("SaveLoadManager.Update hook: direct=%p virtual=%p slot=%u ok=%d",
                 slm_direct, slm_virt, slm_slot, slm_ok ? 1 : 0);
    } else {
        LOG_WARN("SaveLoadManager.Update not found; slot auto-load disabled");
    }

    // ArmsController.Update gets the same treatment as Player.Update above. It is patched rather
    // than bypassed, because the clone needs its arm IK and must not have its animation writes.
    // See hooked_arms_controller_update.
    struct Il2CppClass* arms_class = find_class_in_domain("", "ArmsController");
    struct Il2CppMethodInfo* arms_update =
        arms_class ? il2cpp_class_get_method_from_name(arms_class, "Update", 0) : nullptr;
    if (arms_update) {
        char* arms_info = reinterpret_cast<char*>(arms_update);
        void* arms_direct = *reinterpret_cast<void**>(arms_info + kMethodPointerOffset);
        void* arms_virt = *reinterpret_cast<void**>(arms_info + kVirtualPointerOffset);
        g_arms_update_original = reinterpret_cast<ManagedFn>(arms_direct);
        void* arms_replacement = reinterpret_cast<void*>(&hooked_arms_controller_update);
        bool arms_ok = write_pointer(arms_info + kMethodPointerOffset, arms_replacement);
        if (arms_virt) {
            arms_ok = write_pointer(arms_info + kVirtualPointerOffset, arms_replacement) && arms_ok;
        }
        unsigned short arms_slot =
            *reinterpret_cast<unsigned short*>(arms_info + kMethodSlotOffset);
        unsigned short arms_vtable_count = *reinterpret_cast<unsigned short*>(
            reinterpret_cast<char*>(arms_class) + kClassVtableCountOffset);
        if (arms_slot != 0xFFFF && arms_slot < arms_vtable_count) {
            void* slot_addr = reinterpret_cast<char*>(arms_class) + kClassVtableOffset +
                              arms_slot * kVirtualInvokeSize;
            void* slot_original = *(void**)slot_addr;
            if (slot_original) {
                if (!g_arms_update_original) {
                    g_arms_update_original = reinterpret_cast<ManagedFn>(slot_original);
                }
                arms_ok = write_pointer(slot_addr, arms_replacement) && arms_ok;
            }
        }
        LOG_INFO("ArmsController.Update hook: direct=%p virtual=%p slot=%u ok=%d", arms_direct,
                 arms_virt, arms_slot, arms_ok ? 1 : 0);
    } else {
        LOG_WARN("ArmsController.Update not found, arm IK will stay under local control");
    }

#if defined(SNMOD_BASE_GEOMETRY_APPLY)
    contents_install_reactor_update();
#endif
    LOG_INFO("managed hooks installed: Player.Update (direct=%p virtual=%p invoker=%p)",
             direct, virt, invoker);
    LOG_INFO("SNMOD_READY managed hooks installed, net starting");
    net_init();
    LOG_INFO("SNMOD_READY");
    return true;
}
