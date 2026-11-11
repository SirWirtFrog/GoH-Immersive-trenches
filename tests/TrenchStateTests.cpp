#include "../src/TrenchState.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

// Do not use assert: these checks must also execute in the Release test build.
void Require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

// In-memory engine substitute. Stable token addresses stand in for cap entities;
// no game process, native memory reads or executable addresses are involved.
struct FakeEngine final : trench::Engine {
    std::map<const void*, TrenchVector> live;
    std::set<std::uint64_t> previews;
    int tokens[128]{};
    unsigned spawned = 0, deleted = 0, seated = 0;
    bool haveGround = false;
    float ground = 65.0f;
    std::uint64_t Now() const override { return 100; }
    trench::Entity Identify(const void* p) const override { return {p, p ? 1u : ~0u}; }
    bool IsLive(trench::Entity e) const override { return live.count(e.pointer) != 0; }
    bool IsAt(trench::Entity e, TrenchVector p) const override {
        const auto it = live.find(e.pointer);
        return it != live.end() && std::fabs(it->second.x - p.x) < 1 &&
            std::fabs(it->second.y - p.y) < 1 && std::fabs(it->second.z - p.z) < 1;
    }
    bool IsNetworkPreviewLive(std::uint64_t key) const override { return previews.count(key) != 0; }
    bool ReadGroundHeight(TrenchVector, float& height) const override {
        height = ground;
        return haveGround;
    }
    void* SpawnCap(TrenchVector p, TrenchVector, bool) override {
        Require(spawned < 128, "unexpected cap spawn loop");
        auto* token = &tokens[spawned++]; live[token] = p; return token;
    }
    void DeleteCap(trench::Entity e, bool) override { live.erase(e.pointer); ++deleted; }
    void SeatCap(const trench::Anchor&, trench::Entity) override { ++seated; }
    void LogCap(const char*, const trench::Anchor&, trench::Entity, bool) override {}
    bool ReadTransform(const void*, TrenchTransform&) override { return false; }
    bool ReadCapOffsets(const void*, const TrenchTransform&, float (&)[2]) override { return false; }
};

// A two-ended straight module keeps expected seam positions explicit: the next
// center is 195 units away with the default -5 axial overlap.
constexpr TrenchPoint outline[]{{-100, -20}, {100, -20}, {100, 20}, {-100, 20}};
constexpr TrenchModule module{"section", "preview", outline, 4, 2,
    {{{-100, 0}, 180}, {{100, 0}, 0}}};

TrenchPlacement Prepare(trench::State& state, float x) {
    TrenchTarget target{1, {x, 0, 0}, {x + 10, 0, 0}, {}};
    TrenchPlacement placement;
    Require(state.PrepareTrenchPlacement(module, 0, 0, target, placement), "prepare failed");
    return placement;
}

