#pragma once
#include <cstdint>

// Original head-and-shoulders glyph, rasterized from two simple vector shapes.
// White RGB lets the native ping material apply its normal player-marker tint.
// Unity raw texture rows start at the bottom. Four-by-four coverage keeps the
// edge smooth when the 64px source is displayed at the smaller native HUD size.
constexpr unsigned kPlayerMarkerIconSize = 64;
inline void render_player_marker_icon(std::uint8_t* rgba) {
    for (unsigned y=0;y<kPlayerMarkerIconSize;++y) {
        for (unsigned x=0;x<kPlayerMarkerIconSize;++x) {
            unsigned coverage=0;
            for (unsigned sy=0;sy<4;++sy) for (unsigned sx=0;sx<4;++sx) {
                const float px=x+(sx+0.5f)/4.0f,py=y+(sy+0.5f)/4.0f;
                const float dx=px-32.0f,head_y=py-44.0f,body_y=(py-13.0f)/18.0f;
                const bool head=dx*dx+head_y*head_y<=8.5f*8.5f;
                const bool body=py>=10.0f&&py<=31.0f&&
                    (dx*dx)/(19.0f*19.0f)+body_y*body_y<=1.0f;
                if(head||body)++coverage;
            }
            auto* pixel=rgba+4*(y*kPlayerMarkerIconSize+x);
            pixel[0]=pixel[1]=pixel[2]=255;
            pixel[3]=static_cast<std::uint8_t>((coverage*255+8)/16);
        }
    }
}
