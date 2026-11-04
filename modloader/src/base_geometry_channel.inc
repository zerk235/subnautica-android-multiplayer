// Experimental separate UDP channel. This file is included inside net.cpp's anonymous
// namespace after the session and peer helpers are defined. No Unity calls occur here.
constexpr size_t kGeometryTransfers = 4;
constexpr size_t kGeometryReceiveSlots = 32;
constexpr size_t kGeometryInbox = 8;
constexpr double kGeometryExpiry = 60.0;
constexpr size_t kGeometrySessionPrefix = sizeof(PacketHeader) + 16 + 4;
constexpr size_t kGeometryAckBody = 4 + 4 + 4 + 16 + 4 + 1;

struct GeometrySession {
    bool valid = false;
    uint8_t id[16]{};
    uint32_t epoch = 0;
};
struct GeometryPeer {
    bool active = false;
    uint32_t id = 0;
    sockaddr_in addr{};
    base_geometry::ReliableSender sender;
};
struct GeometryTx {
    bool used = false;
    GeometrySession session;
    std::string object_id;
    uint64_t fingerprint = 0;
    double created = 0;
    GeometryPeer peers[kMaxPeers];
};
struct GeometryRx {
    bool used = false, delivered = false, pending = false;
    GeometrySession session;
    base_geometry::TransferKey key;
    base_geometry::ReliableReceiver receiver;
    base_geometry::GhostEnvelope result;
    double updated = 0;
};
struct GeometryInboxEntry {
    GeometrySession session;
    uint32_t origin = 0;
    base_geometry::GhostEnvelope envelope;
};
pthread_mutex_t g_geometry_lock = PTHREAD_MUTEX_INITIALIZER;
GeometryTx g_geometry_tx[kGeometryTransfers];
GeometryRx g_geometry_rx[kGeometryReceiveSlots];
GeometryInboxEntry g_geometry_inbox[kGeometryInbox];
size_t g_geometry_inbox_head = 0, g_geometry_inbox_count = 0;
uint32_t g_geometry_generation = 0;

GeometrySession geometry_session() {
    GeometrySession out;
    pthread_mutex_lock(&g_session.lock);
    out.valid = g_session.local_member && g_session.local_has_world;
    out.epoch = g_session.epoch;
    memcpy(out.id, g_session.session_id, sizeof(out.id));
    pthread_mutex_unlock(&g_session.lock);
    return out;
}
bool geometry_same_session(const GeometrySession& a, const GeometrySession& b) {
    return a.valid && b.valid && a.epoch == b.epoch && memcmp(a.id, b.id, sizeof(a.id)) == 0;
}

bool geometry_peer_allowed(uint32_t id, const sockaddr_in& addr) {
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    int index = peer_index_by_id(id);
    bool allowed = index >= 0 && g_peers[index].raw.sin_addr.s_addr == addr.sin_addr.s_addr &&
                   g_peers[index].raw.sin_port == addr.sin_port &&
                   session_peer_eligible_locked(static_cast<size_t>(index), true);
    pthread_mutex_unlock(&g_peers_lock);
    return allowed;
}

std::vector<PeerState> geometry_peers() {
    std::vector<PeerState> result;
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    for (size_t i = 0; i < kMaxPeers; ++i)
        if (session_peer_eligible_locked(i, true)) result.push_back(g_peers[i]);
    pthread_mutex_unlock(&g_peers_lock);
    return result;
}

