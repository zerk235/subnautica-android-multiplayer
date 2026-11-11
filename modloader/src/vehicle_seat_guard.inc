#include "vehicle_occupancy.h"

using VehicleClickFn=void (*)(void*,void*,void*);
using VehicleEnterFn=void (*)(void*,void*,bool,bool,void*);
VehicleClickFn g_seamoth_click_original=nullptr;
VehicleEnterFn g_seamoth_enter_original=nullptr;
bool g_seamoth_seat_hooks=false;
// Unity 6000.0.44f1's class header differs from the old generated dump header.
// Native SetupVTable reads the count at +0x12a (0x2560764, 0x2560b14), and
// the table still starts at +0x138. +0x126 is an event count, not this count.
constexpr size_t kSeamothVtableCountOffset=0x12a;

std::uint32_t seamoth_seat_occupant(void* vehicle) {
    const auto state=net_session_state();
    if(state!=kSessionHost&&state!=kSessionGuest)return 0;
    std::uint8_t guid[16]{};
    try {
        bool replica=false;
        // A prefab replica has its own local UniqueIdentifier. Its exact scene
        // object is already associated with the wire GUID by the vehicle sync.
        // Use that association, never a nearest-vehicle / position heuristic.
        void* go=g_get_game_object?g_get_game_object.call(vehicle,g_get_game_object.method):nullptr;
        for(const auto& driven:g_driven) {
            if(go&&driven.used&&driven.game_object==go&&
               !std::memcmp(driven.classId,"SeaMoth",7)&&!driven.classId[7]) {
                std::memcpy(guid,driven.guid,16);replica=true;break;
            }
        }
        if(!replica&&!component_guid(vehicle,guid))return 0;
        VehicleState vehicles[8]{};PeerState peers[kMaxPeers]{};
        const auto count=net_copy_vehicles(vehicles,8);
        const auto peer_count=net_copy_peers(peers,kMaxPeers);
        const double now=monotonic_seconds();
        const auto pilot=remote_seamoth_pilot(guid,net_local_id(),vehicles,count,peers,peer_count,now);
        static double next_check_log=0;
        if(now>=next_check_log) {
            next_check_log=now+1;
            LOG_INFO("vehicle seat: entry check source=%s pilot=%08x vehicles=%zu peers=%zu",
                     replica?"replica":"native",pilot,count,peer_count);
        }
        return pilot;
    } catch(...) {
        LOG_WARN("vehicle seat: identity lookup failed; original interaction retained");
        return 0;
    }
}

void seamoth_seat_notice(std::uint32_t pilot,const char* entry) {
    static double next_notice=0;
    const double now=monotonic_seconds();
    if(now<next_notice)return;
    next_notice=now+1;
    LOG_INFO("vehicle seat: entry blocked type=SeaMoth pilot=%08x path=%s",pilot,entry);
    // A UI failure must never turn a known occupied seat into an allowed entry.
    try {
        static Entry<void (*)(void*,void*)> message;
        if(!message)message.bind(find_method_typed(find_class_in_domain("","ErrorMessage"),"AddMessage",1,"system.string"));
        void* text=cached_managed_string("Мотылёк занят напарником");
        if(message&&text)message.call(text,message.method);
    } catch(...) {LOG_WARN("vehicle seat: occupied message unavailable");}
}

void hooked_seamoth_hand_click(void* vehicle,void* hand,void* method) {
    if(const auto pilot=seamoth_seat_occupant(vehicle)) {seamoth_seat_notice(pilot,"hand-click");return;}
    if(g_seamoth_click_original)g_seamoth_click_original(vehicle,hand,method);
}
void hooked_seamoth_enter(void* vehicle,void* player,bool teleport,bool animation,void* method) {
    if(const auto pilot=seamoth_seat_occupant(vehicle)) {seamoth_seat_notice(pilot,"enter");return;}
    if(g_seamoth_enter_original)g_seamoth_enter_original(vehicle,player,teleport,animation,method);
}

bool seamoth_patch_slot(Il2CppClass* klass,Il2CppMethodInfo* method,unsigned expected_slot,
                       void* replacement,void** original) {
    if(!klass||!method)return false;
    auto* info=reinterpret_cast<char*>(method);
    const unsigned slot=*reinterpret_cast<unsigned short*>(info+kMethodSlotOffset);
    const unsigned count=*reinterpret_cast<unsigned short*>(reinterpret_cast<char*>(klass)+kSeamothVtableCountOffset);
    if(slot!=expected_slot||slot>=count)return false;
    void* address=reinterpret_cast<char*>(klass)+kClassVtableOffset+slot*kVirtualInvokeSize;
    void* current=*reinterpret_cast<void**>(address);
    if(current==replacement)return *original!=nullptr;
    void* direct=*reinterpret_cast<void**>(info+kMethodPointerOffset);
    void* virt=*reinterpret_cast<void**>(info+kVirtualPointerOffset);
    if(!current||(current!=direct&&current!=virt))return false;
    *original=current;
    if(write_pointer(address,replacement))return true;
    *original=nullptr;return false;
}

void install_seamoth_seat_hooks() {
    static double next_attempt=0;
    if(g_seamoth_seat_hooks||monotonic_seconds()<next_attempt)return;
    next_attempt=monotonic_seconds()+5;
    auto* seamoth=find_class_in_domain("","SeaMoth");
    auto* vehicle=find_class_in_domain("","Vehicle");
    if(!seamoth||!vehicle)return;
    auto* click=find_method_typed(vehicle,"OnHandClick",1,"guihand");
    auto* enter=find_method_typed(seamoth,"EnterVehicle",3,"player");
    // Change only SeaMoth's virtual table. Vehicle.OnHandClick is inherited,
    // but changing its shared MethodInfo would also intercept other vehicles.
    void* click_original=reinterpret_cast<void*>(g_seamoth_click_original);
    void* enter_original=reinterpret_cast<void*>(g_seamoth_enter_original);
    const bool clicked=seamoth_patch_slot(seamoth,click,6,reinterpret_cast<void*>(&hooked_seamoth_hand_click),&click_original);
    const bool entered=seamoth_patch_slot(seamoth,enter,53,reinterpret_cast<void*>(&hooked_seamoth_enter),&enter_original);
    g_seamoth_click_original=reinterpret_cast<VehicleClickFn>(click_original);
    g_seamoth_enter_original=reinterpret_cast<VehicleEnterFn>(enter_original);
    g_seamoth_seat_hooks=clicked&&entered;
    const unsigned count=*reinterpret_cast<unsigned short*>(reinterpret_cast<char*>(seamoth)+kSeamothVtableCountOffset);
    LOG_INFO("vehicle seat: SeaMoth hooks click=%u enter=%u ready=%u vtable_count=%u",unsigned(clicked),unsigned(entered),unsigned(g_seamoth_seat_hooks),count);
}
