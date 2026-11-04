
#include "modloader/src/base_geometry_codec.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#define LOG_WARN(...) ((void)0)
struct Il2CppObject {};
struct Array { int length; };
struct Base { int shape[4]; Array *faces, *cells, *links, *glass, *masks, *objects; void* placement; };
struct Fields { void *shape, *faces, *cells, *links, *glass, *masks; } g_ghost_capture{
 (void*)1,(void*)2,(void*)3,(void*)4,(void*)5,(void*)6};
struct { void* cell_objects = (void*)7; void* placement_ghost = (void*)8; } g_corridor_api;
void il2cpp_field_get_value(Il2CppObject* object, void* field, void* output) {
 auto& b = *reinterpret_cast<Base*>(object);
 switch ((uintptr_t)field) {
 case 1: memcpy(output,b.shape,sizeof(b.shape)); break;
 case 2: *(Array**)output=b.faces; break; case 3: *(Array**)output=b.cells; break;
 case 4: *(Array**)output=b.links; break; case 5: *(Array**)output=b.glass; break;
 case 6: *(Array**)output=b.masks; break; case 7: *(Array**)output=b.objects; break;
 case 8: *(void**)output=b.placement; break; default: assert(false);
 }
}
void il2cpp_field_set_value_object(Il2CppObject* object, void* field, Il2CppObject* input) {
 assert(field==(void*)8); reinterpret_cast<Base*>(object)->placement=input;
}
struct { void* method=nullptr; int call(void* array,void*) { return ((Array*)array)->length; } } g_array_get_length;
int destroyed=0;
struct { void* method=nullptr; void call(void*,void*) { ++destroyed; } } g_object_destroy;
bool build_live(void* object) { return object!=nullptr; }
int g_applying_remote=0;
struct WorldEvent { const char* object_id="test"; };
namespace base_geometry { struct GhostEnvelope {}; }
bool build_corridor_set_reference(void* object, void* field, void* reference) {
    // This API takes the managed reference itself, never the address of a local
    // pointer. The latter stores a stack address and corrupts the managed heap.
    il2cpp_field_set_value_object(reinterpret_cast<Il2CppObject*>(object), field,
                                  reinterpret_cast<Il2CppObject*>(reference));
    void* actual = nullptr;
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(object), field, &actual);
    return actual == reference;
}

struct CorridorAttempt {
    void* object = nullptr;
    void* ghost = nullptr;
    const char* stage = "validate";
    bool placed = false;
};
// A caught game exception stops further attempts until the process is restarted.
bool g_corridor_apply_failed = false;

bool build_corridor_storage_valid(void* base, const base_geometry::Snapshot& snapshot) {
    auto& a = g_ghost_capture;
    int32_t shape[4]{};
    il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(base), a.shape, shape);
    if (shape[0] != snapshot.shape[0] || shape[1] != snapshot.shape[1] ||
        shape[2] != snapshot.shape[2] || shape[3] != shape[0] * shape[1]) return false;
    size_t count = 0;
    if (!base_geometry::cell_count(snapshot.shape, &count)) return false;
    auto matches = [&](void* field, size_t length, bool nullable = false) {
        void* array = nullptr;
        il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(base), field, &array);
        return !array ? nullable : g_array_get_length.call(array, g_array_get_length.method) ==
                                 static_cast<int32_t>(length);
    };
    return matches(a.faces, count * 6) && matches(a.cells, count) &&
           matches(a.links, count) && matches(a.glass, count) && matches(a.masks, count, true) &&
           matches(g_corridor_api.cell_objects, count);
}


