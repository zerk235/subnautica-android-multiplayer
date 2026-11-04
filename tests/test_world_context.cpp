#include "modloader/src/world_context.h"
#include "modloader/src/net.h"
#include <cassert>
#include <pthread.h>
#define LOG_INFO(...) ((void)0)
struct {
    pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    bool active=false,local_host=false,local_member=false,local_has_world=false;
    bool join_accepted=false,selected=false,join_request_pending=false;
    bool content_guid_valid=false,resume_host_slot=false,world_refresh_pending=false;
    bool world_active=false,presence_dirty=false;
    uint32_t host_id=0,epoch=0,host_world_gen_seen=0;
    char slot[24]{};
    uint8_t session_id[16]{},content_guid[16]{};
    double election_started=0,world_request_last=0,leave_cooldown_until=0;
    SessionState state=kSessionDiscovering;
} g_session;
double monotonic_seconds(){return 100.;}
void session_set_state_locked(SessionState state,SessionJoinStatus,const char*){g_session.state=state;}
#include "modloader/src/session_world_context.inc"
int main(){
    WorldContextWatch watch;
    assert(!world_context_transition(&watch,"",false));
    assert(!world_context_transition(&watch,"slot-old",false));
    assert(!world_context_transition(&watch,"slot-old",false)); // autosave does not reset
    assert(world_context_transition(&watch,"slot-old",true)); // same-slot reload
    assert(!world_context_transition(&watch,"slot-old",true));
    assert(world_context_transition(&watch,"slot-new",true)); // reused player, new slot
    assert(!world_context_transition(&watch,"slot-new",false));
    assert(!world_context_transition(&watch,"slot-new",false));
    // A menu observer must never revoke an accepted guest download/autoload.
    g_session.active=g_session.local_member=g_session.join_accepted=true;
    g_session.host_id=42;std::strcpy(g_session.slot,"guest-slot");
    assert(!net_session_invalidate_host_world());
    assert(g_session.join_accepted && g_session.active && g_session.local_member);
    assert(!session_world_servable(nullptr));
    // The old host identity becomes unservable immediately, including its pinned transfer.
    g_session.local_host=g_session.local_has_world=true;
    g_session.epoch=7;g_session.session_id[0]=1;g_session.content_guid[0]=2;
    g_session.content_guid_valid=true;g_session.host_world_gen_seen=17;
    assert(session_world_servable("guest-slot"));
    assert(!session_world_servable("another-slot"));
    assert(net_session_invalidate_host_world());
    assert(!session_world_servable(nullptr) && !session_world_servable("guest-slot"));
    assert(!g_session.active && !g_session.local_member && !g_session.local_has_world);
    assert(!g_session.join_accepted && !g_session.content_guid_valid);
    assert(!g_session.host_id && !g_session.epoch && !g_session.session_id[0]);
    assert(!g_session.slot[0] && !g_session.content_guid[0] && !g_session.host_world_gen_seen);
    assert(g_session.world_active && g_session.presence_dirty);
    assert(!net_session_invalidate_host_world());
    puts("world context: same-player slot switch/reload, guest JOIN preserved, old host and transfer retired: PASS");
}
