#pragma once

#include "net.h"

#include <cmath>
#include <cstring>

namespace item_drop_sync {

constexpr int32_t kGravTrap = 758;
constexpr int32_t kWaterproofLocker = 759;
constexpr int32_t kVehicleBay = 752;
// For a Vehicle Bay drop, build_flags carries its native deployed state.
constexpr uint8_t kBayDeployed = 1;
// A locker snapshot may create the missing replica. It does not move or reactivate a known object.
constexpr uint8_t kLockerEnsurePresent = 1;

inline bool locker_presence(const WorldEvent& event) {
    return event.kind == kWorldEventDrop && event.tech_type == kWaterproofLocker &&
           event.build_flags == kLockerEnsurePresent;
}

// Materials whose prefab can recreate the dropped item in Android 1.22.83315.
// Charged tools and other items with instance state require native serialization.
inline bool plain_resource(int32_t tech) {
    switch (tech) {
        case 1:   // Quartz
        case 2:   // ScrapMetal
        case 3:   // FiberMesh
        case 7:   // Copper
        case 8:   // Lead
        case 9:   // Salt
        case 15:  // Glass
        case 16:  // Titanium
        case 17:  // Silicone
        case 21:  // Gold
        case 28:  // Silver
        case 30:  // TitaniumIngot
        case 32:  // CopperWire
        case 33:  // WiringKit
        case 34:  // AdvancedWiringKit
        case 35:  // Cave sulfur (CrashPowder)
        case 36:  // Diamond
        case 40:  // Lithium
        case 41:  // PlasteelIngot
        case 42:  // EnameledGlass
        case 44:  // ComputerChip
        case 52:  // Ruby (AluminumOxide)
        case 54:  // Magnetite
        case 62:  // Lubricant
        case 63:  // UraniniteCrystal
        case 68:  // Kyanite
        case 69:  // Nickel
        case 2503: // CreepvineSeedCluster
        case 3501: // Table coral sample (JeweledDiskPiece)
        case 3502: // Coral tube sample (CoralChunk)
        case 3504: // StalkerTooth
            return true;
        default:
            return false;
    }
}

inline bool deployable_item(int32_t tech) {
    return tech == kGravTrap || tech == kWaterproofLocker || tech == kVehicleBay;
}

inline bool vehicle_bay_deployed(const WorldEvent& event) {
    return event.kind == kWorldEventDrop && event.tech_type == kVehicleBay &&
           (event.build_flags & kBayDeployed) != 0;
}

inline bool replicable_item(int32_t tech) {
    return plain_resource(tech) || deployable_item(tech);
}

inline bool has_object_id(const WorldEvent& event) {
    return event.object_id[0] &&
           std::memchr(event.object_id, '\0', sizeof(event.object_id));
}

inline bool has_item_identity(const WorldEvent& event) {
    return (event.kind == kWorldEventPickup || event.kind == kWorldEventDrop) &&
           replicable_item(event.tech_type) && has_object_id(event);
}

inline bool valid_pose(const WorldEvent& event) {
    const float rotation_length = event.qx * event.qx + event.qy * event.qy +
                                  event.qz * event.qz + event.qw * event.qw;
    return rotation_length > 0.95f && rotation_length < 1.05f &&
           std::isfinite(event.x) && std::isfinite(event.y) && std::isfinite(event.z) &&
           std::isfinite(event.vx) && std::isfinite(event.vy) && std::isfinite(event.vz);
}

inline bool valid_spawn(const WorldEvent& event) {
    return event.kind == kWorldEventDrop && has_item_identity(event) && valid_pose(event);
}

constexpr uint8_t kLockerMoving = 1;

inline bool valid_locker_motion(const WorldEvent& event) {
    return event.kind == kWorldEventLockerMotion && event.tech_type == kWaterproofLocker &&
           (event.build_flags == 0 || event.build_flags == kLockerMoving) &&
           has_object_id(event) && valid_pose(event);
}

inline bool near_drop(const WorldEvent& event, float x, float y, float z) {
    constexpr float kMaxSpawnDistance = 24.0f;
    const float dx = event.x - x;
    const float dy = event.y - y;
    const float dz = event.z - z;
    return dx * dx + dy * dy + dz * dz <= kMaxSpawnDistance * kMaxSpawnDistance;
}

inline bool supersedes(const WorldEvent& incoming, const WorldEvent& pending) {
    // The game receives events in order per origin. A retry must not replay an
    // older pickup or drop once a newer action for the same object has landed.
    return !locker_presence(incoming) &&
           std::memcmp(incoming.guid, pending.guid, sizeof(incoming.guid)) == 0;
}

} // namespace item_drop_sync
