#pragma once

#include "base_geometry_codec.h"
#include <string>
#include <cmath>
#include <cstring>

namespace base_geometry {
constexpr uint32_t kEnvelopeMagic = 0x31454742; // BGE1
constexpr uint32_t kEnvelopeVersion = 3;
constexpr size_t kEnvelopeHeader = 80;
constexpr size_t kMaxObjectId = 127;
constexpr size_t kMaxGhostClass = 63;
constexpr size_t kMaxEnvelope = kEnvelopeHeader + 2 * kMaxObjectId + kMaxGhostClass +
                                kHeaderSize + 10 * kMaxCells;

inline uint64_t envelope_fingerprint(const uint8_t* bytes, size_t length) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < length; ++i) h = (h ^ bytes[i]) * 1099511628211ull;
    return h;
}

struct GhostEnvelope {
    bool creates_base = false; // first piece; target ID is captured after native Finish
    std::array<float,7> base_pose{}; // completed Base world position and quaternion
    std::string object_id;
    std::string target_base_id; // empty only for placement of a new base
    std::string ghost_class;
    std::array<int32_t, 3> target_offset{};
    std::array<int32_t, 3> target_base_anchor{};
    Snapshot geometry;
};

// CopyFrom adds EnsureSize's index shift to Base.anchor. The physical cell is
// therefore (target_offset - target_base_anchor), even after negative expansion.
inline int64_t relative_target_cell(const GhostEnvelope& e,size_t axis) {
    return int64_t(e.target_offset[axis])-e.target_base_anchor[axis];
}
inline bool same_target_cell(const GhostEnvelope& a,const GhostEnvelope& b) {
    if (a.target_base_id.empty() || a.target_base_id!=b.target_base_id) return false;
    for (size_t i=0;i<3;++i) if (relative_target_cell(a,i)!=relative_target_cell(b,i)) return false;
    return true;
}
inline bool remap_target_anchor(const GhostEnvelope& source,const std::array<int32_t,3>& anchor,GhostEnvelope* out) {
    if (!out) return false;
    std::array<int32_t,3> cell{};
    for (size_t i=0;i<3;++i) {
        const auto v=relative_target_cell(source,i)+anchor[i];
        if (anchor[i]<-8192 || anchor[i]>8192 || source.target_base_anchor[i]<-8192 ||
            source.target_base_anchor[i]>8192 || source.target_offset[i]<-8192 ||
            source.target_offset[i]>8192 || v<-8192 || v>8192) return false;
        cell[i]=int32_t(v);
    }
    *out=source;out->target_offset=cell;out->target_base_anchor=anchor;
    return true;
}

inline bool printable_id(const std::string& s, size_t maximum, bool allow_empty) {
    if (s.size() > maximum || (!allow_empty && s.empty())) return false;
    for (unsigned char c : s) if (c < 0x20 || c > 0x7e) return false;
    return true;
}

inline bool valid_base_pose(const GhostEnvelope& e) {
    if (!e.creates_base) {
        for (float v:e.base_pose) if (v!=0) return false;
        return true;
    }
    for (float v:e.base_pose) if (!std::isfinite(v)) return false;
    for (size_t i=0;i<3;++i) if (std::fabs(e.base_pose[i])>100000) return false;
    float norm=0;
    for (size_t i=3;i<7;++i) norm+=e.base_pose[i]*e.base_pose[i];
    return norm>0.99f && norm<1.01f;
}

inline bool encode_envelope(const GhostEnvelope& e, std::vector<uint8_t>* out) {
    if (!out || !printable_id(e.object_id, kMaxObjectId, false) ||
        !printable_id(e.target_base_id, kMaxObjectId, true) ||
        !printable_id(e.ghost_class, kMaxGhostClass, false) ||
        (e.creates_base && e.target_base_id.empty()) || !valid_base_pose(e)) return false;
    std::vector<uint8_t> geometry;
    if (!encode(e.geometry, &geometry)) return false;
    std::vector<uint8_t> bytes;
    bytes.reserve(kEnvelopeHeader + e.object_id.size() + e.target_base_id.size() +
                  e.ghost_class.size() + geometry.size());
    write_u32(bytes, kEnvelopeMagic); write_u32(bytes, kEnvelopeVersion);
    write_u32(bytes, static_cast<uint32_t>(e.object_id.size()));
    write_u32(bytes, static_cast<uint32_t>(e.target_base_id.size()));
    write_u32(bytes, static_cast<uint32_t>(e.ghost_class.size()));
    write_u32(bytes, static_cast<uint32_t>(geometry.size()));
    for (int32_t value : e.target_offset) write_u32(bytes, static_cast<uint32_t>(value));
    for (int32_t value : e.target_base_anchor) write_u32(bytes, static_cast<uint32_t>(value));
    write_u32(bytes,e.creates_base ? 1u : 0u);
    for (float value:e.base_pose) {
        uint32_t bits=0; std::memcpy(&bits,&value,sizeof(bits)); write_u32(bytes,bits);
    }
    for (const auto* text : {&e.object_id, &e.target_base_id, &e.ghost_class})
        bytes.insert(bytes.end(), text->begin(), text->end());
    bytes.insert(bytes.end(), geometry.begin(), geometry.end());
    *out = std::move(bytes);
    return true;
}

inline bool decode_envelope(const uint8_t* data, size_t length, GhostEnvelope* out) {
    if (!data || !out || length < kEnvelopeHeader || length > kMaxEnvelope ||
        read_u32(data) != kEnvelopeMagic || read_u32(data + 4) != kEnvelopeVersion) return false;
    const size_t object_len = read_u32(data + 8), target_len = read_u32(data + 12);
    const size_t class_len = read_u32(data + 16), geometry_len = read_u32(data + 20);
    if (!object_len || object_len > kMaxObjectId || target_len > kMaxObjectId ||
        !class_len || class_len > kMaxGhostClass || geometry_len < kHeaderSize ||
        geometry_len > kHeaderSize + 10 * kMaxCells ||
        length != kEnvelopeHeader + object_len + target_len + class_len + geometry_len) return false;
    GhostEnvelope result;
    const uint32_t flags=read_u32(data+48);
    if (flags>1) return false;
    result.creates_base=flags!=0;
    for (size_t i=0;i<7;++i) {
        const uint32_t bits=read_u32(data+52+i*4);
        std::memcpy(&result.base_pose[i],&bits,sizeof(bits));
    }
    auto signed32 = [](uint32_t value) -> int32_t {
        return value <= uint32_t(std::numeric_limits<int32_t>::max())
            ? static_cast<int32_t>(value) : -1 - static_cast<int32_t>(~value);
    };
    for (size_t i = 0; i < 3; ++i) result.target_offset[i] = signed32(read_u32(data + 24 + i * 4));
    for (size_t i = 0; i < 3; ++i) result.target_base_anchor[i] = signed32(read_u32(data + 36 + i * 4));
    size_t off = kEnvelopeHeader;
    auto read_string = [&](std::string& text, size_t n) {
        text.assign(reinterpret_cast<const char*>(data + off), n); off += n;
    };
    read_string(result.object_id, object_len); read_string(result.target_base_id, target_len);
    read_string(result.ghost_class, class_len);
    if (!printable_id(result.object_id, kMaxObjectId, false) ||
        !printable_id(result.target_base_id, kMaxObjectId, true) ||
        !printable_id(result.ghost_class, kMaxGhostClass, false) ||
        (result.creates_base && result.target_base_id.empty()) || !valid_base_pose(result) ||
        !decode(data + off, geometry_len, &result.geometry)) return false;
    *out = std::move(result);
    return true;
}
} // namespace base_geometry