int calls=0, mode=0;
bool build_apply_completed_corridor_inner(void* prefab, void*, const WorldEvent&,
 const base_geometry::GhostEnvelope&, CorridorAttempt& attempt) {
 ++calls; attempt.object=prefab; attempt.ghost=prefab; attempt.stage="mock-game-call";
 ++g_applying_remote;
 if (mode==1 || mode==2) { attempt.placed=(mode==2); throw std::runtime_error("game"); }
 return mode==3;
}
bool build_apply_completed_corridor(void* prefab, void* target, const WorldEvent& e,
                                    const base_geometry::GhostEnvelope& snapshot) {
    if (g_corridor_apply_failed) return false;
    CorridorAttempt attempt;
    const int prior_remote = g_applying_remote;
    bool verified = false;
    try {
        verified = build_apply_completed_corridor_inner(prefab, target, e, snapshot, attempt);
    } catch (...) {
        // IL2CPP throws C++ exceptions. Never let one skip the remote-state reset
        // or repeat a partly applied build on the following Player.Update.
        g_corridor_apply_failed = true;
        LOG_WARN("build: corridor game exception id=%s stage=%s; apply disabled until restart",
                 e.object_id, attempt.stage);
    }
    g_applying_remote = prior_remote;
    if (!verified && attempt.object && !attempt.placed) {
        try {
            if (!build_live(attempt.object)) return false;
            void* placement = nullptr;
            if (build_live(target) && g_corridor_api.placement_ghost) {
                il2cpp_field_get_value(reinterpret_cast<Il2CppObject*>(target),
                                       g_corridor_api.placement_ghost, &placement);
                if (placement == attempt.ghost) {
                    if (!build_corridor_set_reference(target, g_corridor_api.placement_ghost, nullptr)) {
                        g_corridor_apply_failed = true;
                        LOG_WARN("build: corridor reference cleanup failed id=%s", e.object_id);
                    }
                }
            }
            g_object_destroy.call(attempt.object, g_object_destroy.method);
        } catch (...) {
            g_corridor_apply_failed = true;
            LOG_WARN("build: corridor cleanup failed id=%s; apply disabled", e.object_id);
        }
    }
    return verified;
}

int main() {
 Array six{6}, one{1}, stale{2};
 Base b{{1,1,1,1},&six,&one,&one,&one,nullptr,&one,nullptr};
 base_geometry::Snapshot snapshot; snapshot.shape={1,1,1};
 assert(build_corridor_storage_valid(&b,snapshot));
 b.objects=&stale; assert(!build_corridor_storage_valid(&b,snapshot)); b.objects=&one;
 b.shape[3]=2; assert(!build_corridor_storage_valid(&b,snapshot)); b.shape[3]=1;
 b.masks=&stale; assert(!build_corridor_storage_valid(&b,snapshot)); b.masks=&one;
 assert(build_corridor_storage_valid(&b,snapshot));
 b.faces=&one; assert(!build_corridor_storage_valid(&b,snapshot)); b.faces=&six;
 int object, other; WorldEvent e; base_geometry::GhostEnvelope envelope;
 assert(build_corridor_set_reference(&b,(void*)8,&object) && b.placement==&object);
 assert(build_corridor_set_reference(&b,(void*)8,nullptr) && b.placement==nullptr);
 b.placement=&object; g_applying_remote=7; mode=1;
 assert(!build_apply_completed_corridor(&object,&b,e,envelope));
 assert(g_applying_remote==7 && destroyed==1 && b.placement==nullptr && g_corridor_apply_failed);
 int old_calls=calls; assert(!build_apply_completed_corridor(&object,&b,e,envelope));
 assert(calls==old_calls && destroyed==1);
 g_corridor_apply_failed=false; mode=0; b.placement=&other;
 assert(!build_apply_completed_corridor(&object,&b,e,envelope));
 assert(g_applying_remote==7 && destroyed==2 && b.placement==&other);
 mode=2; b.placement=&object;
 assert(!build_apply_completed_corridor(&object,&b,e,envelope));
 assert(g_applying_remote==7 && destroyed==2 && b.placement==&object && g_corridor_apply_failed);
 g_corridor_apply_failed=false; mode=3;
 assert(build_apply_completed_corridor(&object,&b,e,envelope));
 assert(g_applying_remote==7 && destroyed==2);
 puts("corridor storage and exception-guard tests passed (mock game calls)");
}
