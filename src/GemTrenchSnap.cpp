#define WIN32_LEAN_AND_MEAN
#include "GemTrenchSnap.h"
#include "GemBranchRelay.h"
#include "TrenchState.h"
#include "GemSceneLifecycle.h"
#include "TrenchEntityLifetimes.h"
#include <atomic>

using trench::Entity;
using trench::Anchor;
using trench::Liveness;

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#if defined(IT_CAP_AFFINE_PROBE) && defined(_DEBUG)
#include <vector>
#endif

static_assert(sizeof(TrenchTarget) == 0xa8);
static_assert(offsetof(TrenchTarget, first) == 4);
static_assert(offsetof(TrenchTarget, second) == 0x10);

// Links synchronous placement, preview and enqueue callbacks on one thread.
// Nested placements save and restore the previous scope.
thread_local TrenchOrderScope* activeTrenchOrder = nullptr;

namespace {
constexpr float radiansPerDegree = 0.01745329252f;

using Enqueue = void* (*)(void*, void*, const void*);
using DestroyOrder = void* (*)(void*, unsigned);
using SceneQuant = void (*)(void*);

const unsigned char* hostImage = nullptr;
DWORD hostTimestamp = 0;
DWORD entityTreeRva = 0;
void (*writeCapLog)(const char*) = nullptr;
void** distributorVtable = nullptr;
Enqueue originalEnqueue = nullptr;
DestroyOrder originalDestroy = nullptr;
SceneQuant originalSceneQuant = nullptr;
std::atomic<std::uint64_t> nativeSceneSerial{1};
void* activeNativeScene = nullptr;
bool sceneLifecycleInstalled = false;
bool nativeSceneInitialized = false;
bool entityLifetimeInstalled = false;
trench::EntityLifetimes nativeLifetimes;
// A failed memory read makes the observation unavailable; it does not prove
// that the engine object remains alive after a successful read.
template <typename T>
bool Read(const void* pointer, T& value) noexcept {
    return ReadProcessMemory(GetCurrentProcess(), pointer, &value, sizeof(value), nullptr) != FALSE;
}

// Store both pointer and ID: pointer reuse alone must not identify an entity.
Entity Identify(const void* pointer) noexcept {
    DWORD id = 0xffffffff;
    if (pointer != nullptr) {
        Read(static_cast<const unsigned char*>(pointer) + 0xc0, id);
    }
    const auto generation = entityLifetimeInstalled ? nativeLifetimes.Generation(pointer) : 0;
    // A loaded/foreign or unobserved entity must not become a new owned cap.
    if (entityLifetimeInstalled && generation == 0) id = 0xffffffff;
    return {pointer, id, generation};
}

Liveness QueryEntity(Entity entity) noexcept {
    if (entity.pointer == nullptr || entity.id == 0xffffffff) {
        return Liveness::Gone;
    }
    if (entityLifetimeInstalled) {
        const auto lease = nativeLifetimes.Query(entity.pointer, entity.generation);
        if (lease == trench::EntityLifetimes::Liveness::Gone) return Liveness::Gone;
        if (lease == trench::EntityLifetimes::Liveness::Unavailable) return Liveness::Unavailable;
    } else if (entity.generation != 0) return Liveness::Unavailable;

    // Read the same ID tree as GEM's entity lookup. The preview runs on the UI thread,
    // so each read can fail if the simulation changes the tree or unloads the scene.
    struct Node {
        const Node* left;
        const Node* parent;
        const Node* right;
        unsigned char color;
        unsigned char nil;
        unsigned char padding[6];
        DWORD id;
        const void* entity;
    };
    static_assert(offsetof(Node, id) == 0x20 && offsetof(Node, entity) == 0x28);
    const Node* head = nullptr;
    Node node{};
    if (hostImage == nullptr || !Read(hostImage + entityTreeRva, head) || head == nullptr || !Read(head, node)) {
        return Liveness::Unavailable;
    }
    const Node* current = node.parent;
    for (unsigned depth = 0; current != head && depth < 64; ++depth) {
        if (!Read(current, node)) {
            return Liveness::Unavailable;
        }
        if (node.nil) {
            return Liveness::Gone;
        }
        if (node.id == entity.id) {
            DWORD flags = 0;
            if (node.entity != entity.pointer) return Liveness::Gone;
            if (!Read(static_cast<const unsigned char*>(entity.pointer) + 0x38, flags)) {
                return Liveness::Unavailable;
            }
            if (entityLifetimeInstalled) {
                const auto lease = nativeLifetimes.Query(entity.pointer, entity.generation);
                if (lease == trench::EntityLifetimes::Liveness::Gone) return Liveness::Gone;
                if (lease == trench::EntityLifetimes::Liveness::Unavailable) return Liveness::Unavailable;
            }
            return (flags & 0x10020) == 0 ? Liveness::Alive : Liveness::Gone;
        }
        current = node.id > entity.id ? node.left : node.right;
    }
    return current == head ? Liveness::Gone : Liveness::Unavailable;
}

#if defined(IT_CAP_AFFINE_PROBE) && defined(_DEBUG)
struct CapProbeSample {
    Entity entity;
    TrenchTransform expected;
    unsigned frames = 0;
    bool mismatch = false;
};
std::vector<CapProbeSample> capProbeSamples;

bool SameProbeMatrix(const TrenchTransform& a, const TrenchTransform& b) {
    const TrenchVector left[]{a.xAxis, a.yAxis, a.zAxis, a.origin};
    const TrenchVector right[]{b.xAxis, b.yAxis, b.zAxis, b.origin};
    for (unsigned i = 0; i < 4; ++i) {
        if (!std::isfinite(left[i].x) || !std::isfinite(left[i].y) || !std::isfinite(left[i].z) ||
            std::fabs(left[i].x - right[i].x) > 0.0001f ||
            std::fabs(left[i].y - right[i].y) > 0.0001f ||
            std::fabs(left[i].z - right[i].z) > 0.0001f) return false;
    }
    return true;
}

// Observe before our own state executor can repair an engine-overwritten pose.
// ID/tree checks precede each read; this observer never writes/deletes entities.
void ObserveCapProbe() {
    for (auto it = capProbeSamples.begin(); it != capProbeSamples.end();) {
        const auto live = QueryEntity(it->entity);
        if (live == Liveness::Gone) { it = capProbeSamples.erase(it); continue; }
        if (live == Liveness::Unavailable) { it->frames = 0; ++it; continue; }
        TrenchTransform actual{};
        if (!Read(static_cast<const unsigned char*>(it->entity.pointer) + 0x50, actual)) {
            it->frames = 0;
            ++it;
            continue;
        }
        const bool same = SameProbeMatrix(actual, it->expected);
        if (!same && !it->mismatch && writeCapLog != nullptr) {
            char message[240];
            std::snprintf(message, sizeof(message),
                "[GEM cap_probe] matrix_mismatch pid=%lu id=%08lx resource=it_trench_dirt_end_cap frame=%u xz=%.4f yz=%.4f\r\n",
                GetCurrentProcessId(), it->entity.id, it->frames, actual.xAxis.z, actual.yAxis.z);
            writeCapLog(message);
        }
        if (!same) { it->frames = 0; it->mismatch = true; }
        else if (!it->mismatch && it->frames < 120 && ++it->frames == 120 && writeCapLog != nullptr) {
            char message[240];
            std::snprintf(message, sizeof(message),
                "[GEM cap_probe] matrix_stable pid=%lu id=%08lx resource=it_trench_dirt_end_cap frames=120 a=%.4f b=%.4f renderer=NOT_TESTED\r\n",
                GetCurrentProcessId(), it->entity.id, it->expected.xAxis.z, it->expected.yAxis.z);
            writeCapLog(message);
        }
        ++it;
    }
}
#endif

// An anchor is usable only while its owning entity remains at the recorded
// XY origin. Native terrain alignment is allowed to change root Z.
bool IsAt(Entity entity, TrenchVector origin) noexcept {
    TrenchVector currentOrigin{};
    return QueryEntity(entity) == Liveness::Alive &&
           Read(static_cast<const unsigned char*>(entity.pointer) + 0x74, currentOrigin) &&
           std::isfinite(currentOrigin.x) && std::isfinite(currentOrigin.y) &&
           ((currentOrigin.x - origin.x) * (currentOrigin.x - origin.x) +
            (currentOrigin.y - origin.y) * (currentOrigin.y - origin.y)) <= 1.0f;
}

bool IsNetworkPreviewLive(std::uint64_t key) noexcept {
    // The GEM visualizer map orders nodes by (local flag, preview key).
    constexpr DWORD treeRva = 0x15d03e8;
    struct Node {
        const Node* left;
        const Node* parent;
        const Node* right;
        unsigned char color, nil, padding[6];
        std::uint64_t key;
        unsigned char local;
    };
    static_assert(offsetof(Node, key) == 0x20 && offsetof(Node, local) == 0x28);
    const Node* head = nullptr;
    Node node{};
    if (!Read(hostImage + treeRva, head) || !Read(head, node)) {
        return false;
    }
    auto* current = node.parent;
    for (unsigned depth = 0; current != head && depth < 64; ++depth) {
        if (!Read(current, node) || node.nil) {
            return false;
        }
        if (node.local == 0 && node.key == key) {
            return true;
        }
        current = node.local != 0 || key < node.key ? node.left : node.right;
    }
    return false;
}

// Optionally sample the actual cap pose to distinguish model socket height
// from the final native entity position in diagnostics.
void LogCap(const char* event, const Anchor& anchor, Entity entity, bool measureHeight = false) {
    if (writeCapLog != nullptr) {
        char message[640];
        const int length = std::snprintf(message, sizeof(message),
                      "[GEM caps] %s pid=%lu anchor=%llu:%u socket_id=%u entity=%p id=%08lx client_only=%u resource=it_trench_dirt_end_cap "
                      "generation=%llu position=(%.2f,%.2f,%.2f)",
                      event, GetCurrentProcessId(), static_cast<unsigned long long>(anchor.owner), anchor.side,
                      anchor.socketId, entity.pointer, entity.id, static_cast<unsigned>(anchor.localCap),
                      static_cast<unsigned long long>(entity.generation), anchor.seam.x, anchor.seam.y, anchor.seam.z);
        if (length < 0 || static_cast<std::size_t>(length) >= sizeof(message)) {
            return;
        }
        TrenchTransform actual{};
        const bool readPose = measureHeight || std::strcmp(event, "spawned_completed") == 0;
        if (readPose && QueryEntity(entity) == Liveness::Alive &&
            Read(static_cast<const unsigned char*>(entity.pointer) + 0x50, actual)) {
            DWORD flags = 0;
            Read(static_cast<const unsigned char*>(entity.pointer) + 0x38, flags);
            const float pitch = std::atan2(actual.xAxis.z, std::hypot(actual.xAxis.x, actual.xAxis.y));
            const char* source = "native_land_patch";
#if defined(IT_CAP_AFFINE_PROBE) && defined(_DEBUG)
            source = "affine_probe";
#endif
            std::snprintf(message + length, sizeof(message) - length,
                          " actual_position=(%.2f,%.2f,%.2f) pitch=%.2f height_offset=%.2f height_source=%s "
                          "yaw=%.2f expected_yaw=%.2f yaw_error=%.2f x_axis_z=%.4f y_axis_z=%.4f flags=%08lx\r\n",
                          actual.origin.x, actual.origin.y, actual.origin.z, pitch / radiansPerDegree,
                          actual.origin.z - anchor.seam.z, source,
                          std::atan2(actual.xAxis.y, actual.xAxis.x) / radiansPerDegree,
                          std::atan2(-anchor.outward.y, -anchor.outward.x) / radiansPerDegree,
                          std::remainder(std::atan2(actual.xAxis.y, actual.xAxis.x) -
                              std::atan2(-anchor.outward.y, -anchor.outward.x), 6.28318530718f) / radiansPerDegree,
                          actual.xAxis.z, actual.yAxis.z, flags);
        } else {
            std::snprintf(message + length, sizeof(message) - length, "\r\n");
        }
        writeCapLog(message);
    }
}

bool SeatCap(const Anchor& anchor, Entity entity) {
    if (!anchor.completed || QueryEntity(entity) != Liveness::Alive) return false;
    struct Guard { Entity entity; std::uint64_t serial; } guard{entity, nativeSceneSerial.load()};
    const auto mayRead = [](void* context) {
        const auto& lease = *static_cast<const Guard*>(context);
        return nativeSceneSerial.load() == lease.serial && QueryEntity(lease.entity) == Liveness::Alive;
    };
    const bool applied = SeatCompletedTrenchCap(hostImage, hostTimestamp, entity.pointer, anchor.seam, anchor.outward, mayRead, &guard);
    if (nativeSceneSerial.load() != guard.serial) return false;
    return applied && QueryEntity(entity) == Liveness::Alive;
}


// The only bridge from the state model to GEM memory and native functions.
// The model itself owns no executable addresses or native object layouts.
struct GemEngine final : trench::Engine {
    ULONGLONG Now() const override { return GetTickCount64(); }
    Entity Identify(const void* p) const override { return ::Identify(p); }
    Liveness QueryEntity(Entity e) const override { return ::QueryEntity(e); }
    bool IsAt(Entity e, TrenchVector p) const override { return ::IsAt(e, p); }
    bool IsNetworkPreviewLive(std::uint64_t k) const override { return ::IsNetworkPreviewLive(k); }
    void* SpawnCap(TrenchVector p, TrenchVector d, bool local) override {
        if (!nativeSceneInitialized || !entityLifetimeInstalled || !nativeLifetimes.Ready()) return nullptr;
        return SpawnTrenchCap(hostImage, hostTimestamp, p, d, local);
    }
    void DeleteCap(Entity e, bool local) override { DeleteTrenchCap(hostImage, hostTimestamp, e.pointer, local); }
    bool SeatCap(const Anchor& a, Entity e) override { return ::SeatCap(a, e); }
    void LogCap(const char* event, const Anchor& a, Entity e, bool measure) override { ::LogCap(event, a, e, measure); }
    bool ReadTransform(const void* p, TrenchTransform& pose) override {
        return Read(static_cast<const unsigned char*>(p) + 0x50, pose);
    }
};
GemEngine engine;
trench::State state(engine);

void LogScene(const char* event, void* scene) noexcept {
    if (!writeCapLog) return;
    char message[240];
    std::snprintf(message, sizeof(message), "[GEM scene] %s pid=%lu scene=%p serial=%llu affine_admitted=0\r\n",
        event, GetCurrentProcessId(), scene, static_cast<unsigned long long>(nativeSceneSerial.load()));
    writeCapLog(message);
}
void BeforeSceneInit(void* scene) noexcept {
    nativeSceneSerial.fetch_add(1);
    nativeSceneInitialized = false;
    activeNativeScene = nullptr;
    state.SceneInvalidating();
    if (entityLifetimeInstalled) nativeLifetimes.BeginScene();
#if defined(IT_CAP_AFFINE_PROBE) && defined(_DEBUG)
    capProbeSamples.clear();
#endif
    LogScene("begin", scene);
}
void AfterSceneInit(void* scene) noexcept {
    activeNativeScene = scene;
    nativeSceneInitialized = true;
    state.SceneReady();
    LogScene("init_complete", scene);
}
void BeforeSceneDestroy(void* scene) noexcept {
    nativeSceneSerial.fetch_add(1);
    nativeSceneInitialized = false;
    activeNativeScene = nullptr;
    state.SceneInvalidating();
    nativeLifetimes.Invalidate();
#if defined(IT_CAP_AFFINE_PROBE) && defined(_DEBUG)
    capProbeSamples.clear();
#endif
    LogScene("invalidating", scene);
}
void ObserveEntityLifetime(const void* entity, bool added) noexcept {
    nativeLifetimes.Observe(entity, added);
    if (!entity && writeCapLog) {
        writeCapLog("[GEM lifetime] registry_invalidated: unreadable add/remove event; new ownership disabled until next native init.\r\n");
    }
}

// Read settled heights after native scene simulation, preserving its error value.
void SceneQuantWithCapRefresh(void* scene) {
    originalSceneQuant(scene);
    const DWORD lastError = GetLastError();
    if (sceneLifecycleInstalled && (!nativeSceneInitialized || activeNativeScene != scene)) {
        SetLastError(lastError);
        return;
    }
#if defined(IT_CAP_AFFINE_PROBE) && defined(_DEBUG)
    ObserveCapProbe();
#endif
    state.SceneUpdated();
    SetLastError(lastError);
}

// Detach the reservation before GEM destroys the order. Dropping its
// anchors schedules cap reconciliation in the scene update.
void* DestroyTrenchOrder(void* order, unsigned flags) {
    const DWORD lastError = GetLastError();
    state.ReleaseOrder(order);
    SetLastError(lastError);
    return originalDestroy(order, flags);
}

// Intercept only the distributor belonging to the active mod placement.
// Claim in the model, enqueue natively, then publish or discard its anchors.
void* EnqueueTrenchOrder(void* actor, void* order, const void* options) {
    auto* scope = activeTrenchOrder;
    if (scope == nullptr || order == nullptr || *static_cast<void***>(order) != distributorVtable ||
        *reinterpret_cast<const void* const*>(static_cast<const unsigned char*>(order) + 0x390) != scope->action) {
        return originalEnqueue(actor, order, options);
    }

    const DWORD lastError = GetLastError();
    const auto& placement = scope->placement;
    const auto owner = state.BeginOrder(order, placement, scope->blocked);
    SetLastError(lastError);
    if (scope->blocked != nullptr) {
        // Match the engine's rejected-order path: stop the order, then delete it.
        using StopOrder = void (*)(void*);
        reinterpret_cast<StopOrder>(distributorVtable[4])(order);
        originalDestroy(order, 1);
        return nullptr;
    }

    auto* accepted = originalEnqueue(actor, order, options);
    const DWORD enqueueError = GetLastError();
    state.FinishOrder(owner, placement, accepted != nullptr);
    SetLastError(enqueueError);
    scope->reserved |= accepted != nullptr;
    return accepted;
}

} // namespace

