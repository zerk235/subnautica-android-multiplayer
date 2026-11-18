#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace guest_progress {

constexpr size_t kMaxScene = 8u * 1024u * 1024u;
using Bytes = std::vector<uint8_t>;
using View = std::basic_string_view<uint8_t>;

inline View view(const Bytes& bytes) { return {bytes.data(), bytes.size()}; }

inline bool varint(View& input, uint64_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 64 && !input.empty(); shift += 7) {
        const uint8_t byte = input.front();
        input.remove_prefix(1);
        if (shift == 63 && byte > 1) return false;
        value |= uint64_t(byte & 127) << shift;
        if (!(byte & 128)) return true;
    }
    return false;
}

inline bool blob(View& input, View& value) {
    uint64_t length = 0;
    if (!varint(input, length) || length > input.size()) return false;
    value = input.substr(0, size_t(length));
    input.remove_prefix(size_t(length));
    return true;
}

struct Field {
    uint32_t number = 0;
    uint8_t wire = 0;
    uint64_t scalar = 0;
    View data, encoded;
};

inline bool field(View& input, Field& out) {
    const View before = input;
    uint64_t key = 0;
    out = {};
    if (!varint(input, key) || key >> 3 == 0 || key >> 3 >= (1u << 29)) return false;
    out.number = uint32_t(key >> 3);
    out.wire = uint8_t(key & 7);
    switch (out.wire) {
        case 0: if (!varint(input, out.scalar)) return false; break;
        case 2: if (!blob(input, out.data)) return false; break;
        case 1:
        case 5: {
            const size_t width = out.wire == 1 ? 8 : 4;
            if (width > input.size()) return false;
            input.remove_prefix(width);
            break;
        }
        default: return false;
    }
    out.encoded = before.substr(0, before.size() - input.size());
    return true;
}

inline bool find(View input, uint32_t tag, Field& result) {
    bool found = false;
    while (!input.empty()) {
        Field entry;
        if (!field(input, entry)) return false;
        if (entry.number != tag) continue;
        if (found) return false;
        found = true;
        result = entry;
    }
    return found;
}

inline bool text_is(View view, std::string_view text) {
    return view.size() == text.size() && !memcmp(view.data(), text.data(), text.size());
}

inline void put(Bytes& output, View bytes) {
    if (!bytes.empty()) output.insert(output.end(), bytes.begin(), bytes.end());
}

inline void put_varint(Bytes& output, uint64_t value) {
    while (value >= 128) {
        output.push_back(uint8_t(value) | 128);
        value >>= 7;
    }
    output.push_back(uint8_t(value));
}

inline void put_blob(Bytes& output, View bytes) {
    put_varint(output, bytes.size());
    put(output, bytes);
}

inline Bytes replace(View body, View old, View replacement) {
    const size_t offset = size_t(old.data() - body.data());
    Bytes result;
    result.reserve(body.size() - old.size() + replacement.size());
    put(result, body.substr(0, offset));
    put(result, replacement);
    put(result, body.substr(offset + old.size()));
    return result;
}

inline Bytes replace_field(View body, const Field& old, View replacement) {
    Bytes field;
    put_varint(field, (old.number << 3) | 2);
    put_blob(field, replacement);
    return replace(body, old.encoded, view(field));
}

struct Components {
    View root, tree;
    Field map_entry, envelope, tree_field;
    View inventory, player;
    View inventory_record, player_record;
};