int main() {
    FakeEngine engine;
    trench::State state(engine);
    int order = 0, nextOrder = 0, section = 0;
    const char* blocked = nullptr;
    const auto first = Prepare(state, 0);
    const auto owner = state.BeginOrder(&order, first, blocked);
    Require(owner != 0 && blocked == nullptr && engine.spawned == 0, "unaccepted order spawned caps");
    Require(Prepare(state, 180).jointCount == 0, "unaccepted order exposed anchors");
    state.FinishOrder(owner, first, true);
    Require(engine.spawned == 2, "accepted order must expose two caps");
    const auto next = Prepare(state, 180);
    Require(next.jointCount == 1 && std::fabs(next.transform.origin.x - 195) < 0.01f, "queued snapping changed");
    Require(state.CheckTrenchPlacement(next) == nullptr, "valid seam was blocked");
    Require(state.CheckTrenchPlacement(first) != nullptr, "queued overlap accepted");
    const auto nextOwner = state.BeginOrder(&nextOrder, next, blocked);
    Require(nextOwner != 0 && blocked == nullptr, "next order rejected");
    state.FinishOrder(nextOwner, next, false);
    Require(Prepare(state, 180).jointCount == 1, "rejected order did not release joint");
    Require(state.ReleaseOrder(&order), "cancel did not release reservation");
    state.SyncTrenchCaps();
    Require(engine.deleted == 2 && Prepare(state, 180).jointCount == 0, "cancel left caps or anchors");

    // An engine callback may destroy a reservation before enqueue returns.
    const auto reentrant = state.BeginOrder(&order, first, blocked);
    Require(state.ReleaseOrder(&order), "reentrant release failed");
    state.FinishOrder(reentrant, first, true);
    Require(engine.spawned == 2, "enqueue resurrected destroyed reservation");

    // Completion transfers ownership away from the order without respawning its
    // existing caps. A repeated completion notification must remain harmless.
    const auto completed = state.BeginOrder(&order, first, blocked);
    state.FinishOrder(completed, first, true);
    engine.live[&section] = first.transform.origin;
    unsigned joined = 0;
    Require(state.TrackTrenchSection(module, first.transform, &section, joined, false, false) == 2,
        "completion lost anchors");
    Require(!state.ReleaseOrder(&order), "completed section retained order reservation");
    Require(engine.seated == 2 && Prepare(state, 180).jointCount == 1, "completion changed caps or snapping");
    state.TrackTrenchSection(module, first.transform, &section, joined, false, false);
    Require(engine.spawned == 4, "duplicate completion spawned extra caps");

    // Remote previews publish temporary sockets immediately, but cap mutation
    // waits for the scene-update path and requires its callback to be available.
    FakeEngine remote;
    trench::State client(remote);
    Require(!client.EnableNetworkTrenchCaps(false), "caps enabled without scene callback");
    Require(client.EnableNetworkTrenchCaps(true), "caps not enabled with scene callback");
    remote.previews.insert(42);
    Require(client.TrackNetworkTrenchPreview(42, module, {}, 0) == 2, "remote preview missing anchors");
    client.SceneUpdated();
    Require(remote.spawned == 2, "remote caps not synced");
    Require(client.RemoveNetworkTrenchPreview(42), "remote preview removal failed");
    client.SceneUpdated();
    Require(remote.deleted == 2, "remote preview left caps");
    // A queued chain on a slope may retain Z=88 while terrain at its ends is 65.
    // Move only local preview visuals, preserving the model's reservation height.
    FakeEngine slope;
    slope.haveGround = true;
    trench::State slopeState(slope);
    auto elevated = first;
    elevated.transform.origin.z = 88.0f;
    const auto slopeOwner = slopeState.BeginOrder(&order, elevated, blocked);
    slopeState.FinishOrder(slopeOwner, elevated, true);
    Require(slope.spawned == 2, "slope preview caps missing");
    for (const auto& entry : slope.live) {
        Require(entry.second.z == 65.0f, "local preview retained inherited height");
    }
    TrenchTarget elevatedTarget{1, {180, 0, 88}, {190, 0, 88}, {}};
    TrenchPlacement elevatedSnap;
    Require(slopeState.PrepareTrenchPlacement(module, 0, 0, elevatedTarget, elevatedSnap) &&
        elevatedSnap.jointCount == 1 && elevatedTarget.first.z == 88.0f,
        "visual grounding modified the reservation pose");

    FakeEngine completedSlope;
    completedSlope.haveGround = true;
    completedSlope.live[&section] = elevated.transform.origin;
    trench::State completedState(completedSlope);
    completedState.TrackTrenchSection(module, elevated.transform, &section, joined, false, false);
    Require(completedSlope.spawned == 2 && completedSlope.seated == 2,
        "completed caps bypassed native seating");
    for (const auto& entry : completedSlope.live) {
        Require(entry.second.z == 88.0f, "preview grounding affected completed section");
    }

    FakeEngine remoteSlope;
    remoteSlope.haveGround = true;
    remoteSlope.previews.insert(7);
    trench::State remoteState(remoteSlope);
    remoteState.EnableNetworkTrenchCaps(true);
    remoteState.TrackNetworkTrenchPreview(7, module, {0, 0, 88}, 0);
    remoteState.SceneUpdated();
    Require(remoteSlope.spawned == 2, "remote slope caps missing");
    for (const auto& entry : remoteSlope.live) {
        Require(entry.second.z == 88.0f, "preview grounding changed remote placement");
    }
    std::puts("TrenchState lifecycle tests passed");
}
