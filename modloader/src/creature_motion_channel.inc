// Included after geometry channel helpers. Pump owns socket access; no Unity here.
pthread_mutex_t g_creature_motion_lock=PTHREAD_MUTEX_INITIALIZER;
std::deque<creature_motion::Message> g_creature_motion_tx,g_creature_motion_rx;
bool creature_motion_enqueue(const creature_motion::Message& m) {
    if(!creature_motion::valid(m)||m.sender!=g_local_id)return false;
    const auto session=geometry_session();
    if(!session.valid||m.epoch!=session.epoch||memcmp(m.session.data(),session.id,16))return false;
    pthread_mutex_lock(&g_creature_motion_lock);
    // At most one pending pose per exact run; control messages cannot be evicted by poses.
    if(m.kind==creature_motion::Pose)for(auto& old:g_creature_motion_tx)
        if(old.kind==m.kind&&old.target==m.target&&creature_motion::same_run(old,m)){
            old=m;pthread_mutex_unlock(&g_creature_motion_lock);return true;
        }
    bool ok=g_creature_motion_tx.size()<32;if(ok)g_creature_motion_tx.push_back(m);
    pthread_mutex_unlock(&g_creature_motion_lock);return ok;
}
void creature_motion_service() {
    std::deque<creature_motion::Message> pending;
    pthread_mutex_lock(&g_creature_motion_lock);pending.swap(g_creature_motion_tx);pthread_mutex_unlock(&g_creature_motion_lock);
    if(pending.empty())return;
    const auto session=geometry_session();const auto peers=geometry_peers();
    for(const auto& m:pending){
        if(!session.valid||m.epoch!=session.epoch||memcmp(m.session.data(),session.id,16))continue;
        std::vector<uint8_t> body;if(!creature_motion::encode(m,&body))continue;
        PacketHeader h{};h.magic=kMagic;h.type=kPacketCreatureProbe;
        std::vector<uint8_t> packet(sizeof(h)+body.size());memcpy(packet.data(),&h,sizeof(h));memcpy(packet.data()+sizeof(h),body.data(),body.size());
        for(const auto& peer:peers)if(peer.id==m.target){send_to(packet.data(),packet.size(),peer.raw);break;}
    }
}
void creature_motion_handle_packet(const uint8_t* data,size_t size,const sockaddr_in& addr) {
    if(size!=sizeof(PacketHeader)+creature_motion::kWireSize)return;
    PacketHeader h{};memcpy(&h,data,sizeof(h));if(!header_reserved_zero(h))return;
    creature_motion::Message m;if(!creature_motion::decode(data+sizeof(h),size-sizeof(h),&m))return;
    const auto session=geometry_session();creature_motion::Id id{};memcpy(id.data(),session.id,16);
    if(!session.valid||!creature_motion::permitted(m,g_local_id,net_session_host_id(),id,session.epoch)||!geometry_peer_allowed(m.sender,addr))return;
    pthread_mutex_lock(&g_creature_motion_lock);
    if(g_creature_motion_rx.size()<64)g_creature_motion_rx.push_back(m);
    pthread_mutex_unlock(&g_creature_motion_lock);
}
bool creature_motion_take(creature_motion::Message* out) {
    if(!out)return false;
    pthread_mutex_lock(&g_creature_motion_lock);bool ok=!g_creature_motion_rx.empty();
    if(ok){*out=g_creature_motion_rx.front();g_creature_motion_rx.pop_front();}
    pthread_mutex_unlock(&g_creature_motion_lock);return ok;
}
