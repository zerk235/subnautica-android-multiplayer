#include "modloader/src/base_build_completion.h"
#include "modloader/src/base_geometry_corridor.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <vector>
#define SNMOD_BASE_GEOMETRY_CAPTURE 1
#include "modloader/src/net.h"
#include "modloader/src/base_corridor_registry.inc"

struct Node { Node* parent = nullptr; };
int main() {
    using namespace base_geometry;
    alignas(void*) uint8_t cls[0x300]{}, method[0x60]{};
    uint16_t slot=13, count=22, event_count=1, actual_slot=0, actual_count=0;
    void* method_pointer=method;
    void* fake_function=&cls[0];
    std::memcpy(method+0x50,&slot,2);
    std::memcpy(cls+0x126,&event_count,2); // former bug rejected this valid table
    std::memcpy(cls+0x12A,&count,2);
    std::memcpy(method,&fake_function,sizeof(void*));
    std::memcpy(cls+0x208,&fake_function,sizeof(void*));
    std::memcpy(cls+0x210,&method_pointer,sizeof(void*));
    assert(completion_vtable_entry(cls,method,&actual_slot,&actual_count)==cls+0x208);
    assert(actual_slot==13 && actual_count==22);
    cls[0x210]=0; cls[0x211]=0; cls[0x212]=0; cls[0x213]=0;
    assert(!completion_vtable_entry(cls,method,&actual_slot,&actual_count));
    // Deconstruction uses the separately validated ProgressDeconstruction slot.
    count=22; slot=16;
    std::memcpy(cls+0x12A,&count,2); std::memcpy(method+0x50,&slot,2);
    std::memcpy(cls+0x238,&fake_function,sizeof(void*));
    std::memcpy(cls+0x240,&method_pointer,sizeof(void*));
    assert(completion_vtable_entry(cls,method,&actual_slot,&actual_count,16)==cls+0x238);
    assert(!completion_vtable_entry(cls,method,&actual_slot,&actual_count,13));
    std::memcpy(cls+0x210,&method_pointer,sizeof(void*));
    count=13; std::memcpy(cls+0x12A,&count,2);
    assert(!completion_vtable_entry(cls,method,&actual_slot,&actual_count));
    Node world, root{&world}, model{&root}, child{&model}, unrelated{&world};
    auto parent = [](void* p) -> void* { return static_cast<Node*>(p)->parent; };
    assert(preview_related(&root, &model, parent)); // bug: preview is a child
    assert(preview_related(&child, &model, parent));
    assert(preview_related(&model, &model, parent));
    assert(!preview_related(&unrelated, &model, parent));
    assert(!preview_related(&root, nullptr, parent));
    // Every preview, already-completed prefab, remote application, load, or
    // unfinished/cancelled transition must still run the game once but never send.
    for (unsigned mask = 0; mask < 64; ++mask) {
        const bool requested = mask & 1, was_complete = mask & 2, preview = mask & 4;
        const bool remote = mask & 8, ready = mask & 16, success = mask & 32;
        std::vector<int> sequence;
        const bool eligible = requested && !was_complete && !preview && !remote && ready;
        auto result = observe_completion(requested, was_complete, preview, remote, ready,
            [&] { sequence.push_back(1); return true; },
            [&] { sequence.push_back(2); return uint8_t(success); },
            [&] { sequence.push_back(3); }, [](const char*) { assert(false); });
        assert(result == uint8_t(success));
        std::vector<int> expected;
        if (eligible) expected.push_back(1);
        expected.push_back(2);
        if (eligible && success) expected.push_back(3);
        assert(sequence == expected);
    }
    int calls=0, sends=0, errors=0;
    observe_completion(true,false,false,false,true,
        []() -> bool { throw std::runtime_error("capture"); },
        [&] { ++calls; return uint8_t(1); }, [&] { ++sends; }, [&](const char*) { ++errors; });
    assert(calls==1 && sends==0 && errors==1);
    observe_completion(true,false,false,false,true, [] { return false; },
        [&] { ++calls; return uint8_t(1); }, [&] { ++sends; }, [&](const char*) { ++errors; });
    assert(calls==2 && sends==0 && errors==1);
    bool threw=false;
    try {
        observe_completion(true,false,false,false,true, [] { return true; },
            []() -> uint8_t { throw std::runtime_error("game"); },
            [&] { ++sends; }, [&](const char*) { ++errors; });
    } catch (...) { threw=true; }
    assert(threw && sends==0); // Failed game transition must not publish.
    for (float amount : {0.0f, 0.5f, 1.0f, -1.0f, NAN}) {
        for (unsigned mask=0; mask<32; ++mask) {
            bool complete=mask&1, remote=mask&2, ready=mask&4, preview=mask&8, success=mask&16;
            std::vector<int> sequence;
            void* expected = success ? &world : nullptr;
            auto result = observe_deconstruction(amount, complete, remote, ready, preview,
                [&] { sequence.push_back(1); return true; },
                [&]() -> void* { sequence.push_back(2); return expected; },
                [&] { sequence.push_back(3); }, [](const char*) { assert(false); });
            assert(result==expected);
            bool eligible = amount==0 && !complete && !remote && ready && !preview;
            std::vector<int> wanted;
            if (eligible) wanted.push_back(1);
            wanted.push_back(2);
            if (eligible && success) wanted.push_back(3);
            assert(sequence==wanted);
        }
    }
    int removal_original_calls=0, removal_errors=0;
    auto removal_result = observe_deconstruction(0,false,false,true,false,
        []() -> bool { throw std::runtime_error("capture"); },
        [&]() -> void* { ++removal_original_calls; return &world; },
        [] { assert(false); }, [&](const char*) { ++removal_errors; });
    assert(removal_result==&world && removal_original_calls==1 && removal_errors==1);
    removal_result = observe_deconstruction(0,false,false,true,false,
        [] { return true; }, [&]() -> void* { ++removal_original_calls; return &world; },
        [] { throw std::runtime_error("publish"); }, [&](const char*) { ++removal_errors; });
    assert(removal_result==&world && removal_original_calls==2 && removal_errors==2);
    GhostEnvelope envelope;
    envelope.ghost_class="BaseGhost"; envelope.target_base_id="existing";
    envelope.geometry.shape={1,1,1}; envelope.geometry.faces.resize(6);
    envelope.geometry.cells={4}; envelope.geometry.links={0}; envelope.geometry.glass={0};
    assert(supported_existing_corridor(envelope,5510));
    // Each corridor family must agree with both the prefab's shape/glass fields
    // and the immutable geometry. Other base cells must never enter this path.
    for (int tech : {5510,5511,5512,5513,5516,5517}) {
        auto variant = envelope;
        variant.geometry.glass[0] = uint8_t(corridor_glass(tech));
        variant.object_id="variant";
        assert(supported_existing_corridor(variant,tech));
        for (int shape=-1;shape<=4;++shape) for (bool glass : {false,true})
            assert(corridor_prefab_matches(tech,shape,glass) ==
                   (shape==corridor_shape(tech) && glass==corridor_glass(tech)));
        // Transport retains orientation topology, glass and nullable masks.
        for (uint8_t links : {uint8_t(3),uint8_t(6),uint8_t(12),uint8_t(15)}) {
            variant.geometry.links[0]=links;
            std::vector<uint8_t> bytes;
            GhostEnvelope decoded;
            assert(encode_envelope(variant,&bytes));
            assert(decode_envelope(bytes.data(),bytes.size(),&decoded));
            assert(decoded.geometry.links==variant.geometry.links &&
                   decoded.geometry.glass==variant.geometry.glass && decoded.geometry.masks.empty());
            assert(supported_existing_corridor(decoded,tech));
        }
        variant.geometry.glass[0]^=1;
        assert(!supported_existing_corridor(variant,tech));
        variant=envelope; variant.geometry.glass[0]=uint8_t(corridor_glass(tech));
        variant.ghost_class="BaseAddCellGhost";
        assert(!supported_existing_corridor(variant,tech));
        variant.ghost_class="BaseAddCorridorGhost";
        assert(supported_existing_corridor(variant,tech));
        variant.target_base_id.clear();
        assert(!supported_existing_corridor(variant,tech));
    }
    for (int tech : {0,5508,5509,5514,5515,5518,5521,5522}) {
        assert(corridor_shape(tech)<0);
        assert(!supported_existing_corridor(envelope,tech));
        assert(!corridor_prefab_matches(tech,0,false));
    }
    auto other = envelope;
    assert(same_corridor_cell(envelope, other));
    other.target_base_id="different";
    assert(!same_corridor_cell(envelope, other));
    other=envelope; other.target_offset[0]++;
    assert(!same_corridor_cell(envelope, other));
    other=envelope; other.target_base_anchor[0]++;
    assert(!same_corridor_cell(envelope, other));
    WorldEvent original{};
    std::snprintf(original.object_id, sizeof(original.object_id), "old-build");
    envelope.object_id=original.object_id;
    build_remember_corridor(original,envelope);
    assert(build_synced_cell(envelope)==build_synced_corridor("old-build"));
    build_synced_corridor("old-build")->removed=true;
    assert(build_corridor_removed("old-build") && !build_synced_cell(envelope));
    build_remember_corridor(original,envelope); // a late repair cannot revive it
    assert(build_corridor_removed("old-build"));
    WorldEvent replacement{};
    std::snprintf(replacement.object_id, sizeof(replacement.object_id), "new-build");
    other=envelope; other.object_id=replacement.object_id;
    build_remember_corridor(replacement,other);
    assert(build_synced_cell(other)==build_synced_corridor("new-build"));
    assert(build_corridor_removed("old-build") && !build_corridor_removed("new-build"));
    // Hatches on separate sides of the same cell never alias one another or
    // the enclosing corridor. Canonical descriptors cannot carry cell changes.
    // Room backing arrays can have capacity beyond the active shape. A hatch
    // descriptor must use face/target metadata without exporting those arrays.
    auto room=envelope;
    room.geometry.shape={3,1,3};
    room.geometry.faces.assign(108,1); // observed: actual=108 vs expected=54
    room.geometry.cells.assign(18,1);
    assert(!valid(room.geometry));
    auto room_hatch=hatch_descriptor(room,{-3,0,7},2);
    assert(hatch_direction(room_hatch,5501)==2);
    assert(room_hatch.target_base_id==room.target_base_id &&
           room_hatch.target_base_anchor==room.target_base_anchor);
    for (int direction=0;direction<6;++direction) {
        auto hatch=hatch_descriptor(envelope,{5,0,2},direction);
        assert(hatch_direction(hatch,5501)==direction);
        assert(hatch_direction(hatch,5510)<0);
        assert(!supported_existing_corridor(hatch,5510));
        auto other_face=hatch_descriptor(envelope,{5,0,2},(direction+1)%6);
        assert(!same_hatch_face(hatch,other_face));
        auto bad=hatch; bad.geometry.cells[0]=4;
        assert(hatch_direction(bad,5501)<0);
        bad=hatch; bad.geometry.masks[0]^=uint8_t(1u<<((direction+1)%6));
        assert(hatch_direction(bad,5501)<0);
        bad=hatch; bad.geometry.faces[(direction+1)%6]=3;
        assert(hatch_direction(bad,5501)<0);
        bad=hatch; bad.target_offset[0]=8193;
        assert(hatch_direction(bad,5501)<0);
        std::vector<uint8_t> encoded;
        GhostEnvelope decoded;
        hatch.object_id="hatch-wire";
        assert(encode_envelope(hatch,&encoded) && decode_envelope(encoded.data(),encoded.size(),&decoded));
        assert(same_hatch_face(hatch,decoded));
    }
    WorldEvent hatch_state{};
    hatch_state.tech_type=5501;
    std::snprintf(hatch_state.object_id,sizeof(hatch_state.object_id),"hatch-a");
    auto hatch=hatch_descriptor(other,other.target_offset,0);
    hatch.object_id=hatch_state.object_id;
    build_remember_corridor(hatch_state,hatch);
    assert(build_synced_face(hatch)==build_synced_corridor("hatch-a"));
    assert(build_synced_cell(other)==build_synced_corridor("new-build"));
    build_synced_corridor("hatch-a")->removed=true;
    build_remember_corridor(hatch_state,hatch);
    assert(!build_synced_face(hatch) && build_corridor_removed("hatch-a"));
    hatch_state.object_id[6]='b'; hatch.object_id=hatch_state.object_id;
    build_remember_corridor(hatch_state,hatch);
    assert(build_synced_face(hatch)==build_synced_corridor("hatch-b"));
    for (int tech:{5501,5505,5514}) for (int d=0;d<6;++d) {
        auto face=face_descriptor(envelope,{5,0,2},d,tech);
        assert(face_direction(face,tech)==d && face_descriptor_tech(face)==tech);
        assert(!supported_existing_corridor(face,5510));
        for (int other_tech:{5501,5505,5514}) {
            auto other_face=face_descriptor(envelope,{5,0,2},d,other_tech);
            assert(same_structural_face(face,other_face));
            if (other_tech!=tech) assert(face_direction(face,other_tech)<0);
        }
        std::vector<uint8_t> bytes; GhostEnvelope decoded;
        assert(encode_envelope(face,&bytes) && decode_envelope(bytes.data(),bytes.size(),&decoded));
        assert(face_direction(decoded,tech)==d && same_structural_face(face,decoded));
        auto bad=face; bad.geometry.faces[(d+1)%6]=uint8_t(face_type_for_tech(tech));
        assert(face_direction(bad,tech)<0);
        bad=face; bad.geometry.cells[0]=4; assert(face_direction(bad,tech)<0);
        bad=face; bad.geometry.glass[0]=1; assert(face_direction(bad,tech)<0);
        bad=face; bad.geometry.masks[0]=0; assert(face_direction(bad,tech)<0);
    }
    for (int unsupported:{5500,5504,5515,5522,5528,5529,5542}) {
        auto face=face_descriptor(envelope,{5,0,2},0,unsupported);
        assert(face_type_for_tech(unsupported)<0 && face_direction(face,unsupported)<0);
    }
    WorldEvent win{}; win.tech_type=5505;
    snprintf(win.object_id,sizeof(win.object_id),"window-replacement");
    auto win_face=face_descriptor(other,other.target_offset,0,win.tech_type);
    win_face.object_id=win.object_id; build_remember_corridor(win,win_face);
    assert(build_face_has_replacement(hatch,"hatch-b"));
    assert(!build_synced_face(hatch)); // cross-recipe same physical wall ambiguous
    assert(build_synced_cell(other)==build_synced_corridor("new-build"));
    build_synced_corridor(win.object_id)->removed=true;
    assert(!build_face_has_replacement(hatch,"hatch-b"));
        auto connector=envelope;
    connector.object_id="connector";connector.ghost_class="BaseAddConnectorGhost";
    connector.geometry.shape={1,1,1};connector.geometry.cells={6};connector.geometry.glass={0};
    assert(single_cell_type(5519)==6 && supported_existing_cell(connector,5519));
    assert(!supported_existing_corridor(connector,5519));
    assert(std::string(single_cell_ghost(5519))=="BaseAddConnectorGhost");
    assert(std::string(single_cell_ghost(5510))=="BaseAddCorridorGhost");
    assert(!supported_existing_cell(connector,5510));
    auto bad_connector=connector;bad_connector.ghost_class="BaseAddCorridorGhost";
    assert(!supported_existing_cell(bad_connector,5519));
    bad_connector=connector;bad_connector.geometry.cells[0]=4;assert(!supported_existing_cell(bad_connector,5519));
    bad_connector=connector;bad_connector.geometry.glass[0]=1;assert(!supported_existing_cell(bad_connector,5519));
    bad_connector=connector;bad_connector.target_base_id.clear();assert(!supported_existing_cell(bad_connector,5519));
    bad_connector=connector;bad_connector.target_offset[1]=8193;assert(!supported_existing_cell(bad_connector,5519));
    bad_connector=connector;bad_connector.geometry.shape={1,2,1};assert(!supported_existing_cell(bad_connector,5519));
    std::vector<uint8_t> connector_bytes;GhostEnvelope connector_decoded;
    assert(encode_envelope(connector,&connector_bytes) && decode_envelope(connector_bytes.data(),connector_bytes.size(),&connector_decoded));
    assert(supported_existing_cell(connector_decoded,5519));
    assert(supported_existing_cell(envelope,5510));
    for (int unsupported:{5500,5509,5518,5520,5526,5534}) assert(single_cell_type(unsupported)<0);
    // Capacity remains bounded; an old removal is never evicted to admit a repair.
    for (unsigned i=0;i<128;++i) {
        char id[32]; std::snprintf(id,sizeof(id),"capacity-%u",i);
        build_synced_corridor(id,true);
    }
    assert(!build_synced_corridor("overflow",true));
    assert(build_corridor_removed("old-build"));
    envelope.geometry.cells[0]=5;
    assert(!supported_existing_corridor(envelope,5510));
    puts("completion trigger / preview hierarchy / completed corridor policy tests passed");
}