// Native scene-objects.bin v1 contains a named "Player" scene object. Only its
// root Inventory and selected fields of Player are personal to the guest.
inline bool locate(View file, Components& out) {
    if (file.size() > kMaxScene || !blob(file, out.root) || !file.empty()) return false;
    Field version;
    if (!find(out.root, 1, version) || version.wire || version.scalar != 1) return false;
    bool player_found = false;
    View records = out.root;
    while (!records.empty()) {
        Field entry, key;
        if (!field(records, entry)) return false;
        if (entry.number != 2 || entry.wire != 2) continue;
        if (!find(entry.data, 1, key) || !text_is(key.data, "Player")) continue;
        if (player_found || !find(entry.data, 2, out.envelope) || out.envelope.wire != 2) return false;
        player_found = true;
        out.map_entry = entry;
    }
    if (!player_found || !find(out.envelope.data, 3, out.tree_field) || out.tree_field.wire != 2) {
        return false;
    }
    out.tree = out.tree_field.data;
    View remaining = out.tree;
    View object, loop;
    Field tag, count;
    if (!blob(remaining, object) || !find(object, 4, tag) || !text_is(tag.data, "Player") ||
        !blob(remaining, loop) || !find(loop, 1, count) || count.wire || count.scalar > 256) {
        return false;
    }
    for (uint64_t i = 0; i < count.scalar; i++) {
        View header, component;
        Field name;
        if (!blob(remaining, header) || !find(header, 1, name)) return false;
        const View record = remaining;
        if (!blob(remaining, component)) return false;
        if (text_is(name.data, "Inventory")) {
            if (out.inventory_record.data()) return false;
            out.inventory_record = record.substr(0, record.size() - remaining.size());
            out.inventory = component;
        }
        if (text_is(name.data, "Player")) {
            if (out.player_record.data()) return false;
            out.player_record = record.substr(0, record.size() - remaining.size());
            out.player = component;
        }
    }
    return out.inventory_record.data() && out.player_record.data();
}

inline bool personal_field(uint32_t number) {
    switch (number) {
        case 5:  // Known blueprints
        case 7:  // PDA journal
        case 8:  // Encyclopedia
        case 9:  // Scanner data
        case 11: // Tools used
        case 13: // Analyzed tech
        case 26: // Recipe pins
            return true;
        default:
            return false;
    }
}

inline bool filter_player(View input, bool personal, Bytes& output) {
    while (!input.empty()) {
        Field entry;
        if (!field(input, entry)) return false;
        if (personal_field(entry.number) == personal) put(output, entry.encoded);
    }
    return true;
}

inline bool valid_inventory(View inventory) {
    Field version;
    return find(inventory, 1, version) && version.wire == 0 && version.scalar == 6;
}

inline bool extract(View scene, Bytes& profile) {
    Components source;
    Bytes personal;
    if (!locate(scene, source) || !valid_inventory(source.inventory) ||
        !filter_player(source.player, true, personal)) return false;
    profile.clear();
    put_blob(profile, source.inventory);
    put_blob(profile, view(personal));
    return profile.size() <= kMaxScene;
}

inline bool overlay(View scene, View profile, Bytes& output) {
    Components target;
    View inventory, personal;
    if (!locate(scene, target) || !valid_inventory(target.inventory) ||
        !blob(profile, inventory) || !valid_inventory(inventory) ||
        !blob(profile, personal) || !profile.empty()) return false;
    Bytes player, validated_personal;
    if (!filter_player(target.player, false, player) ||
        !filter_player(personal, true, validated_personal) ||
        personal.size() != validated_personal.size()) return false;
    put(player, personal);
    Bytes player_record, inventory_record;
    put_blob(player_record, view(player));
    put_blob(inventory_record, inventory);

    // Rebuild in byte order so replacing one length cannot shift the other.
    View first = target.player_record, second = target.inventory_record;
    const Bytes* first_data = &player_record;
    const Bytes* second_data = &inventory_record;
    if (second.data() < first.data()) {
        std::swap(first, second);
        std::swap(first_data, second_data);
    }
    const size_t begin = size_t(first.data() - target.tree.data());
    const size_t next = size_t(second.data() - target.tree.data());
    Bytes tree;
    put(tree, target.tree.substr(0, begin));
    put(tree, view(*first_data));
    put(tree, target.tree.substr(begin + first.size(), next - begin - first.size()));
    put(tree, view(*second_data));
    put(tree, target.tree.substr(next + second.size()));
    const Bytes envelope = replace_field(target.envelope.data, target.tree_field, view(tree));
    const Bytes entry = replace_field(target.map_entry.data, target.envelope, view(envelope));
    const Bytes root = replace_field(target.root, target.map_entry, view(entry));
    Bytes file;
    put_blob(file, view(root));
    if (file.size() > kMaxScene) return false;
    output = std::move(file);
    return true;
}

} // namespace guest_progress
