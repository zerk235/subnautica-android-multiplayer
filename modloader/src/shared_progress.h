#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include "net.h"

namespace shared_progress {
enum Kind : uint8_t { PodRepair = 1, Door = 2, Blueprint = 3, RadioRepair = 4 };
enum DoorState : uint16_t { Open = 1, Unlocked = 2, Unsealed = 4 };

inline bool valid(const WorldEvent& event) {
    switch (event.build_flags) {
    case PodRepair:
    case RadioRepair:
        return std::isfinite(event.constructed_amount) &&
               event.constructed_amount > 0 && event.constructed_amount <= 1;
    case Door:
        return event.object_id[0] && std::memchr(event.object_id, '\0', sizeof(event.object_id)) &&
               !(event.reserved16 & ~(Open | Unlocked | Unsealed));
    case Blueprint:
        return event.tech_type > 0;
    default:
        return false;
    }
}
}