bool geometry_enqueue(const base_geometry::GhostEnvelope& envelope) {
    const GeometrySession session = geometry_session();
    if (!session.valid || !g_local_id) return false;
    std::vector<uint8_t> encoded;
    if (!base_geometry::encode_envelope(envelope, &encoded)) return false;
    const uint64_t fingerprint = base_geometry::envelope_fingerprint(encoded.data(), encoded.size());
    const auto peers = geometry_peers();
    if (peers.empty()) return false;
    pthread_mutex_lock(&g_geometry_lock);
    for (const auto& t : g_geometry_tx) {
        if (t.used && geometry_same_session(t.session, session) &&
            t.object_id == envelope.object_id && t.fingerprint == fingerprint) {
            pthread_mutex_unlock(&g_geometry_lock);
            return true; // an immutable copy is already in flight
        }
    }
    GeometryTx* slot = nullptr;
    for (auto& t : g_geometry_tx) if (!t.used) { slot = &t; break; }
    if (!slot) { pthread_mutex_unlock(&g_geometry_lock); return false; }
    *slot = GeometryTx{};
    slot->session = session; slot->object_id = envelope.object_id; slot->fingerprint = fingerprint;
    slot->created = monotonic_seconds();
    if (++g_geometry_generation == 0) ++g_geometry_generation;
    base_geometry::TransferKey key;
    key.origin = g_local_id; key.generation = g_geometry_generation;
    key.object = base_geometry::object_key(envelope.object_id);
    size_t accepted = 0;
    for (size_t i = 0; i < peers.size() && i < kMaxPeers; ++i) {
        GeometryPeer& p = slot->peers[i];
        if (!p.sender.start(key, envelope)) continue;
        p.id = peers[i].id; p.addr = peers[i].raw; p.active = true; ++accepted;
    }
    slot->used = accepted != 0;
    if (!slot->used) *slot = GeometryTx{};
    pthread_mutex_unlock(&g_geometry_lock);
    return accepted != 0;
}

bool geometry_take(uint32_t* origin, base_geometry::GhostEnvelope* out) {
    if (!origin || !out) return false;
    const GeometrySession session = geometry_session();
    pthread_mutex_lock(&g_geometry_lock);
    while (g_geometry_inbox_count) {
        auto& entry = g_geometry_inbox[g_geometry_inbox_head];
        bool current = geometry_same_session(entry.session, session);
        if (current) { *origin = entry.origin; *out = std::move(entry.envelope); }
        entry = GeometryInboxEntry{};
        g_geometry_inbox_head = (g_geometry_inbox_head + 1) % kGeometryInbox;
        --g_geometry_inbox_count;
        if (current) { pthread_mutex_unlock(&g_geometry_lock); return true; }
    }
    pthread_mutex_unlock(&g_geometry_lock);
    return false;
}

void geometry_add_prefix(std::vector<uint8_t>* out, uint8_t type, const GeometrySession& session) {
    PacketHeader header{}; header.magic = kMagic; header.type = type;
    const auto* p = reinterpret_cast<const uint8_t*>(&header);
    out->insert(out->end(), p, p + sizeof(header));
    out->insert(out->end(), session.id, session.id + sizeof(session.id));
    base_geometry::write_u32(*out, session.epoch);
}

void geometry_service(double now) {
    const GeometrySession session = geometry_session();
    const auto peers = geometry_peers();
    pthread_mutex_lock(&g_geometry_lock);
    for (auto& rx : g_geometry_rx)
        if (rx.used && (!geometry_same_session(rx.session, session) || now - rx.updated > kGeometryExpiry))
            rx = GeometryRx{};
    for (auto& tx : g_geometry_tx) {
        if (!tx.used) continue;
        if (!geometry_same_session(tx.session, session) || now - tx.created > kGeometryExpiry) {
            tx = GeometryTx{}; continue;
        }
        bool pending = false;
        for (auto& peer : tx.peers) {
            if (!peer.active) continue;
            if (peer.sender.done()) { peer.active = false; continue; }
            pending = true;
            for (const auto& current : peers) {
                if (current.id != peer.id) continue;
                peer.addr = current.raw;
                std::vector<uint8_t> fragment;
                if (peer.sender.frame(now, &fragment)) {
                    std::vector<uint8_t> packet;
                    packet.reserve(kGeometrySessionPrefix + fragment.size());
                    geometry_add_prefix(&packet, kPacketGeometryFragment, tx.session);
                    packet.insert(packet.end(), fragment.begin(), fragment.end());
                    send_to(packet.data(), packet.size(), peer.addr);
                }
                break;
            }
        }
        if (!pending) tx = GeometryTx{};
    }
    pthread_mutex_unlock(&g_geometry_lock);
}

