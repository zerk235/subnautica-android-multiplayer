#pragma once

// Experimental, transport-independent representation of a Base ghost's topology.
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace base_geometry {
constexpr uint32_t kMagic = 0x31475342; // BSG1 in little endian
constexpr uint32_t kVersion = 2;
constexpr uint32_t kMasksAbsent = 0x80000000u;
constexpr size_t kMaxCells = 4096;
constexpr size_t kHeaderSize = 48;

struct Snapshot {
    std::array<int32_t, 3> shape{};
    std::array<int32_t, 3> cell_offset{};
    std::array<int32_t, 3> anchor{};
    // Six face types per cell, followed by one value per cell in each array.
    std::vector<uint8_t> faces, cells, links, masks, glass;
};

inline bool cell_count(const std::array<int32_t, 3>& shape, size_t* out) {
    size_t count = 1;
    for (int32_t dim : shape) {
        if (dim <= 0 || static_cast<size_t>(dim) > kMaxCells / count) return false;
        count *= static_cast<size_t>(dim);
    }
    *out = count;
    return true;
}

inline bool valid(const Snapshot& s) {
    size_t n;
    if (!cell_count(s.shape, &n) || s.faces.size() != n * 6 || s.cells.size() != n ||
        s.links.size() != n || (!s.masks.empty() && s.masks.size() != n) || s.glass.size() != n) return false;
    // Android 1.22.83315 dump: CellType 0..19; FaceType 0..20 and occupied faces 128..133.
    for (uint8_t value : s.cells) if (value > 19) return false;
    for (uint8_t value : s.faces)
        if (value > 20 && (value < 128 || value > 133)) return false;
    for (uint8_t value : s.glass) if (value > 1) return false;
    return true;
}

inline void write_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
inline uint32_t read_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline bool encode(const Snapshot& s, std::vector<uint8_t>* out) {
    if (!out || !valid(s)) return false;
    size_t n;
    if (!cell_count(s.shape, &n)) return false;
    std::vector<uint8_t> bytes;
    bytes.reserve(kHeaderSize + 10 * n);
    write_u32(bytes, kMagic); write_u32(bytes, kVersion);
    write_u32(bytes, static_cast<uint32_t>(n) | (s.masks.empty() ? kMasksAbsent : 0));
    for (int32_t v : s.shape) write_u32(bytes, static_cast<uint32_t>(v));
    for (int32_t v : s.cell_offset) write_u32(bytes, static_cast<uint32_t>(v));
    for (int32_t v : s.anchor) write_u32(bytes, static_cast<uint32_t>(v));
    for (const auto* a : {&s.faces, &s.cells, &s.links})
        bytes.insert(bytes.end(), a->begin(), a->end());
    if (s.masks.empty()) bytes.insert(bytes.end(), n, 0);
    else bytes.insert(bytes.end(), s.masks.begin(), s.masks.end());
    bytes.insert(bytes.end(), s.glass.begin(), s.glass.end());
    *out = std::move(bytes);
    return true;
}

inline bool decode(const uint8_t* data, size_t len, Snapshot* out) {
    if (!data || !out || len < kHeaderSize || read_u32(data) != kMagic ||
        read_u32(data + 4) != kVersion) return false;
    const uint32_t declared_field = read_u32(data + 8);
    const uint32_t declared = declared_field & ~kMasksAbsent;
    Snapshot s;
    size_t off = 12;
    // Decode signed fields without implementation-defined unsigned-to-signed conversion.
    auto signed32 = [](uint32_t value) -> int32_t {
        return value <= uint32_t(std::numeric_limits<int32_t>::max())
            ? static_cast<int32_t>(value)
            : -1 - static_cast<int32_t>(~value);
    };
    for (int32_t& v : s.shape) { v = signed32(read_u32(data + off)); off += 4; }
    for (int32_t& v : s.cell_offset) { v = signed32(read_u32(data + off)); off += 4; }
    for (int32_t& v : s.anchor) { v = signed32(read_u32(data + off)); off += 4; }
    size_t n;
    if (!cell_count(s.shape, &n) || declared != n || len != kHeaderSize + 10 * n) return false;
    auto copy = [&](std::vector<uint8_t>& a, size_t amount) {
        a.assign(data + off, data + off + amount); off += amount;
    };
    copy(s.faces, n * 6); copy(s.cells, n); copy(s.links, n); copy(s.masks, n); copy(s.glass, n);
    if (declared_field & kMasksAbsent) {
        for (uint8_t v : s.masks) if (v != 0) return false;
        s.masks.clear();
    }
    if (!valid(s)) return false;
    *out = std::move(s);
    return true;
}
} // namespace base_geometry
