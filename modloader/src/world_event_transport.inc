// Ordered, bounded event history. Discovery slots and IPs are transient;
// delivery watermarks belong to a process in an exact session.
struct EventPeerView {
    uint32_t id;
    sockaddr_in address;
    uint8_t session_id[16];
    uint32_t epoch;
};

size_t event_peer_views(EventPeerView out[kMaxPeers]) {
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    size_t count = 0;
    for (size_t i = 0; i < kMaxPeers; ++i) {
        if (!session_peer_eligible_locked(i, true)) continue;
        auto& view = out[count++];
        view.id = g_peers[i].id;
        view.address = g_peers[i].raw;
        memcpy(view.session_id, g_remote[i].session_id, 16);
        view.epoch = g_remote[i].epoch;
    }
    pthread_mutex_unlock(&g_peers_lock);
    return count;
}

// Success returns with event_lock held; order is session -> event. A temporary
// menu/world gate retains history. A different session or epoch clears history
// AND inbox before either networking or the game thread can use old commands.
bool event_lock_context() {
    pthread_mutex_lock(&g_session.lock);
    pthread_mutex_lock(&g_event_lock);
    bool valid = g_session.local_member && g_session.local_has_world &&
                 g_session.epoch && bytes_nonzero(g_session.session_id, 16);
    if (valid && (!g_events.context_valid || g_events.epoch != g_session.epoch ||
                  !same_session(g_events.session_id, g_session.session_id))) {
        memset(&g_events, 0, sizeof(g_events));
        g_event_inbox_head = g_event_inbox_tail = 0;
        g_events.context_valid = true;
        memcpy(g_events.session_id, g_session.session_id, 16);
        g_events.epoch = g_session.epoch;
        if (++g_event_generation == 0) ++g_event_generation;
        g_events.stream_id = g_event_generation;
        LOG_INFO("event stream: new context epoch=%u stream=%u", g_events.epoch,
                 g_events.stream_id);
    }
    pthread_mutex_unlock(&g_session.lock);
    if (!valid) pthread_mutex_unlock(&g_event_lock);
    return valid;
}

bool event_view_matches(const EventPeerView& view) {
    return view.epoch == g_events.epoch && same_session(view.session_id, g_events.session_id);
}

EventRecipient* event_recipient(uint32_t id, bool create) {
    for (auto& r : g_events.recipients)
        if (r.used && r.id == id) return &r;
    if (!create) return nullptr;
    for (auto& r : g_events.recipients) {
        if (r.used) continue;
        r.used = true;
        r.id = id;
        r.first_sequence = g_events.next_sequence + 1;
        r.acknowledged = r.sent_through = g_events.next_sequence;
        r.last_send = r.last_status = r.last_report = -1;
        LOG_INFO("event stream: peer=%08x start=%u (new recipient)", id, r.first_sequence);
        return &r;
    }
    // Never recycle a disconnected recipient into an unsafe new baseline.
    static double last_capacity_log = -10;
    const double now = monotonic_seconds();
    if (now - last_capacity_log >= 5) {
        last_capacity_log = now;
        LOG_ERROR("event recipient_capacity: save host and start a fresh session");
    }
    return nullptr;
}

EventStreamStamp event_stamp(const EventRecipient& r) {
    EventStreamStamp stamp{};
    memcpy(stamp.session_id, g_events.session_id, 16);
    stamp.epoch = g_events.epoch;
    stamp.stream_id = g_events.stream_id;
    stamp.first_sequence = r.first_sequence;
    return stamp;
}

uint32_t event_oldest_sequence() {
    return g_events.next_sequence >= kEventRingSize
        ? g_events.next_sequence - kEventRingSize + 1 : 1;
}

void event_history_lost(EventRecipient& r) {
    if (r.history_lost) return;
    r.history_lost = true;
    LOG_ERROR("event history_gap: peer=%08x missing=%u oldest=%u latest=%u; "
              "save host and rejoin from a fresh world snapshot", r.id,
              r.acknowledged + 1, event_oldest_sequence(), g_events.next_sequence);
}

