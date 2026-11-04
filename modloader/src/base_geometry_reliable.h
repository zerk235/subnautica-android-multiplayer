#pragma once

// Stop-and-wait state for a single peer and one immutable geometry envelope.
// UDP I/O, session/peer authorization and bounded per-peer queues remain external.
#include "base_geometry_transfer.h"
#include <cmath>

namespace base_geometry {
constexpr double kResendSeconds = 0.35;

inline std::array<uint8_t, 16> object_key(const std::string& id) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char ch : id) { hash ^= ch; hash *= 1099511628211ull; }
    const uint64_t tail = 0x9E3779B97F4A7C15ull ^ id.size();
    std::array<uint8_t, 16> out{};
    for (unsigned i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>(hash >> (i * 8));
        out[i + 8] = static_cast<uint8_t>(tail >> (i * 8));
    }
    return out;
}

enum class AckStatus : uint8_t { chunk = 1, complete = 2, restart = 3 };
struct Ack { TransferKey key{}; uint32_t index = 0; AckStatus status = AckStatus::chunk; };

class ReliableSender {
public:
    bool start(const TransferKey& key, const GhostEnvelope& envelope) {
        if ((started_ && !done_) || !key.origin || !key.generation ||
            key.object != object_key(envelope.object_id)) return false;
        std::vector<uint8_t> bytes;
        if (!encode_envelope(envelope, &bytes)) return false;
        key_ = key; blob_ = std::move(bytes); index_ = 0; last_sent_ = -1;
        done_ = false; started_ = true;
        return true;
    }
    bool frame(double now, std::vector<uint8_t>* out) {
        if (!started_ || done_ || !std::isfinite(now) || now < 0 ||
            (last_sent_ >= 0 && now - last_sent_ < kResendSeconds)) return false;
        if (!make_fragment(key_, blob_, index_, out)) return false;
        last_sent_ = now;
        return true;
    }
    bool acknowledge(const Ack& ack) {
        if (!started_ || done_ || last_sent_ < 0 || !(ack.key == key_) || ack.index != index_)
            return false;
        const size_t count = (blob_.size() + kFragmentPayload - 1) / kFragmentPayload;
        if (ack.status == AckStatus::restart) { index_ = 0; last_sent_ = -1; return true; }
        if (index_ + 1 == count) {
            if (ack.status != AckStatus::complete) return false;
            done_ = true;
        } else {
            if (ack.status != AckStatus::chunk) return false;
            ++index_;
        }
        last_sent_ = -1;
        return true;
    }
    bool done() const { return done_; }
private:
    TransferKey key_{};
    std::vector<uint8_t> blob_;
    size_t index_ = 0;
    double last_sent_ = -1;
    bool started_ = false, done_ = false;
};

class ReliableReceiver {
public:
    // Only call after the enclosing UDP layer has authenticated peer ID, address and session.
    bool receive(const uint8_t* frame, size_t length, Ack* ack, GhostEnvelope* result) {
        if (!frame || !ack || !result || length < kFragmentHeader ||
            read_u32(frame) != kFragmentMagic) return false;
        TransferKey key;
        key.origin = read_u32(frame + 4); key.generation = read_u32(frame + 8);
        std::memcpy(key.object.data(), frame + 12, key.object.size());
        const uint32_t total = read_u32(frame + 28), hash = read_u32(frame + 32);
        const uint32_t offset = read_u32(frame + 36), n = read_u32(frame + 40);
        if (!key.origin || !key.generation || !total || total > kMaxBlob ||
            offset % kFragmentPayload != 0 || offset >= total || !n ||
            n != std::min(kFragmentPayload, size_t(total - offset)) ||
            length != kFragmentHeader + size_t(n))
            return false;
        const size_t index = offset / kFragmentPayload;
        // The restart ACK may have been lost. Until fragment zero starts a new pass,
        // repeat the negative ACK for the last fragment of the failed pass.
        if (restart_pending_ && index != 0 && key == restart_key_ && index == restart_index_ &&
            total == restart_total_ && hash == restart_hash_) {
            ack->key = key; ack->index = static_cast<uint32_t>(index);
            ack->status = AckStatus::restart;
            return true;
        }
        if (index > next_index_ || (started_ && !(key == key_))) return false;
        if (!parts_.accept(frame, length)) return false;
        if (!started_) { key_ = key; started_ = true; restart_pending_ = false; }
        ack->key = key; ack->index = static_cast<uint32_t>(index);
        const size_t count = (size_t(total) + kFragmentPayload - 1) / kFragmentPayload;
        if (index < next_index_) {
            ack->status = completed_ && index + 1 == count ? AckStatus::complete : AckStatus::chunk;
            return true;
        }
        if (index + 1 < count) { ++next_index_; ack->status = AckStatus::chunk; return true; }
        GhostEnvelope decoded;
        if (!parts_.finish(&decoded) || object_key(decoded.object_id) != key.object) {
            parts_ = Reassembler{}; next_index_ = 0; started_ = false; completed_ = false;
            restart_pending_ = true; restart_key_ = key; restart_index_ = index;
            restart_total_ = total; restart_hash_ = hash;
            ack->status = AckStatus::restart;
            return true;
        }
        ++next_index_; completed_ = true; *result = std::move(decoded);
        ack->status = AckStatus::complete;
        return true;
    }
private:
    Reassembler parts_;
    TransferKey key_{};
    TransferKey restart_key_{};
    size_t next_index_ = 0;
    size_t restart_index_ = 0;
    uint32_t restart_total_ = 0, restart_hash_ = 0;
    bool started_ = false, completed_ = false, restart_pending_ = false;
};
} // namespace base_geometry
