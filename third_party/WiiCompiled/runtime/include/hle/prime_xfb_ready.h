#pragma once
#include <cstdint>
#include <unordered_map>

namespace PrimeXfbReady {
// Different XFB destinations can be copied before VI selects either one.
// A newer copy arriving while a presentation waits must stay pending.
class Tracker {
    std::unordered_map<uint32_t, uint64_t> pending_;
    uint64_t generation_ = 0;
public:
    void MarkCopy(uint32_t address) { if (address) pending_[address] = ++generation_; }
    uint64_t Revision(uint32_t address) const {
        const auto it = pending_.find(address);
        return it == pending_.end() ? 0 : it->second;
    }
    void Presented(uint32_t address, uint64_t revision) {
        const auto it = pending_.find(address);
        if (it != pending_.end() && it->second == revision) pending_.erase(it);
    }
};
}
