#pragma once
#include <cmath>
struct HeldRotation { float x,y,z,w; };
inline bool held_rotation_normalize(HeldRotation q,HeldRotation* out) {
    float n=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    if (!out || !std::isfinite(n) || n<0.0001f || n>100.0f) return false;
    float s=1.0f/std::sqrt(n); *out={q.x*s,q.y*s,q.z*s,q.w*s}; return true;
}
inline HeldRotation held_rotation_mul(HeldRotation a,HeldRotation b) {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
            a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
            a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
            a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
inline bool held_rotation_relative(HeldRotation hand,HeldRotation tool,HeldRotation* out) {
    HeldRotation a{},b{};
    if (!held_rotation_normalize(hand,&a) || !held_rotation_normalize(tool,&b)) return false;
    return held_rotation_normalize(held_rotation_mul({-a.x,-a.y,-a.z,a.w},b),out);
}