void geometry_handle_packet(const uint8_t* data, size_t size, const sockaddr_in& addr, double now) {
    if (size < kGeometrySessionPrefix) return;
    PacketHeader header{}; memcpy(&header, data, sizeof(header));
    if (header.magic != kMagic) return;
    const GeometrySession session = geometry_session();
    if (!session.valid || memcmp(data + sizeof(header), session.id, 16) != 0 ||
        base_geometry::read_u32(data + sizeof(header) + 16) != session.epoch) return;
    const uint8_t* body = data + kGeometrySessionPrefix;
    const size_t length = size - kGeometrySessionPrefix;
    if (header.type == kPacketGeometryAck) {
        if (length != kGeometryAckBody) return;
        const uint32_t sender = base_geometry::read_u32(body);
        base_geometry::Ack ack;
        ack.key.origin = base_geometry::read_u32(body + 4);
        ack.key.generation = base_geometry::read_u32(body + 8);
        memcpy(ack.key.object.data(), body + 12, ack.key.object.size());
        ack.index = base_geometry::read_u32(body + 28);
        ack.status = static_cast<base_geometry::AckStatus>(body[32]);
        if (ack.key.origin != g_local_id || !geometry_peer_allowed(sender, addr)) return;
        pthread_mutex_lock(&g_geometry_lock);
        for (auto& tx : g_geometry_tx) if (tx.used && geometry_same_session(tx.session, session))
            for (auto& peer : tx.peers)
                if (peer.active && peer.id == sender) peer.sender.acknowledge(ack);
        pthread_mutex_unlock(&g_geometry_lock);
        return;
    }
    if (header.type != kPacketGeometryFragment || length < base_geometry::kFragmentHeader ||
        base_geometry::read_u32(body) != base_geometry::kFragmentMagic) return;
    base_geometry::TransferKey key;
    key.origin = base_geometry::read_u32(body + 4); key.generation = base_geometry::read_u32(body + 8);
    memcpy(key.object.data(), body + 12, key.object.size());
    if (!geometry_peer_allowed(key.origin, addr)) return;
    pthread_mutex_lock(&g_geometry_lock);
    GeometryRx* rx = nullptr;
    for (auto& candidate : g_geometry_rx)
        if (candidate.used && geometry_same_session(candidate.session, session) && candidate.key == key) {
            rx = &candidate; break;
        }
    bool created = false;
    if (!rx && base_geometry::read_u32(body + 36) == 0) {
        for (auto& candidate : g_geometry_rx) if (!candidate.used) { rx = &candidate; break; }
        // Completed transfers must not occupy every slot for a full timeout while a
        // player is building several pieces. Never evict an incomplete transfer.
        if (!rx) for (auto& candidate : g_geometry_rx)
            if (candidate.delivered && (!rx || candidate.updated < rx->updated)) rx = &candidate;
        if (rx) {
            *rx = GeometryRx{}; rx->used = true; rx->session = session; rx->key = key;
            created = true;
        }
    }
    if (!rx) { pthread_mutex_unlock(&g_geometry_lock); return; }
    base_geometry::Ack ack;
    base_geometry::GhostEnvelope result;
    if (!rx->receiver.receive(body, length, &ack, &result)) {
        if (created) *rx = GeometryRx{};
        pthread_mutex_unlock(&g_geometry_lock); return;
    }
    rx->updated = now;
    if (ack.status == base_geometry::AckStatus::complete && !rx->delivered) {
        if (!result.object_id.empty()) { rx->result = std::move(result); rx->pending = true; }
        if (!rx->pending || g_geometry_inbox_count == kGeometryInbox) {
            pthread_mutex_unlock(&g_geometry_lock); return; // no ACK until admitted to inbox
        }
        size_t index = (g_geometry_inbox_head + g_geometry_inbox_count) % kGeometryInbox;
        g_geometry_inbox[index].session = session;
        g_geometry_inbox[index].origin = key.origin;
        g_geometry_inbox[index].envelope = std::move(rx->result);
        ++g_geometry_inbox_count; rx->pending = false; rx->delivered = true;
    }
    pthread_mutex_unlock(&g_geometry_lock);
    std::vector<uint8_t> response;
    geometry_add_prefix(&response, kPacketGeometryAck, session);
    base_geometry::write_u32(response, g_local_id);
    base_geometry::write_u32(response, ack.key.origin);
    base_geometry::write_u32(response, ack.key.generation);
    response.insert(response.end(), ack.key.object.begin(), ack.key.object.end());
    base_geometry::write_u32(response, ack.index);
    response.push_back(static_cast<uint8_t>(ack.status));
    send_to(response.data(), response.size(), addr);
}
