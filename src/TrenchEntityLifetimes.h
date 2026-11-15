#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <mutex>

namespace trench {
// Leases for entities observed through the admitted GEM add/remove call site.
// No native memory is read here. A pointer is only a map key, never an object.
class EntityLifetimes {
public:
    enum class Liveness { Alive, Gone, Unavailable };

    void BeginScene() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        generations_.clear();
        ready_ = nextToken_ != (std::numeric_limits<std::uint64_t>::max)();
    }
    void Invalidate() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        generations_.clear();
        ready_ = false;
    }
    void Observe(const void* entity, bool added) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (entity == nullptr) {
            generations_.clear(); ready_ = false;
            return;
        }
        // An ordinary event cannot heal an observer fault or scene teardown.
        if (!ready_) return;
        if (!added) { generations_.erase(entity); return; }
        if (nextToken_ == (std::numeric_limits<std::uint64_t>::max)()) {
            generations_.clear(); ready_ = false;
            return;
        }
        try {
            generations_[entity] = nextToken_++;
        } catch (...) {
            // Allocation failure must not leave an old lease apparently alive.
            generations_.clear(); ready_ = false;
        }
    }
    std::uint64_t Generation(const void* entity) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_ || entity == nullptr) return 0;
        const auto entry = generations_.find(entity);
        return entry == generations_.end() ? 0 : entry->second;
    }
    Liveness Query(const void* entity, std::uint64_t token) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_ || token == 0) return Liveness::Unavailable;
        const auto entry = generations_.find(entity);
        return entry != generations_.end() && entry->second == token ? Liveness::Alive : Liveness::Gone;
    }
    bool Ready() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return ready_;
    }

private:
    mutable std::mutex mutex_;
    std::map<const void*, std::uint64_t> generations_;
    std::uint64_t nextToken_ = 1;
    bool ready_ = false;
};
}
