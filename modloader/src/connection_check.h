#pragma once

#include <cstdint>

namespace connection_check {

constexpr double kWaitSeconds = 30.0;

enum class Stage { Idle, GuestDiscovery, GuestTransfer, GuestLoad, HostWaiting };

struct Wait {
    Stage stage = Stage::Idle;
    double since = 0;
    uint64_t progress = 0;
    bool reported = false;

    bool update(Stage next, uint64_t current_progress, double now) {
        if (next != stage || current_progress != progress) {
            *this = {next, now, current_progress, false};
        }
        if (stage == Stage::Idle || reported || now - since < kWaitSeconds) return false;
        reported = true;
        return true;
    }
};

// Inputs share the same byte order. Without a mask there is no subnet evidence.
inline bool same_subnet(uint32_t local, uint32_t mask, uint32_t peer) {
    return mask && ((local ^ peer) & mask) == 0;
}

enum class Discovery {
    NoLanAddress, NoPeers, OtherSubnet, IncompatibleHost, HostNotReady, JoinUnanswered
};

struct VisiblePeers {
    unsigned total = 0;
    unsigned hosts = 0;
    unsigned incompatible = 0;
    unsigned compatible_hosts = 0;
    unsigned same_subnet_hosts = 0;
    unsigned same_subnet_peers = 0;
};

inline Discovery diagnose(bool has_lan_address, const VisiblePeers& peers) {
    if (!has_lan_address) return Discovery::NoLanAddress;
    if (!peers.total) return Discovery::NoPeers;
    if (peers.compatible_hosts && !peers.same_subnet_hosts) return Discovery::OtherSubnet;
    if (peers.compatible_hosts) return Discovery::JoinUnanswered;
    if (peers.hosts || peers.incompatible == peers.total) return Discovery::IncompatibleHost;
    if (!peers.same_subnet_peers) return Discovery::OtherSubnet;
    return Discovery::HostNotReady;
}

}  // namespace connection_check