#if defined(IT_CAP_AFFINE_PROBE) && defined(_DEBUG)
void RecordTrenchCapAffineProbe(const void* pointer, const TrenchTransform& expected) {
    const auto entity = Identify(pointer);
    if (QueryEntity(entity) != Liveness::Alive) return;
    for (auto& sample : capProbeSamples) {
        if (sample.entity.pointer != entity.pointer || sample.entity.id != entity.id) continue;
        if (!SameProbeMatrix(expected, sample.expected)) sample = {entity, expected};
        return;
    }
    if (capProbeSamples.size() >= 64) return;
    capProbeSamples.push_back({entity, expected});
    if (writeCapLog != nullptr) {
        char message[240];
        std::snprintf(message, sizeof(message),
            "[GEM cap_probe] applied pid=%lu id=%08lx resource=it_trench_dirt_end_cap a=%.4f b=%.4f test_only=1 renderer=NOT_TESTED\r\n",
            GetCurrentProcessId(), entity.id, expected.xAxis.z, expected.yAxis.z);
        writeCapLog(message);
    }
}
#endif

// Redirect the verified scene tail jump; the wrapper runs the original
// scene update before refreshing caps.
bool InstallTrenchHeightRefresh(unsigned char* image, DWORD sceneQuantJump, DWORD sceneQuantEntry) noexcept {
    auto* jump = image + sceneQuantJump;
    const auto expected = static_cast<std::int32_t>(sceneQuantEntry) - static_cast<std::int32_t>(sceneQuantJump + 5);
    if (*jump != 0xe9 || std::memcmp(jump + 1, &expected, sizeof(expected)) != 0) {
        return false;
    }
    auto* relay = AllocateGemRelayNear(jump + 5);
    if (relay == nullptr) {
        return false;
    }
    const unsigned char code[]{0xff, 0x25, 0, 0, 0, 0};
    const auto hook = &SceneQuantWithCapRefresh;
    std::memcpy(relay, code, sizeof(code));
    std::memcpy(relay + sizeof(code), &hook, sizeof(hook));
    DWORD oldProtection = 0;
    DWORD jumpProtection = 0;
    if (!VirtualProtect(relay, sizeof(code) + sizeof(hook), PAGE_EXECUTE_READ, &oldProtection) ||
        !FlushInstructionCache(GetCurrentProcess(), relay, sizeof(code) + sizeof(hook)) ||
        !VirtualProtect(jump, 5, PAGE_EXECUTE_READWRITE, &jumpProtection)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    originalSceneQuant = reinterpret_cast<SceneQuant>(image + sceneQuantEntry);
    const auto relative = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(relay) -
                                                    reinterpret_cast<std::intptr_t>(jump + 5));
    std::memcpy(jump + 1, &relative, sizeof(relative));
    FlushInstructionCache(GetCurrentProcess(), jump, 5);
    VirtualProtect(jump, 5, jumpProtection, &oldProtection);
    return true;
}

