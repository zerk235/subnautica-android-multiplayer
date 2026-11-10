#include "modloader/src/player_marker_policy.h"
#include <cassert>
#include <cstring>
#include <initializer_list>
using namespace player_marker;
int main() {
    Control c;
    assert(c.active()&&c.hud_enabled&&!c.text_enabled);
    c.apply(Command::Off);
    assert(!c.active());
    auto apply=[&](const char* text){c.apply(parse(text,static_cast<unsigned>(strlen(text))));};
    apply("probe\n");assert(c.probe_pending&&!c.text_enabled);
    c.probe_pending=false;assert(!c.active()); // Consumed probe does not enable text.
    for(const auto* text:{"ping","nitrox","beacon","cleanup"}){
        assert(parse(text,static_cast<unsigned>(strlen(text)))==Command::Refused);
        apply(text);assert(!c.active());
    }
    apply(" on\r\n");assert(c.text_enabled);
    apply("probe");assert(c.text_enabled&&c.probe_pending);
    apply("off");assert(!c.active());
    for(const auto* text:{"onion","on more","probe-extra","ping-more","ON","","offx"}){
        assert(parse(text,static_cast<unsigned>(strlen(text)))==Command::Ignore);
        apply(text);assert(!c.active());
    }
    assert(parse(nullptr,2)==Command::Ignore);
    char oversized[40];memset(oversized,' ',sizeof(oversized));oversized[0]='o';oversized[1]='n';
    assert(parse(oversized,sizeof(oversized))==Command::Ignore);
    apply("on");apply("cleanup");assert(c.text_enabled); // Rejected commands do not alter an existing choice.
    apply("off");apply("probe");assert(!c.text_enabled&&c.probe_pending);
    apply("hud");assert(c.hud_enabled&&!c.text_enabled&&c.active());
    apply("probe");assert(c.hud_enabled&&c.probe_pending&&!c.text_enabled);
    apply("ping");assert(c.hud_enabled); // Failed archive stays inaccessible.
    apply("off");assert(!c.active()&&!c.hud_enabled);
    apply("hud");apply("on");assert(c.text_enabled&&!c.hud_enabled);
    apply("hud");assert(c.hud_enabled&&!c.text_enabled);
    apply("off");assert(!c.active());
    PoseLease lease;
    assert(lease.update(7,1,1,10,5));
    assert(lease.update(7,1,1,15,5));
    assert(!lease.update(7,1,1,15.1,5));
    assert(!lease.update(7,1,1,20,5)); // Destroying a visual must not refresh an expired pose.
    assert(lease.update(7,2,1,20,5));
    assert(lease.update(7,2,2,30,5)); // New world can legitimately reuse a sequence.
    assert(lease.update(8,2,2,40,5));
    lease.clear();assert(lease.update(8,2,2,50,5));
}
