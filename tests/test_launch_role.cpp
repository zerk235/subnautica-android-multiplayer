#include "modloader/src/net.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
struct State {
 pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
 SessionLaunchRole launch_role=kLaunchAuto;
 SessionState state=kSessionDiscovering;
 bool world_active=false,local_host=false,local_has_world=false,local_member=false,active=false;
 bool role_reset_pending=false,explicit_leave=false,selected=false,join_request_pending=false,join_accepted=false;
 bool presence_dirty=false,content_guid_valid=false,resume_host_slot=false;
 uint32_t host_id=0,epoch=0,host_world_gen_seen=0;
 double election_started=0,last_host_presence=0;
 uint8_t content_guid[16]{},session_id[16]{};char slot[24]{};
}g_session;
uint32_t g_local_id=123;
constexpr double kElectionConvergenceSeconds=1.5;
bool received=true;
bool world_slot_is_host_received(const char*){return received;}
bool bytes_nonzero(const uint8_t* p,size_t n){for(size_t i=0;i<n;++i)if(p[i])return true;return false;}
bool same_content(const uint8_t* a,const uint8_t* b){return !memcmp(a,b,16);}
double monotonic_seconds(){return 100;}
void random_bytes(uint8_t* out,size_t n){memset(out,7,n);}
void session_set_state_locked(SessionState state,SessionJoinStatus,const char*){g_session.state=state;}
#include "modloader/src/session_launch_role.inc"
#include "modloader/src/session_local_world.inc"
void idle(){g_session.active=g_session.local_member=g_session.local_host=g_session.local_has_world=g_session.world_active=false;g_session.selected=g_session.join_accepted=g_session.join_request_pending=false;}
int main(){
 uint8_t guid[16]{1};
 net_session_set_local_world(true,"slot-test",guid);
 assert(!g_session.local_host && g_session.resume_host_slot && !g_session.active); // ordinary downloaded copy cannot fork a host
 idle();assert(net_session_choose_role(kLaunchHost));
 assert(g_session.role_reset_pending && g_session.explicit_leave && g_session.launch_role==kLaunchHost);
 g_session.role_reset_pending=g_session.explicit_leave=false; // pump leaves; exercise the actual next-world transition
 net_session_set_local_world(true,"slot-test",guid);
 assert(g_session.active && g_session.local_host && g_session.local_member && g_session.local_has_world);
 assert(g_session.host_id==g_local_id && g_session.epoch==1 && g_session.state==kSessionHost);
 assert(!net_session_choose_role(kLaunchGuest) && g_session.launch_role==kLaunchHost); // never change authority mid-world
 idle();g_session.selected=g_session.join_request_pending=g_session.join_accepted=true;
 assert(net_session_choose_role(kLaunchGuest));assert(!g_session.selected && !g_session.join_request_pending && !g_session.join_accepted);
 received=false;net_session_set_local_world(true,"native-world",guid);
 assert(!g_session.local_host && !g_session.active && !g_session.local_has_world && g_session.state==kSessionConflict);
 idle();assert(net_session_choose_role(kLaunchAuto));
 net_session_set_local_world(true,"native-world",guid);assert(g_session.local_host);
 idle();assert(!net_session_choose_role(SessionLaunchRole(99)));
 g_session.launch_role=kLaunchGuest;g_session.active=g_session.local_member=g_session.join_accepted=true;
 g_session.content_guid_valid=true;memcpy(g_session.content_guid,guid,16);strcpy(g_session.slot,"slot-test");g_session.host_id=42;
 net_session_set_local_world(true,"slot-test",guid);assert(g_session.local_has_world && !g_session.local_host && g_session.state==kSessionGuest);
 puts("actual launch role: downloaded copy remains guest by default, explicit host creates authority, live change refused, pending JOIN canceled, guest cannot self-host, auto restored and accepted guest load preserved passed");
}
