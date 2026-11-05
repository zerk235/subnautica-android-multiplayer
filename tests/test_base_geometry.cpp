#include "modloader/src/base_geometry_reliable.h"
#include "modloader/src/base_geometry_cache.h"
#include "modloader/src/base_geometry_corridor.h"
#include <cassert>
#include <cstdio>
#include <limits>

using namespace base_geometry;

int main() {
    Snapshot s;
    s.shape = {2, 1, 2};
    s.cell_offset = {-42, 0, 7};
    s.anchor = {1, -2, std::numeric_limits<int32_t>::min()};
    s.faces.resize(24);
    for (size_t i = 0; i < s.faces.size(); ++i) s.faces[i] = static_cast<uint8_t>(i % 21);
    s.faces.back() = 133;
    s.cells = {1, 2, 3, 4}; s.links = {5, 6, 7, 8};
    s.masks = {9, 10, 11, 12}; s.glass = {0, 1, 1, 0};
    std::vector<uint8_t> bytes;
    assert(encode(s, &bytes));
    assert(bytes.size() == kHeaderSize + 40);
    Snapshot out;
    assert(decode(bytes.data(), bytes.size(), &out));
    assert(out.shape == s.shape && out.cell_offset == s.cell_offset && out.anchor == s.anchor);
    assert(out.faces == s.faces && out.cells == s.cells && out.links == s.links &&
           out.masks == s.masks && out.glass == s.glass);
    for (size_t n = 0; n < bytes.size(); ++n) assert(!decode(bytes.data(), n, &out));
    auto bad = bytes; bad[0] ^= 1; assert(!decode(bad.data(), bad.size(), &out));
    bad = bytes; bad[4] = 3; assert(!decode(bad.data(), bad.size(), &out));
    bad = bytes; bad[8] = 5; assert(!decode(bad.data(), bad.size(), &out));
    bad = bytes; bad[12] = 0; assert(!decode(bad.data(), bad.size(), &out));
    bad = bytes; bad.back() = 2; assert(!decode(bad.data(), bad.size(), &out));
    bad = bytes; bad[kHeaderSize] = 127; assert(!decode(bad.data(), bad.size(), &out));
    bad = bytes; bad[kHeaderSize + 24] = 20; assert(!decode(bad.data(), bad.size(), &out));
    bad = bytes; bad.push_back(0); assert(!decode(bad.data(), bad.size(), &out));
    assert(!decode(nullptr, 0, &out));
    const Snapshot before = out;
    bad = bytes; bad[4] = 3;
    assert(!decode(bad.data(), bad.size(), &out));
    assert(out.shape == before.shape && out.faces == before.faces);
    auto missing_masks = s;
    missing_masks.masks.clear();
    std::vector<uint8_t> missing_encoded;
    assert(encode(missing_masks, &missing_encoded));
    assert(decode(missing_encoded.data(), missing_encoded.size(), &out));
    assert(out.masks.empty() && out.faces == s.faces && out.glass == s.glass);
    missing_encoded[kHeaderSize + 24 + 4 + 4] = 1;
    assert(!decode(missing_encoded.data(), missing_encoded.size(), &out));
    s.shape = {4097, 1, 1}; assert(!encode(s, &bytes));
    s.shape = {2, 1, 2}; s.glass[0] = 3; assert(!encode(s, &bytes));
    Snapshot max;
    max.shape = {16, 16, 16};
    max.faces.resize(kMaxCells * 6); max.cells.resize(kMaxCells);
    max.links.resize(kMaxCells); max.masks.resize(kMaxCells); max.glass.resize(kMaxCells);
    assert(encode(max, &bytes));
    assert(bytes.size() == kHeaderSize + 10 * kMaxCells);
    assert(decode(bytes.data(), bytes.size(), &out));
    assert(out.faces == max.faces && out.cells == max.cells);
    TransferKey key; key.origin = 9; key.generation = 42; key.object[3] = 17;
    const size_t fragments = (bytes.size() + kFragmentPayload - 1) / kFragmentPayload;
    assert(fragments > 1 && fragments <= kMaxFragments);
    std::vector<std::vector<uint8_t>> frames(fragments);
    for (size_t i = 0; i < fragments; ++i) assert(make_fragment(key, bytes, i, &frames[i]));
    std::vector<uint8_t> invalid_frame;
    assert(!make_fragment(key, bytes, fragments, &invalid_frame));
    Reassembler receive;
    assert(!receive.finish(&out));
    for (size_t i = fragments; i-- > 0;) {
        assert(receive.accept(frames[i].data(), frames[i].size()));
        assert(receive.accept(frames[i].data(), frames[i].size())); // retry is idempotent
        if (i) assert(!receive.finish(&out));
    }
    assert(receive.finish(&out) && out.cells == max.cells && out.faces == max.faces);
    auto changed = frames[0]; changed.back() ^= 1;
    assert(!receive.accept(changed.data(), changed.size())); // conflicting retry
    changed = frames[0]; changed[8] ^= 1;
    assert(!receive.accept(changed.data(), changed.size())); // different transfer
    changed = frames[0]; changed.pop_back();
    assert(!receive.accept(changed.data(), changed.size())); // truncated frame
    changed = frames[0]; changed[36] = 1;
    assert(!receive.accept(changed.data(), changed.size())); // unaligned offset
    Reassembler corrupted;
    changed = frames[0]; changed.back() ^= 1;
    assert(corrupted.accept(changed.data(), changed.size()));
    for (size_t i = 1; i < fragments; ++i) assert(corrupted.accept(frames[i].data(), frames[i].size()));
    assert(!corrupted.finish(&out)); // whole-snapshot checksum catches corruption
    GhostEnvelope e;
    e.object_id = "1f2c65c5-bd82-49a6-a27a-5847b4182524";
    e.target_base_id = "base-01";
    e.ghost_class = "BaseAddCorridorGhost";
    e.target_offset = {-3, 7, std::numeric_limits<int32_t>::min()};
    e.target_base_anchor = {9, -8, 7};
    e.geometry = max;
    std::vector<uint8_t> envelope;
    assert(encode_envelope(e, &envelope));
    assert(envelope.size() <= kMaxEnvelope);
    GhostEnvelope decoded;
    assert(decode_envelope(envelope.data(), envelope.size(), &decoded));
    assert(decoded.object_id == e.object_id && decoded.target_base_id == e.target_base_id &&
           decoded.ghost_class == e.ghost_class && decoded.target_offset == e.target_offset &&
           decoded.target_base_anchor == e.target_base_anchor &&
           decoded.geometry.faces == e.geometry.faces);
    auto invalid_envelope = envelope; invalid_envelope[8] = 0xff;
    assert(!decode_envelope(invalid_envelope.data(), invalid_envelope.size(), &decoded));
    invalid_envelope = envelope; invalid_envelope[kEnvelopeHeader] = 0;
    assert(!decode_envelope(invalid_envelope.data(), invalid_envelope.size(), &decoded));
    const size_t envelope_fragments = (envelope.size() + kFragmentPayload - 1) / kFragmentPayload;
    Reassembler envelope_rx;
    for (size_t i = envelope_fragments; i-- > 0;) {
        std::vector<uint8_t> frame;
        assert(make_fragment(key, envelope, i, &frame));
        assert(envelope_rx.accept(frame.data(), frame.size()));
    }
    assert(envelope_rx.finish(&decoded));
    assert(decoded.object_id == e.object_id && decoded.target_base_id == e.target_base_id &&
           decoded.target_offset == e.target_offset && decoded.geometry.cells == max.cells);
    GeometryCache cache;
    uint64_t fingerprint = 0;
    assert(cache.put(1, e, 5.0, &fingerprint));
    assert(cache.find(1, e.object_id.c_str(), fingerprint, 5.0));
    assert(!cache.find(2, e.object_id.c_str(), fingerprint, 5.0));
    assert(!cache.find(1, "another", fingerprint, 5.0));
    assert(!cache.find(1, e.object_id.c_str(), fingerprint ^ 1, 5.0));
    assert(!cache.find(1, e.object_id.c_str(), fingerprint, 66.0));
    auto newer = e; newer.geometry.cells[0] = 4;
    uint64_t newer_fingerprint = 0;
    assert(cache.put(1, newer, 6.0, &newer_fingerprint));
    assert(newer_fingerprint != fingerprint);
    assert(cache.find(1, e.object_id.c_str(), fingerprint, 6.0));
    assert(cache.find(1, e.object_id.c_str(), newer_fingerprint, 6.0));
    cache.clear(); assert(!cache.find(1, e.object_id.c_str(), fingerprint, 6.0));
    e.target_base_id.clear(); // new base has no existing target
    assert(encode_envelope(e, &envelope));
    assert(decode_envelope(envelope.data(), envelope.size(), &decoded));
    assert(decoded.target_base_id.empty());
    e.creates_base=true;e.base_pose={1,2,3,0,0,0,1};
    assert(!encode_envelope(e,&envelope)); // a birth must include the actual new ID
    e.target_base_id="new-base";
    assert(encode_envelope(e,&envelope));
    assert(decode_envelope(envelope.data(),envelope.size(),&decoded));
    assert(decoded.creates_base && decoded.base_pose==e.base_pose);
    auto bad_birth=envelope;bad_birth[48]=2;
    assert(!decode_envelope(bad_birth.data(),bad_birth.size(),&decoded));
    e.base_pose[3]=NAN;assert(!encode_envelope(e,&envelope));e.base_pose[3]=0;
    e.base_pose[6]=0;assert(!encode_envelope(e,&envelope));e.base_pose[6]=1;
    e.creates_base=false;assert(!encode_envelope(e,&envelope)); // no hidden creation pose
    e.base_pose={};
    TransferKey reliable_key; reliable_key.origin = 17; reliable_key.generation = 77;
    reliable_key.object = object_key(e.object_id);
    ReliableSender sender; ReliableReceiver receiver;
    assert(sender.start(reliable_key, e));
    std::vector<uint8_t> frame, repeated;
    Ack ack;
    ack.key = reliable_key; ack.index = 0;
    assert(!sender.acknowledge(ack)); // no fragment has been sent yet
    assert(!sender.start(reliable_key, e)); // don't silently replace an in-flight transfer
    assert(sender.frame(0.0, &frame));
    assert(receiver.receive(frame.data(), frame.size(), &ack, &decoded));
    // Lose the first acknowledgement. Nothing goes out before the retry interval,
    // then the exact same frame is resent and acknowledged idempotently.
    assert(!sender.frame(0.1, &repeated));
    assert(sender.frame(0.36, &repeated) && repeated == frame);
    assert(receiver.receive(repeated.data(), repeated.size(), &ack, &decoded));
    assert(sender.acknowledge(ack));
    double now = 1.0;
    while (!sender.done()) {
        assert(sender.frame(now, &frame));
        assert(receiver.receive(frame.data(), frame.size(), &ack, &decoded));
        assert(sender.acknowledge(ack));
        now += 1.0;
    }
    assert(decoded.object_id == e.object_id && decoded.geometry.faces == e.geometry.faces);
    assert(receiver.receive(frame.data(), frame.size(), &ack, &decoded));
    assert(ack.status == AckStatus::complete); // lost final ACK can be recovered
    ReliableSender retry_sender; ReliableReceiver retry_receiver;
    assert(retry_sender.start(reliable_key, e));
    bool corrupted_once = false, restarted = false;
    now = 0;
    while (!retry_sender.done()) {
        assert(retry_sender.frame(now, &frame));
        if (!corrupted_once) { frame.back() ^= 1; corrupted_once = true; }
        assert(retry_receiver.receive(frame.data(), frame.size(), &ack, &decoded));
        if (ack.status == AckStatus::restart) {
            restarted = true;
            // Lose the negative ACK too: a retransmitted final fragment must
            // receive another restart request instead of deadlocking the sender.
            assert(retry_sender.frame(now + 0.36, &repeated));
            assert(retry_receiver.receive(repeated.data(), repeated.size(), &ack, &decoded));
            assert(ack.status == AckStatus::restart);
        }
        assert(retry_sender.acknowledge(ack));
        now += 1.0;
    }
    assert(restarted && decoded.geometry.faces == e.geometry.faces);
    TransferKey wrong_key = reliable_key; wrong_key.object[0] ^= 1;
    assert(!retry_sender.start(wrong_key, e));
    GhostEnvelope corridor;
    corridor.object_id = "corridor"; corridor.target_base_id = "base";
    corridor.ghost_class = "BaseAddCorridorGhost"; corridor.geometry.shape = {1, 1, 1};
    corridor.geometry.faces.assign(6, 0); corridor.geometry.cells = {4};
    corridor.geometry.links = {0}; corridor.geometry.masks = {0}; corridor.geometry.glass = {0};
    assert(supported_existing_corridor(corridor, 5510));
    assert(!supported_existing_corridor(corridor, 5500));
    corridor.target_base_id.clear(); assert(!supported_existing_corridor(corridor, 5510));
    corridor.target_base_id = "base"; corridor.geometry.cells[0] = 1;
    assert(!supported_existing_corridor(corridor, 5510));
    corridor.geometry.cells[0] = 4; corridor.geometry.glass[0] = 1;
    assert(!supported_existing_corridor(corridor, 5510));
    corridor.geometry.glass[0] = 0; corridor.target_offset[0] = 8193;
    assert(!supported_existing_corridor(corridor, 5510));
    e.ghost_class.clear(); assert(!encode_envelope(e, &envelope));
    std::puts("base geometry codec: all tests passed");
}