// Prepare the relay and verify the queue/destructor pair before installing
// guards. Publish host readiness only after the patch is complete.
const char* InitializeTrenchSnap(unsigned char* image, DWORD timestamp, DWORD entityTree, DWORD queueCall,
                                DWORD queueEntry, DWORD orderVtable, DWORD orderDestructor,
                                void (*log)(const char*)) noexcept {
    auto* call = image + queueCall;
    auto** vtable = reinterpret_cast<void**>(image + orderVtable);
    const auto expected = static_cast<std::int32_t>(queueEntry) - static_cast<std::int32_t>(queueCall + 5);
    if (*call != 0xe8 || std::memcmp(call + 1, &expected, sizeof(expected)) != 0 ||
        vtable[0] != image + orderDestructor) {
        return "unexpected order call or destructor";
    }
    auto* relay = AllocateGemRelayNear(call + 5);
    if (relay == nullptr) {
        return "no relay page within rel32 range";
    }

    // Tail-jump to the hook, preserving the native Win64 call arguments.
    const unsigned char code[]{0xff, 0x25, 0, 0, 0, 0};
    const auto hook = &EnqueueTrenchOrder;
    std::memcpy(relay, code, sizeof(code));
    std::memcpy(relay + sizeof(code), &hook, sizeof(hook));
    DWORD oldProtection = 0;
    if (!VirtualProtect(relay, sizeof(code) + sizeof(hook), PAGE_EXECUTE_READ, &oldProtection) ||
        !FlushInstructionCache(GetCurrentProcess(), relay, sizeof(code) + sizeof(hook))) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return "could not prepare order relay";
    }
    DWORD tableProtection = 0;
    if (!VirtualProtect(vtable, sizeof(void*), PAGE_READWRITE, &tableProtection)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return "could not unlock order destructor";
    }
    DWORD callProtection = 0;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &callProtection)) {
        VirtualProtect(vtable, sizeof(void*), tableProtection, &oldProtection);
        VirtualFree(relay, 0, MEM_RELEASE);
        return "could not unlock order call";
    }

    entityTreeRva = entityTree;
    hostTimestamp = timestamp;
    writeCapLog = log;
    distributorVtable = vtable;
    originalEnqueue = reinterpret_cast<Enqueue>(image + queueEntry);
    originalDestroy = reinterpret_cast<DestroyOrder>(image + orderDestructor);
    if (InterlockedCompareExchangePointer(vtable, reinterpret_cast<void*>(&DestroyTrenchOrder),
                                          image + orderDestructor) != image + orderDestructor) {
        VirtualProtect(call, 5, callProtection, &oldProtection);
        VirtualProtect(vtable, sizeof(void*), tableProtection, &oldProtection);
        VirtualFree(relay, 0, MEM_RELEASE);
        return "order destructor changed during setup";
    }

    const auto relative = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(relay) -
                                                    reinterpret_cast<std::intptr_t>(call + 5));
    std::memcpy(call + 1, &relative, sizeof(relative));
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    VirtualProtect(call, 5, callProtection, &oldProtection);
    VirtualProtect(vtable, sizeof(void*), tableProtection, &oldProtection);
    hostImage = image;
    const auto* sceneFailure = InstallGemSceneLifecycle(image, timestamp, {BeforeSceneInit, AfterSceneInit, BeforeSceneDestroy});
    sceneLifecycleInstalled = sceneFailure == nullptr;
    if (sceneLifecycleInstalled) {
        // Initialization has not yet been observed. A frame/root pointer alone
        // cannot create a new epoch or authorize ownership of loaded entities.
        state.SceneInvalidating();
        writeCapLog("[GEM scene] Init/destroy guards installed; awaiting native init.\r\n");
    } else {
        char message[256];
        std::snprintf(message, sizeof(message), "[GEM scene] Lifecycle hooks unavailable: %s; affine backend disabled.\r\n", sceneFailure);
        writeCapLog(message);
    }
    const auto* lifetimeFailure = InstallGemEntityLifetime(image, timestamp, ObserveEntityLifetime);
    entityLifetimeInstalled = lifetimeFailure == nullptr;
    if (entityLifetimeInstalled) {
        writeCapLog("[GEM lifetime] Add/remove generation guard installed; awaiting native init.\r\n");
    } else {
        char message[256];
        std::snprintf(message, sizeof(message), "[GEM lifetime] Generation guard unavailable: %s. Automatic caps disabled.\r\n", lifetimeFailure);
        writeCapLog(message);
    }
    if (!sceneLifecycleInstalled || !entityLifetimeInstalled) SetTrenchCapResourceAvailable(false);
    return nullptr;
}

