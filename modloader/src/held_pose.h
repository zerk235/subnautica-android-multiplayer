#pragma once
#include <cmath>
struct HeldOffset {float x,y,z;};
inline bool held_offset_valid(HeldOffset p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
        p.x*p.x+p.y*p.y+p.z*p.z<=9.0f;
}