bool queue_local_event(const WorldEvent& event, bool /*already_applied*/) {
    EventPeerView peers[kMaxPeers]{};
    const size_t count = event_peer_views(peers);
    if (!event_lock_context()) return false;
    // Register before numbering even if the action falls between pump iterations.
    // Previously known offline recipients persist.
    for (size_t i = 0; i < count; ++i)
        if (event_view_matches(peers[i])) event_recipient(peers[i].id, true);
    if (g_events.next_sequence == UINT32_MAX) {
        LOG_ERROR("event sequence_exhausted: save host and start a fresh session");
        pthread_mutex_unlock(&g_event_lock);
        return false;
    }
    const uint32_t sequence = ++g_events.next_sequence;
    auto& slot = g_events.slots[(sequence - 1) & kEventRingMask];
    slot.used = true;
    slot.event = event;
    slot.event.origin_id = g_local_id;
    slot.event.sequence = sequence;
    for (auto& r : g_events.recipients)
        if (r.used && r.acknowledged + 1 < event_oldest_sequence()) event_history_lost(r);
    pthread_mutex_unlock(&g_event_lock);
    return true;
}

void send_world_event(const EventSlot& slot, const EventRecipient& r,
                      const sockaddr_in& address) {
    WorldEventPacket packet{};
    packet.header.magic = kMagic;
    packet.header.type = kPacketWorldEvent;
    packet.id = g_local_id;
    static_assert(offsetof(WorldEventPacket, stream) == 12 + sizeof(WorldEvent));
    memcpy(&packet.origin_id, &slot.event, sizeof(WorldEvent));
    packet.stream = event_stamp(r);
    send_to(&packet, sizeof(packet), address);
    ++g_event_tx;
}

void service_world_events(double now) {
    EventPeerView peers[kMaxPeers]{};
    const size_t count = event_peer_views(peers);
    if (!event_lock_context()) return;
    for (size_t i = 0; i < count; ++i) {
        if (!event_view_matches(peers[i])) continue;
        auto* r = event_recipient(peers[i].id, true);
        if (!r) continue;
        if (r->acknowledged + 1 < event_oldest_sequence()) event_history_lost(*r);
        if (r->last_report < 0 || now - r->last_report >= 15) {
            LOG_INFO("event stream: peer=%08x stream=%u start=%u ack=%u latest=%u "
                     "oldest=%u backlog=%u history_lost=%d", r->id, g_events.stream_id,
                     r->first_sequence, r->acknowledged, g_events.next_sequence,
                     event_oldest_sequence(), g_events.next_sequence - r->acknowledged,
                     r->history_lost ? 1 : 0);
            r->last_report = now;
        }
        if (r->last_status < 0 || now - r->last_status >= 1) {
            WorldEventStatusPacket status{};
            status.header.magic = kMagic;
            status.header.type = kPacketWorldEventStatus;
            status.id = g_local_id;
            status.stream = event_stamp(*r);
            status.latest_sequence = g_events.next_sequence;
            status.oldest_sequence = event_oldest_sequence();
            send_to(&status, sizeof(status), peers[i].address);
            r->last_status = now;
        }
        if (r->history_lost || (r->last_send >= 0 && now - r->last_send < kEventResendInterval))
            continue;
        // Small sliding window, always from the missing prefix in sequence order.
        // There is no time expiry, including when discovery retires the peer slot.
        uint32_t sequence = r->acknowledged;
        for (int n = 0; n < kEventSendWindow && sequence < g_events.next_sequence; ++n) {
            ++sequence;
            const auto& slot = g_events.slots[(sequence - 1) & kEventRingMask];
            if (!slot.used || slot.event.sequence != sequence) { event_history_lost(*r); break; }
            send_world_event(slot, *r, peers[i].address);
            if (sequence > r->sent_through) r->sent_through = sequence;
        }
        r->last_send = now;
    }
    pthread_mutex_unlock(&g_event_lock);
}

EventOrigin* event_origin(uint32_t id, const EventStreamStamp& stamp) {
    EventOrigin* origin = nullptr;
    for (auto& candidate : g_events.origins) {
        if (candidate.used && candidate.id == id) { origin = &candidate; break; }
        if (!candidate.used && !origin) origin = &candidate;
    }
    if (!origin) return nullptr;
    if (origin->used) {
        if (stamp.stream_id == origin->stream_id)
            return stamp.first_sequence == origin->first_sequence ? origin : nullptr;
        if (static_cast<int32_t>(stamp.stream_id - origin->stream_id) <= 0) return nullptr;
    }
    memset(origin, 0, sizeof(*origin));
    origin->used = origin->seq_valid = true;
    origin->id = id;
    origin->stream_id = stamp.stream_id;
    origin->first_sequence = origin->next_sequence = stamp.first_sequence;
    LOG_INFO("event stream: origin=%08x stream=%u next=%u", id, stamp.stream_id,
             origin->next_sequence);
    return origin;
}

