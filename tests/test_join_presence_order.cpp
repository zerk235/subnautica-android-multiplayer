// Reproduce the packet ordering seen after a full restart: JOIN is accepted, then
// an older discovery presence arrives before the guest's joined presence.
#define SNMOD_BASE_GEOMETRY_CAPTURE
#define SNMOD_WORLD_TEST_ROOT "/data/local/tmp/snmod_join_order_tests"
#include "modloader/src/world.cpp"
#include "modloader/src/net.cpp"
#include <cassert>
void mod_logf(const char*, const char*, ...) {}
void net_note_session_role(bool, uint32_t) {}

int main() {
    pthread_once(&g_peers_lock_once,init_peers_lock);
    g_local_id=1;
    g_session.active=g_session.local_member=g_session.local_host=g_session.local_has_world=true;
    g_session.epoch=1; g_session.host_id=1; g_session.session_id[0]=7;
    g_session.content_guid[0]=9; g_session.content_guid_valid=true;
    strcpy(g_session.slot,"test-slot");
    WorldOffer offer{}; offer.valid=true; g_offer_snapshot=&offer;
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_port=htons(45001);
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    SessionPresencePacket presence{};
    presence.header.magic=kMagic; presence.header.type=kPacketSessionPresence;
    presence.protocol_version=kSessionProtocolVersion; presence.packet_size=sizeof(presence);
    presence.sender_id=2; presence.sequence=1; presence.max_players=kMaxPeers+1;
    strcpy(presence.game_build,kGameBuild); strcpy(presence.mod_version,kModVersion);
    auto announce=[&] {handle_packet(reinterpret_cast<uint8_t*>(&presence),sizeof(presence),address,monotonic_seconds());};
    announce();
    JoinRequestPacket join{};
    join.header.magic=kMagic; join.header.type=kPacketJoinRequest;
    join.protocol_version=kSessionProtocolVersion; join.packet_size=sizeof(join);
    join.sender_id=2; join.target_host_id=1; join.session_epoch=1;
    memcpy(join.session_id,g_session.session_id,16); join.request_nonce=123;
    auto request=[&] {handle_packet(reinterpret_cast<uint8_t*>(&join),sizeof(join),address,monotonic_seconds());};
    request();
    assert(g_remote[0].join_accepted && g_remote[0].cached_reply.status==kJoinAccepted);
    assert(authorize_world_sender(2,address,false));
    ++presence.sequence; announce(); // emitted before the guest received the JOIN reply
    ++presence.sequence;
    presence.flags=kSessionFlagMember; presence.session_epoch=1; presence.host_id=1;
    memcpy(presence.session_id,g_session.session_id,16);
    memcpy(presence.content_guid,g_session.content_guid,16);
    strcpy(presence.slot,g_session.slot); announce();
    if (!authorize_world_sender(2,address,false)) {
        puts("FAIL: accepted JOIN was erased by delayed discovery presence; world transfer stays blocked");
        return 1;
    }
    // A world that changed on the host must never inherit this grant.
    g_session.session_id[1]=5;
    memcpy(presence.session_id,g_session.session_id,16); ++presence.sequence; announce();
    assert(!authorize_world_sender(2,address,false));
    g_session.session_id[1]=0;
    memcpy(presence.session_id,g_session.session_id,16); ++presence.sequence; announce();
    assert(authorize_world_sender(2,address,false));
    // The same session after migration has a different epoch; an old grant isn't admission.
    g_session.epoch=2; presence.session_epoch=2; ++presence.sequence; announce();
    assert(!join_grant_matches(g_remote[0],g_session.session_id,g_session.epoch));
    // Explicit leave revokes admission and cannot be undone by another presence.
    g_session.epoch=1; presence.session_epoch=1; ++presence.sequence; announce();
    SessionLeavePacket leave{}; leave.header.magic=kMagic; leave.header.type=kPacketSessionLeave;
    leave.protocol_version=kSessionProtocolVersion; leave.packet_size=sizeof(leave);
    leave.sender_id=2; leave.host_id=1; leave.session_epoch=1;
    memcpy(leave.session_id,g_session.session_id,16);
    handle_packet(reinterpret_cast<uint8_t*>(&leave),sizeof(leave),address,monotonic_seconds());
    ++presence.sequence; announce(); assert(!authorize_world_sender(2,address,false));
    release_guest(find_guest(2)); g_offer_snapshot=nullptr;
    puts("PASS: reordered presence preserves real JOIN; other sessions and explicit leave remain blocked");
}
