#pragma once

// Experimental framing for one geometry snapshot. Session authorization, retries,
// acknowledgements and binding a snapshot to its placement are the caller's job.
#include "base_geometry_envelope.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace base_geometry {
constexpr uint32_t kFragmentMagic = 0x31464742; // BGF1
constexpr size_t kFragmentHeader = 44;
constexpr size_t kFragmentPayload = 960;
constexpr size_t kMaxBlob = kMaxEnvelope;
constexpr size_t kMaxFragments = (kMaxBlob + kFragmentPayload - 1) / kFragmentPayload;

struct TransferKey {
    uint32_t origin = 0;
    uint32_t generation = 0;
    std::array<uint8_t, 16> object{};
    bool operator==(const TransferKey& other) const {
        return origin == other.origin && generation == other.generation && object == other.object;
    }
};

inline uint32_t checksum(const uint8_t* data, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ data[i]) * 16777619u;
    return h;
}

inline bool make_fragment(const TransferKey& key, const std::vector<uint8_t>& blob,
                          size_t index, std::vector<uint8_t>* out) {
    if (!out || key.origin == 0 || key.generation == 0 || blob.empty() || blob.size() > kMaxBlob)
        return false;
    size_t count = (blob.size() + kFragmentPayload - 1) / kFragmentPayload;
    if (index >= count) return false;
    const size_t offset = index * kFragmentPayload;
    const size_t length = std::min(kFragmentPayload, blob.size() - offset);
    std::vector<uint8_t> frame;
    frame.reserve(kFragmentHeader + length);
    write_u32(frame, kFragmentMagic); write_u32(frame, key.origin); write_u32(frame, key.generation);
    frame.insert(frame.end(), key.object.begin(), key.object.end());
    write_u32(frame, static_cast<uint32_t>(blob.size()));
    write_u32(frame, checksum(blob.data(), blob.size()));
    write_u32(frame, static_cast<uint32_t>(offset));
    write_u32(frame, static_cast<uint32_t>(length));
    frame.insert(frame.end(), blob.begin() + offset, blob.begin() + offset + length);
    *out = std::move(frame);
    return true;
}

class Reassembler {
public:
    // A reassembler is single-transfer. Do not feed it packets from an unauthenticated peer.
    bool accept(const uint8_t* frame, size_t size) {
        if (!frame || size < kFragmentHeader || read_u32(frame) != kFragmentMagic) return false;
        TransferKey incoming;
        incoming.origin = read_u32(frame + 4); incoming.generation = read_u32(frame + 8);
        std::memcpy(incoming.object.data(), frame + 12, incoming.object.size());
        const uint32_t total = read_u32(frame + 28), hash = read_u32(frame + 32);
        const uint32_t offset = read_u32(frame + 36), n = read_u32(frame + 40);
        if (!incoming.origin || !incoming.generation || total == 0 || total > kMaxBlob ||
            offset >= total || offset % kFragmentPayload != 0 || n == 0 ||
            n != std::min(kFragmentPayload, size_t(total - offset)) ||
            size != kFragmentHeader + size_t(n)) return false;
        if (!started_) {
            key_ = incoming; total_ = total; hash_ = hash;
            bytes_.resize(total); started_ = true;
        } else if (!(incoming == key_) || total != total_ || hash != hash_) {
            return false;
        }
        const size_t index = offset / kFragmentPayload;
        if (received_[index])
            return std::memcmp(bytes_.data() + offset, frame + kFragmentHeader, n) == 0;
        std::memcpy(bytes_.data() + offset, frame + kFragmentHeader, n);
        received_[index] = true;
        ++received_count_;
        return true;
    }

    bool finish(Snapshot* out) const {
        if (!complete()) return false;
        return decode(bytes_.data(), bytes_.size(), out);
    }

    bool finish(GhostEnvelope* out) const {
        if (!complete()) return false;
        return decode_envelope(bytes_.data(), bytes_.size(), out);
    }

private:
    bool complete() const {
        return started_ && received_count_ == (total_ + kFragmentPayload - 1) / kFragmentPayload &&
               checksum(bytes_.data(), bytes_.size()) == hash_;
    }
    bool started_ = false;
    TransferKey key_{};
    uint32_t total_ = 0, hash_ = 0;
    size_t received_count_ = 0;
    std::array<bool, kMaxFragments> received_{};
    std::vector<uint8_t> bytes_;
};
} // namespace base_geometry
