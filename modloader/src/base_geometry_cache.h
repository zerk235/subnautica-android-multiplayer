#pragma once

#include "base_geometry_envelope.h"
#include <cmath>

namespace base_geometry {
// Game-thread cache for geometry that can arrive before its build event, or vice versa.
// A snapshot is keyed by origin, exact object ID and the event's content fingerprint.
class GeometryCache {
public:
    static constexpr size_t kSlots = 16;
    static constexpr double kExpirySeconds = 60.0;

    void clear() { for (Entry& entry : slots_) entry = Entry{}; }

    bool put(uint32_t origin, GhostEnvelope envelope, double now, uint64_t* fingerprint) {
        if (!origin || !fingerprint || !std::isfinite(now) || now < 0) return false;
        std::vector<uint8_t> encoded;
        if (!encode_envelope(envelope, &encoded)) return false;
        uint64_t hash = envelope_fingerprint(encoded.data(), encoded.size());
        size_t selected = kSlots;
        for (size_t i = 0; i < kSlots; ++i) {
            const Entry& e = slots_[i];
            if (e.used && e.origin == origin && e.hash == hash &&
                e.envelope.object_id == envelope.object_id) { selected = i; break; }
            if (selected == kSlots && (!e.used || now - e.updated > kExpirySeconds)) selected = i;
        }
        if (selected == kSlots) {
            selected = 0;
            for (size_t i = 1; i < kSlots; ++i)
                if (slots_[i].updated < slots_[selected].updated) selected = i;
        }
        Entry& target = slots_[selected];
        target.used = true; target.origin = origin; target.hash = hash; target.updated = now;
        target.envelope = std::move(envelope);
        *fingerprint = hash;
        return true;
    }

    const GhostEnvelope* find(uint32_t origin, const char* object_id, uint64_t fingerprint,
                              double now) const {
        if (!origin || !object_id || !std::isfinite(now) || now < 0) return nullptr;
        for (const Entry& e : slots_)
            if (e.used && e.origin == origin && e.hash == fingerprint &&
                e.envelope.object_id == object_id && now >= e.updated &&
                now - e.updated <= kExpirySeconds) return &e.envelope;
        return nullptr;
    }
private:
    struct Entry {
        bool used = false;
        uint32_t origin = 0;
        uint64_t hash = 0;
        double updated = 0;
        GhostEnvelope envelope;
    };
    std::array<Entry, kSlots> slots_{};
};
} // namespace base_geometry
