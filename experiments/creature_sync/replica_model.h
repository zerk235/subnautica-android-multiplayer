#pragma once
// Design prototype only: not linked into libsnmod and does not touch Unity.
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>

namespace creature_sync_draft {
constexpr int32_t kBoomerang = 2510;
constexpr size_t kMaxRecords = 128; // Includes removal records until context reset.
using SessionId = std::array<uint8_t, 16>;
enum class Role { Offline, Host, Guest };
enum class Simulation { Native, Suppressed };
enum class Visibility { AwaitingBind, Visible, Dormant, Removed };
enum class Lifecycle { Announce, Suspend, Remove };
struct Context {
    SessionId session{};
    uint32_t host = 0, epoch = 0;
    bool operator==(const Context& b) const {
        return session == b.session && host == b.host && epoch == b.epoch;
    }
    bool valid() const {
        uint8_t bits = 0;
        for (auto b : session) bits |= b;
        return bits && host && epoch;
    }
};
inline bool guid_valid(const std::string& s) {
    if (s.size() != 36) return false;
    bool nonzero = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            nonzero |= c != '0';
        }
    }
    return nonzero;
}
inline bool newer(uint32_t value, uint32_t old) {
    const uint32_t delta = value - old;
    return delta && delta < 0x80000000u;
}
struct Identity {
    std::string id, prefab;
    int32_t tech = 0;
    bool valid() const { return guid_valid(id) && guid_valid(prefab) && tech == kBoomerang; }
    bool operator==(const Identity& b) const {
        return id == b.id && prefab == b.prefab && tech == b.tech;
    }
};
struct Pose {
    uint32_t sequence = 0;
    uint64_t lifecycle_revision = 0;
    std::array<float, 3> position{}, velocity{};
    std::array<float, 4> rotation{0, 0, 0, 1};
    bool valid() const {
        // Draft bounds, not a claim about the game's maximum swim speed/world size.
        for (float v : position) if (!std::isfinite(v) || std::fabs(v) > 100000) return false;
        for (float v : velocity) if (!std::isfinite(v) || std::fabs(v) > 200) return false;
        float norm = 0;
        for (float v : rotation) { if (!std::isfinite(v)) return false; norm += v * v; }
        return norm > .99f && norm < 1.01f;
    }
};
struct Record {
    Identity identity;
    uint64_t revision = 0;
    Visibility visibility = Visibility::AwaitingBind;
    bool bound = false, has_pose = false;
    Pose latest;
};
class GuestRegistry {
    Context context_;
    Role role_ = Role::Offline;
    bool experimental_ = false;
    std::map<std::string, Record> records_;
    bool authorized(uint32_t sender, const Context& context) const {
        return experimental_ && role_ == Role::Guest && context_.valid() &&
               sender == context_.host && context == context_;
    }
public:
    // Caller must restore old Unity component states BEFORE changing role/context.
    // Network silence alone MUST NOT call configure(Offline).
    void configure(Role role, const Context& context, bool experimental) {
        if (role != role_ || !(context == context_) || experimental != experimental_) records_.clear();
        role_ = role; context_ = context; experimental_ = experimental;
    }
    Simulation simulation_for(int32_t tech, bool wild, bool in_domain) const {
        // Independent of current packets/known identity: local unmatched fish in
        // the managed domain are suppressed too, then hidden pending host data.
        return experimental_ && role_ == Role::Guest && tech == kBoomerang && wild && in_domain
               ? Simulation::Suppressed : Simulation::Native;
    }
    bool lifecycle(uint32_t sender, const Context& context, const Identity& identity,
                   uint64_t revision, Lifecycle operation) {
        if (!authorized(sender, context) || !identity.valid() || !revision) return false;
        if (operation != Lifecycle::Announce && operation != Lifecycle::Suspend && operation != Lifecycle::Remove) return false;
        auto found = records_.find(identity.id);
        if (found == records_.end()) {
            // Remember early removal too: a delayed announce cannot resurrect it.
            if (records_.size() >= kMaxRecords) return false;
            Record initial; initial.identity = identity;
            found = records_.emplace(identity.id, initial).first;
        }
        auto& r = found->second;
        if (!(r.identity == identity) || revision <= r.revision || r.visibility == Visibility::Removed) return false;
        r.revision = revision; r.has_pose = false;
        if (operation == Lifecycle::Remove) { r.visibility = Visibility::Removed; r.bound = false; }
        else if (operation == Lifecycle::Suspend) r.visibility = Visibility::Dormant;
        else r.visibility = r.bound ? Visibility::Visible : Visibility::AwaitingBind;
        return true;
    }
    bool bind(const std::string& id, bool native_simulation_verified_suppressed) {
        auto found = records_.find(id);
        if (!experimental_ || role_ != Role::Guest || !context_.valid() || !native_simulation_verified_suppressed || found == records_.end()) return false;
        auto& r = found->second;
        if (r.visibility == Visibility::Removed || r.visibility == Visibility::Dormant) return false;
        r.bound = true; r.visibility = Visibility::Visible; return true;
    }
    void unbind(const std::string& id) {
        auto found = records_.find(id);
        if (found == records_.end()) return;
        // Preserve the last sequence across local stream-out/rebind. Otherwise
        // delayed packets from before unloading could move this fish backwards.
        auto& r = found->second; r.bound = false;
        if (r.visibility == Visibility::Visible) r.visibility = Visibility::AwaitingBind;
    }
    bool pose(uint32_t sender, const Context& context, const std::string& id, const Pose& pose) {
        if (!authorized(sender, context) || !pose.valid()) return false;
        auto found = records_.find(id);
        if (found == records_.end()) return false;
        auto& r = found->second;
        if (!r.bound || r.visibility != Visibility::Visible || pose.lifecycle_revision != r.revision ||
            (r.has_pose && !newer(pose.sequence, r.latest.sequence))) return false;
        r.latest = pose; r.has_pose = true; return true;
    }
    const Record* find(const std::string& id) const {
        auto it = records_.find(id); return it == records_.end() ? nullptr : &it->second;
    }
    size_t size() const { return records_.size(); }
};
} // namespace creature_sync_draft
