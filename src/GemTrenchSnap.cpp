#define WIN32_LEAN_AND_MEAN
#include "GemTrenchSnap.h"
#include "GemBranchRelay.h"
#include "TrenchState.h"

using trench::Entity;
using trench::Anchor;

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

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
    return {pointer, id};
}

bool IsLive(Entity entity) noexcept {
    if (entity.pointer == nullptr || entity.id == 0xffffffff) {
        return false;
    }

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
    if (!Read(hostImage + entityTreeRva, head) || !Read(head, node)) {
        return false;
    }
    const Node* current = node.parent;
    for (unsigned depth = 0; current != head && depth < 64; ++depth) {
        if (!Read(current, node) || node.nil) {
            return false;
        }
        if (node.id == entity.id) {
            DWORD flags = 0;
            return node.entity == entity.pointer &&
                   Read(static_cast<const unsigned char*>(entity.pointer) + 0x38, flags) &&
                   (flags & 0x10020) == 0;
        }
        current = node.id > entity.id ? node.left : node.right;
    }
    return false;
}

// An anchor is usable only while its owning entity remains at the recorded
// origin; tolerate sub-unit terrain/rounding differences.
bool IsAt(Entity entity, TrenchVector origin) noexcept {
    TrenchVector currentOrigin{};
    return IsLive(entity) &&
           Read(static_cast<const unsigned char*>(entity.pointer) + 0x74, currentOrigin) &&
           ((currentOrigin.x - origin.x) * (currentOrigin.x - origin.x) +
            (currentOrigin.y - origin.y) * (currentOrigin.y - origin.y)) < 1.0f && std::fabs(currentOrigin.z - origin.z) < 1.0f;
}

bool IsNetworkPreviewLive(std::uint64_t key) noexcept {
    // The GEM visualizer map orders nodes by (local flag, preview key).
    constexpr DWORD treeRva = 0x15a2700;
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
        char message[320];
        const int length = std::snprintf(message, sizeof(message),
                      "[GEM caps] %s pid=%lu anchor=%llu:%u entity=%p id=%08lx client_only=%u "
                      "position=(%.2f,%.2f,%.2f)",
                      event, GetCurrentProcessId(), static_cast<unsigned long long>(anchor.owner), anchor.side,
                      entity.pointer, entity.id, static_cast<unsigned>(anchor.replicated),
                      anchor.seam.x, anchor.seam.y, anchor.seam.z);
        if (length < 0 || static_cast<std::size_t>(length) >= sizeof(message)) {
            return;
        }
        TrenchTransform actual{};
        if (measureHeight && entity.id != 0xffffffff &&
            Read(static_cast<const unsigned char*>(entity.pointer) + 0x50, actual)) {
            const float pitch = std::atan2(actual.xAxis.z, std::hypot(actual.xAxis.x, actual.xAxis.y));
            const char* source = !anchor.completed ? "preview" : anchor.hasBoneOffset ? "bones" : "rim";
            std::snprintf(message + length, sizeof(message) - length,
                          " actual_position=(%.2f,%.2f,%.2f) pitch=%.1f height_offset=%.2f height_source=%s\r\n",
                          actual.origin.x, actual.origin.y, actual.origin.z, pitch / radiansPerDegree,
                          actual.origin.z - anchor.seam.z, source);
        } else {
            std::snprintf(message + length, sizeof(message) - length, "\r\n");
        }
        writeCapLog(message);
    }
}

void SeatCap(const Anchor& anchor, Entity entity) {
    const float* boneOffset = anchor.hasBoneOffset ? &anchor.boneOffset : nullptr;
    SeatCompletedTrenchCap(hostImage, hostTimestamp, entity.pointer, anchor.seam, anchor.outward, boneOffset);
}


// The only bridge from the state model to GEM memory and native functions.
// The model itself owns no executable addresses or native object layouts.
struct GemEngine final : trench::Engine {
    ULONGLONG Now() const override { return GetTickCount64(); }
    Entity Identify(const void* p) const override { return ::Identify(p); }
    bool IsLive(Entity e) const override { return ::IsLive(e); }
    bool IsAt(Entity e, TrenchVector p) const override { return ::IsAt(e, p); }
    bool IsNetworkPreviewLive(std::uint64_t k) const override { return ::IsNetworkPreviewLive(k); }
    bool ReadGroundHeight(TrenchVector p, float& height) const override {
        return ReadTrenchGroundHeight(hostImage, hostTimestamp, p, height);
    }
    void* SpawnCap(TrenchVector p, TrenchVector d, bool local) override { return SpawnTrenchCap(hostImage, hostTimestamp, p, d, local); }
    void DeleteCap(Entity e, bool local) override { DeleteTrenchCap(hostImage, hostTimestamp, e.pointer, local); }
    void SeatCap(const Anchor& a, Entity e) override { ::SeatCap(a, e); }
    void LogCap(const char* event, const Anchor& a, Entity e, bool measure) override { ::LogCap(event, a, e, measure); }
    bool ReadTransform(const void* p, TrenchTransform& pose) override {
        return Read(static_cast<const unsigned char*>(p) + 0x50, pose);
    }
    bool ReadCapOffsets(const void* p, const TrenchTransform& pose, float (&offsets)[2]) override {
        return ReadStraightTrenchCapOffsets(hostImage, hostTimestamp, p, pose, offsets);
    }
};
GemEngine engine;
trench::State state(engine);

// Read settled heights after native scene simulation, preserving its error value.
void SceneQuantWithCapRefresh(void* scene) {
    originalSceneQuant(scene);
    const DWORD lastError = GetLastError();
    state.SceneUpdated();
    SetLastError(lastError);
}

// Detach the reservation before GEM destroys the order; synchronize caps
// after destruction so callbacks observe the updated engine state.
void* DestroyTrenchOrder(void* order, unsigned flags) {
    const DWORD lastError = GetLastError();
    const bool removed = state.ReleaseOrder(order);
    SetLastError(lastError);
    auto* result = originalDestroy(order, flags);
    if (removed) {
        const DWORD destroyError = GetLastError();
        state.SyncTrenchCaps();
        SetLastError(destroyError);
    }
    return result;
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
bool EnableNetworkTrenchCaps() noexcept {
    return state.EnableNetworkTrenchCaps(originalSceneQuant != nullptr);
}
void ClearNetworkTrenches() {
    state.ClearNetworkTrenches();
}
unsigned TrackTrenchSection(const TrenchModule& module, const TrenchTransform& transform, const void* section, unsigned& joinedCount, bool useStraightBones, bool replicated) {
    if (hostImage == nullptr) {
        joinedCount = 0;
        return 0;
    }
    return state.TrackTrenchSection(module, transform, section, joinedCount, useStraightBones, replicated);
}
