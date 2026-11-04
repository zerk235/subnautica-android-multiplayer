#include "modloader/src/held_rotation.h"
#include <cassert>
#include <cstdio>
#include <limits>
bool same(HeldRotation a,HeldRotation b) {
    HeldRotation x{},y{}; assert(held_rotation_normalize(a,&x)); assert(held_rotation_normalize(b,&y));
    return std::fabs(std::fabs(x.x*y.x+x.y*y.y+x.z*y.z+x.w*y.w)-1)<0.00001f;
}
int main() {
    HeldRotation hand{0.70710678f,0,0,0.70710678f}, relative{0,0.70710678f,0,0.70710678f},actual{};
    auto tool=held_rotation_mul(hand,relative);
    assert(held_rotation_relative(hand,tool,&actual) && same(actual,relative));
    assert(!same(held_rotation_mul(relative,hand),tool));
    auto other_hand=HeldRotation{0,0,0.70710678f,0.70710678f};
    auto other_tool=held_rotation_mul(other_hand,actual);
    assert(held_rotation_relative(other_hand,other_tool,&actual) && same(actual,relative));
    assert(held_rotation_relative({-hand.x,-hand.y,-hand.z,-hand.w},tool,&actual) && same(actual,relative));
    assert(held_rotation_normalize({0,0,0,2},&actual) && same(actual,{0,0,0,1}));
    assert(!held_rotation_normalize({0,0,0,0},&actual));
    assert(!held_rotation_normalize({0,0,0,100},&actual));
    assert(!held_rotation_normalize({0,0,0,std::numeric_limits<float>::quiet_NaN()},&actual));
    assert(!held_rotation_normalize({0,0,0,std::numeric_limits<float>::infinity()},&actual));
    assert(!held_rotation_relative(hand,{0,0,0,0},&actual));
    puts("held rotation: noncommuting wrist/tool poses, new wrist, sign equivalence, normalization and invalid values passed");
}
