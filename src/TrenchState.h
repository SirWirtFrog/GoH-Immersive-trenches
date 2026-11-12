#pragma once
#include "TrenchTypes.h"
#include <memory>

namespace trench {
struct Entity {
    const void* pointer;
    std::uint32_t id;
};

struct Anchor {
    // Stable model identity: owner names a section/reservation, side a catalog socket.
    std::uint64_t owner;
    // World-space seam position and outward-facing unit direction.
    TrenchVector seam;
    TrenchVector outward;
    unsigned side;
    bool completed = false; // False while the socket belongs to a queued order.
    bool hasBoneOffset = false;
    float boneOffset = 0.0f;
    bool replicated = false; // Remote-client tracking; caps use the client-local ID pool.
};


// Engine operations only: the model never reads GEM memory or calls an RVA.
struct Engine {
    virtual ~Engine() = default;
    virtual std::uint64_t Now() const = 0;
    virtual Entity Identify(const void*) const = 0;
    virtual bool IsLive(Entity) const = 0;
    virtual bool IsAt(Entity, TrenchVector) const = 0;
    virtual bool IsNetworkPreviewLive(std::uint64_t) const = 0;
    virtual bool ReadGroundHeight(TrenchVector position, float& height) const = 0;
    virtual void* SpawnCap(TrenchVector, TrenchVector, bool) = 0;
    virtual void DeleteCap(Entity, bool) = 0;
    virtual void SeatCap(const Anchor&, Entity) = 0;
    virtual void LogCap(const char*, const Anchor&, Entity, bool = false) = 0;
    // Engine reads can fail during scene changes; the model skips that height sample.
    virtual bool ReadTransform(const void*, TrenchTransform&) = 0;
    virtual bool ReadCapOffsets(const void*, const TrenchTransform&, float (&)[2]) = 0;
};

// Owns topology, placement geometry and order reservations for one process.
// Calls into Engine retain the original callback ordering and lock boundaries.
class State {
public:
    explicit State(Engine& engine);
    ~State();
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    bool PrepareTrenchPlacement(const TrenchModule& module, float modelYawOffset, unsigned char option, TrenchTarget& target, TrenchPlacement& placement) noexcept;
    const char* CheckTrenchPlacement(const TrenchPlacement& placement) noexcept;
    // Two-phase queue transition: claim before native enqueue, finish after its
    // return. ReleaseOrder can run between them through a native callback.
    std::uint64_t BeginOrder(const void* order, const TrenchPlacement& placement, const char*& blocked);
    void FinishOrder(std::uint64_t owner, const TrenchPlacement& placement, bool accepted);
    bool ReleaseOrder(const void* order);
    // Native callbacks may reenter synchronization; the implementation coalesces
    // requests and releases the state mutex around cap mutation calls.
    void SyncTrenchCaps();
    void SceneUpdated();
    unsigned TrackNetworkTrenchPreview(std::uint64_t key, const TrenchModule& module, TrenchVector position, float yaw);
    bool RemoveNetworkTrenchPreview(std::uint64_t key);
    bool EnableNetworkTrenchCaps(bool available) noexcept;
    void ClearNetworkTrenches();
    unsigned TrackTrenchSection(const TrenchModule& module, const TrenchTransform& transform, const void* section, unsigned& joinedCount, bool useStraightBones, bool replicated);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace trench