void handle_event_packet(const uint8_t* data, size_t size, const sockaddr_in& address) {
    if (size < sizeof(PacketHeader)) return;
    PacketHeader header{};
    memcpy(&header, data, sizeof(header));
    if (header.magic != kMagic || !header_reserved_zero(header)) return;
    uint32_t sender = 0;
    EventStreamStamp stamp{};
    WorldEventPacket packet{};
    WorldEventAckPacket ack{};
    WorldEventStatusPacket status{};
    if (header.type == kPacketWorldEvent && size == sizeof(packet)) {
        memcpy(&packet, data, size); sender = packet.id; stamp = packet.stream;
        // Relays are not implemented; peers cannot manufacture other origins.
        if (packet.origin_id != sender || !event_kind_known(packet.kind) ||
            packet.sequence < stamp.first_sequence) return;
    } else if (header.type == kPacketWorldEventAck && size == sizeof(ack)) {
        memcpy(&ack, data, size); sender = ack.id; stamp = ack.stream;
        if (ack.origin_id != g_local_id) return;
    } else if (header.type == kPacketWorldEventStatus && size == sizeof(status)) {
        memcpy(&status, data, size); sender = status.id; stamp = status.stream;
        if (status.latest_sequence == UINT32_MAX || !status.oldest_sequence ||
            status.oldest_sequence > status.latest_sequence + 1 ||
            stamp.first_sequence > status.latest_sequence + 1) return;
    } else return;
    if (!sender || sender == g_local_id || !stamp.stream_id || !stamp.first_sequence) return;
    pthread_once(&g_peers_lock_once, init_peers_lock);
    pthread_mutex_lock(&g_peers_lock);
    const int index = peer_index_by_id(sender);
    const bool allowed = index >= 0 && session_peer_eligible_locked(index, true) &&
        g_peers[index].raw.sin_addr.s_addr == address.sin_addr.s_addr &&
        g_peers[index].raw.sin_port == address.sin_port;
    pthread_mutex_unlock(&g_peers_lock);
    if (!allowed || !event_lock_context()) return;
    if (stamp.epoch != g_events.epoch || !same_session(stamp.session_id, g_events.session_id)) {
        pthread_mutex_unlock(&g_event_lock);
        return;
    }
    if (header.type == kPacketWorldEventAck) {
        auto* r = event_recipient(sender, false);
        if (r && stamp.stream_id == g_events.stream_id &&
            stamp.first_sequence == r->first_sequence &&
            ack.sequence >= r->acknowledged && ack.sequence <= r->sent_through) {
            if (r->acknowledged != ack.sequence) {
                LOG_INFO("event ack: peer=%08x prefix=%u latest=%u", sender, ack.sequence,
                         g_events.next_sequence);
                r->acknowledged = ack.sequence;
                r->last_send = -1;
            }
        }
        pthread_mutex_unlock(&g_event_lock);
        return;
    }
    auto* origin = event_origin(sender, stamp);
    if (!origin) { pthread_mutex_unlock(&g_event_lock); return; }
    if (!origin->history_lost) drain_pending(origin);
    if (header.type == kPacketWorldEventStatus) {
        if (!origin->history_lost && status.oldest_sequence > origin->next_sequence) {
            origin->history_lost = true;
            LOG_ERROR("event history_gap: origin=%08x missing=%u oldest=%u latest=%u; "
                      "save host and rejoin from a fresh world snapshot", sender,
                      origin->next_sequence, status.oldest_sequence, status.latest_sequence);
        }
    } else if (!origin->history_lost) {
        WorldEvent event{};
        memcpy(&event, &packet.origin_id, sizeof(event));
        if (event.sequence < origin->next_sequence || origin_has_pending(origin, event.sequence)) {
            ++g_event_dup;
        } else if (event.sequence == origin->next_sequence) {
            if (inbox_push(event)) {
                ++origin->next_sequence; ++g_event_rx;
                LOG_INFO("event: %s %u from %08x", event_kind_name(event.kind), event.sequence, sender);
                drain_pending(origin);
            }
        } else if (event.sequence - origin->next_sequence < kEventPendingSize) {
            origin_store_pending(origin, event);
        }
    }
    // ACK only the contiguous inbox prefix. Idle status also drains pending data
    // after the game thread frees inbox space, without needing another command.
    ack = {};
    ack.header.magic = kMagic;
    ack.header.type = kPacketWorldEventAck;
    ack.id = g_local_id;
    ack.origin_id = sender;
    ack.stream = stamp;
    ack.sequence = origin->next_sequence - 1;
    pthread_mutex_unlock(&g_event_lock);
    send_to(&ack, sizeof(ack), address);
}