// Decode the native options byte at the adapter boundary. The model receives
// plain geometry and option values, never pointers into GEM options storage.
bool PrepareTrenchPlacement(const TrenchModule& module, float modelYawOffset, const void* options, TrenchTarget& target, TrenchPlacement& placement) noexcept {
    unsigned char option = 0;
    if (hostImage == nullptr || target.flags != 1 || !Read(options, option)) {
        return false;
    }
    return state.PrepareTrenchPlacement(module, modelYawOffset, option, target, placement);
}
const char* CheckTrenchPlacement(const TrenchPlacement& placement) noexcept {
    return state.CheckTrenchPlacement(placement);
}
unsigned TrackNetworkTrenchPreview(std::uint64_t key, const TrenchModule& module, TrenchVector position, float yaw) {
    if (hostImage == nullptr) {
        return 0;
    }
    return state.TrackNetworkTrenchPreview(key, module, position, yaw);
}
bool RemoveNetworkTrenchPreview(std::uint64_t key) {
    return state.RemoveNetworkTrenchPreview(key);
}
bool EnableNetworkTrenchCaps(bool hooksInstalled) noexcept {
    return state.EnableNetworkTrenchCaps(hooksInstalled && originalSceneQuant != nullptr);
}
void ClearNetworkTrenches() {
    state.ClearNetworkTrenches();
}
unsigned TrackTrenchSection(const TrenchModule& module, const TrenchTransform& transform, const void* section, unsigned& joinedCount, bool replicated, bool networkHost) {
    if (hostImage == nullptr) {
        joinedCount = 0;
        return 0;
    }
    return state.TrackTrenchSection(module, transform, section, joinedCount, replicated, networkHost);
}
