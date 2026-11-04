#pragma once
#include "base_geometry_codec.h"

namespace base_geometry {
// Android Base.CellSize[Room] = (3,1,3), Base.SetCell and PackOffset.
// Root is the minimum corner; eight aliases link back to that same root.
inline bool room_topology_valid(const Snapshot& s) {
    if (!valid(s) || s.shape!=std::array<int32_t,3>{3,1,3}) return false;
    for (int z=0;z<3;++z) for (int x=0;x<3;++x) {
        const size_t i=size_t(x+3*z);
        if (s.cells[i]!=(i==0 ? 1 : 3) || s.links[i]!=uint8_t((x<<5)|z) || s.glass[i])
            return false;
    }
    return true;
}
}
