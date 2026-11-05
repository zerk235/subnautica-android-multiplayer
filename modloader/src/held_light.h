#pragma once
#include <cstdint>
inline bool held_light_supported(uint32_t tech) { return tech==507 || tech==751; }
inline bool held_light_visible(uint32_t prop,uint32_t peer,bool powered) {
    return powered && held_light_supported(prop) && prop==peer;
}
