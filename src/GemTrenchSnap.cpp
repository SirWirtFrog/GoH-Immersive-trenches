#define WIN32_LEAN_AND_MEAN
#include "GemTrenchSnap.h"
#include "GemBranchRelay.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <locale.h>
#include <mutex>
#include <vector>

static_assert(sizeof(TrenchTarget) == 0xa8);
static_assert(offsetof(TrenchTarget, first) == 4);
static_assert(offsetof(TrenchTarget, second) == 0x10);

thread_local TrenchOrderScope* activeTrenchOrder = nullptr;

namespace {
constexpr float snapRadius = 117.0f;
constexpr float defaultSnapOutwardOffset = -5.0f;
float snapOutwardOffset = defaultSnapOutwardOffset;
constexpr float radiansPerDegree = 0.01745329252f;

struct Entity {
    const void* pointer;
    DWORD id;
};

struct Anchor {
    std::uint64_t owner;
    TrenchVector seam;
    TrenchVector outward;
    unsigned side;
    float overlap;
    bool completed = false;
    bool hasBoneOffset = false;
    float boneOffset = 0.0f;
    bool replicated = false;
};

struct EndCap {
    Anchor anchor;
    Entity entity;
};

struct Polygon {
    TrenchPoint vertices[13]{};
    unsigned size = 0;
};

struct Footprint {
    Polygon outline;
    TrenchPoint min, max;
    const TrenchTriangle* triangles;
    unsigned triangleCount;
};

struct Section {
    std::uint64_t owner;
    Entity entity;
    TrenchVector origin;
    Footprint cut;
    ULONGLONG registeredAt;
    unsigned heightSamplesRemaining = 0;
    bool replicated = false;
};

struct Reservation {
    std::uint64_t owner;
    const void* order;
    TrenchPlacement placement;
    Footprint cut;
    bool accepted;
    std::uint64_t networkKey = 0;
};

struct Joint {
    std::uint64_t owner, neighbor;
    unsigned side, neighborSide;

    bool Contains(const Anchor& anchor) const noexcept {
        return (owner == anchor.owner && side == anchor.side) ||
               (neighbor == anchor.owner && neighborSide == anchor.side);
    }
};

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
std::mutex anchorMutex;
std::vector<Anchor> anchors;
std::vector<Section> sections;
std::vector<Reservation> reservations;
std::vector<Joint> joints;
std::vector<EndCap> endCaps;
bool capSyncRunning = false;
bool capSyncRequested = false;
bool networkCapsEnabled = false;
bool networkCapsDirty = false;
ULONGLONG nextNetworkCapCheck = 0;
std::uint64_t nextOwner = 1;


template <typename T>
bool Read(const void* pointer, T& value) noexcept {
    return ReadProcessMemory(GetCurrentProcess(), pointer, &value, sizeof(value), nullptr) != FALSE;
}

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

TrenchVector Offset(TrenchVector point, TrenchVector direction, float distance) noexcept {
    return {point.x + direction.x * distance, point.y + direction.y * distance,
            point.z + direction.z * distance};
}

float DistanceSquared(TrenchVector a, TrenchVector b) noexcept {
    const float x = a.x - b.x;
    const float y = a.y - b.y;
    return x * x + y * y;
}

bool IsAt(Entity entity, TrenchVector origin) noexcept {
    TrenchVector currentOrigin{};
    return IsLive(entity) &&
           Read(static_cast<const unsigned char*>(entity.pointer) + 0x74, currentOrigin) &&
           DistanceSquared(currentOrigin, origin) < 1.0f && std::fabs(currentOrigin.z - origin.z) < 1.0f;
}

bool IsClaimed(const Anchor& anchor) noexcept {
    return std::any_of(joints.begin(), joints.end(), [&](const Joint& joint) { return joint.Contains(anchor); });
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

bool IsFree(const Anchor& anchor) noexcept {
    if (IsClaimed(anchor)) {
        return false;
    }
    const auto section = std::find_if(sections.begin(), sections.end(), [&](const Section& entry) {
        return entry.owner == anchor.owner;
    });
    if (section != sections.end()) {
        return IsAt(section->entity, section->origin);
    }
    return std::any_of(reservations.begin(), reservations.end(), [&](const Reservation& entry) {
        return entry.owner == anchor.owner && entry.accepted &&
               (entry.order != nullptr || IsNetworkPreviewLive(entry.networkKey));
    });
}

TrenchVector WorldSocket(const TrenchTransform& transform, const TrenchSocket& socket) noexcept {
    return Offset(Offset(transform.origin, transform.xAxis, socket.center.x), transform.yAxis, socket.center.y);
}

TrenchTransform LevelPose(TrenchVector origin, float yaw) noexcept {
    const float x = std::cos(yaw);
    const float y = std::sin(yaw);
    return {{x, y, 0}, {-y, x, 0}, {0, 0, 1}, origin};
}

float QuantizedYaw(float radians) noexcept {
    const float step = 10.0f * radiansPerDegree;
    return std::round(radians / step) * step;
}

Anchor ModuleAnchor(std::uint64_t owner, const TrenchModule& module, const TrenchTransform& transform,
                    unsigned side) noexcept {
    const auto& socket = module.sockets[side];
    const float yaw = std::atan2(transform.xAxis.y, transform.xAxis.x);
    const auto outward = LevelPose({}, yaw + socket.outwardYawDegrees * radiansPerDegree).xAxis;
    return {owner, WorldSocket(transform, socket), outward, side, socket.overlap};
}

bool SameAnchor(const Anchor& anchor, const TrenchJoint& joint) noexcept {
    return anchor.owner == joint.neighbor && anchor.side == joint.neighborSide &&
           DistanceSquared(anchor.seam, joint.seam) < 1.0f;
}

Footprint WorldCut(const TrenchModule& module, const TrenchTransform& transform) noexcept {
    Footprint cut{};
    cut.outline.size = module.cutSize;
    cut.triangles = module.triangles;
    cut.triangleCount = module.triangleCount;
    for (unsigned i = 0; i < cut.outline.size; ++i) {
        const TrenchPoint p = module.cut[i];
        const auto world = Offset(Offset(transform.origin, transform.xAxis, p.x), transform.yAxis, p.y);
        cut.outline.vertices[i] = {world.x, world.y};
        if (i == 0) {
            cut.min = cut.max = cut.outline.vertices[i];
        } else {
            cut.min = {(std::min)(cut.min.x, world.x), (std::min)(cut.min.y, world.y)};
            cut.max = {(std::max)(cut.max.x, world.x), (std::max)(cut.max.y, world.y)};
        }
    }
    return cut;
}

Polygon ConvexPart(const Footprint& footprint, unsigned part) noexcept {
    if (footprint.triangleCount == 0) {
        return footprint.outline;
    }
    const auto triangle = footprint.triangles[part];
    const auto& vertices = footprint.outline.vertices;
    return {{vertices[triangle.a], vertices[triangle.b], vertices[triangle.c]}, 3};
}

float Project(TrenchPoint point, TrenchPoint axis) noexcept {
    return point.x * axis.x + point.y * axis.y;
}

bool SeparatedByEdge(const Polygon& a, const Polygon& b) noexcept {
    for (unsigned i = 0; i < a.size; ++i) {
        const auto start = a.vertices[i];
        const auto end = a.vertices[(i + 1) % a.size];
        const TrenchPoint normal{start.y - end.y, end.x - start.x};
        const float length = std::hypot(normal.x, normal.y);
        if (length < 0.001f) {
            continue;
        }
        float minA = Project(a.vertices[0], normal);
        float maxA = minA;
        float minB = Project(b.vertices[0], normal);
        float maxB = minB;
        for (unsigned j = 1; j < a.size; ++j) {
            const float projection = Project(a.vertices[j], normal);
            minA = (std::min)(minA, projection);
            maxA = (std::max)(maxA, projection);
        }
        for (unsigned j = 1; j < b.size; ++j) {
            const float projection = Project(b.vertices[j], normal);
            minB = (std::min)(minB, projection);
            maxB = (std::max)(maxB, projection);
        }
        // Shared edges and a small model-rounding tolerance are allowed.
        const float tolerance = 0.1f * length;
        if (maxA <= minB + tolerance || maxB <= minA + tolerance) {
            return true;
        }
    }
    return false;
}

bool Overlaps(const Polygon& a, const Polygon& b) noexcept {
    return a.size >= 3 && b.size >= 3 && !SeparatedByEdge(a, b) && !SeparatedByEdge(b, a);
}

Polygon OutsideSeam(const Polygon& cut, const TrenchJoint& placement, float sign) noexcept {
    Polygon clipped;
    const float allowance = sign < 0.0f ? placement.overlapBehind : placement.overlapAhead;
    const auto depth = [&](TrenchPoint p) {
        return sign * ((p.x - placement.seam.x) * placement.outward.x +
                       (p.y - placement.seam.y) * placement.outward.y) - allowance;
    };
    for (unsigned i = 0; i < cut.size; ++i) {
        const auto a = cut.vertices[i];
        const auto b = cut.vertices[(i + 1) % cut.size];
        const float da = depth(a);
        const float db = depth(b);
        if (da >= 0.0f) {
            clipped.vertices[clipped.size++] = a;
        }
        if ((da < 0.0f && db > 0.0f) || (da > 0.0f && db < 0.0f)) {
            const float t = da / (da - db);
            clipped.vertices[clipped.size++] = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
        }
    }
    return clipped;
}

bool FootprintsOverlap(const Footprint& a, const Footprint& b, const TrenchJoint* seam = nullptr) noexcept {
    if (a.max.x <= b.min.x || b.max.x <= a.min.x || a.max.y <= b.min.y || b.max.y <= a.min.y) {
        return false;
    }
    const auto aCount = (std::max)(1u, a.triangleCount);
    const auto bCount = (std::max)(1u, b.triangleCount);
    for (unsigned i = 0; i < aCount; ++i) {
        const auto part = ConvexPart(a, i);
        const auto behind = seam != nullptr ? OutsideSeam(part, *seam, -1.0f) : part;
        const auto ahead = seam != nullptr ? OutsideSeam(part, *seam, 1.0f) : Polygon{};
        for (unsigned j = 0; j < bCount; ++j) {
            const auto other = ConvexPart(b, j);
            if (Overlaps(behind, other) || Overlaps(ahead, other)) {
                return true;
            }
        }
    }
    return false;
}

const TrenchJoint* FindJoint(const TrenchPlacement& placement, std::uint64_t owner) noexcept {
    for (unsigned i = 0; i < placement.jointCount; ++i) {
        if (placement.joints[i].neighbor == owner) {
            return &placement.joints[i];
        }
    }
    return nullptr;
}

const char* PlacementBlocker(const TrenchPlacement& placement, const Footprint& cut) noexcept {
    for (unsigned i = 0; i < placement.jointCount; ++i) {
        if (std::none_of(anchors.begin(), anchors.end(), [&](const Anchor& anchor) {
                return SameAnchor(anchor, placement.joints[i]) && IsFree(anchor);
            })) {
            return "anchor_unavailable";
        }
    }

    for (const auto& section : sections) {
        if (FootprintsOverlap(cut, section.cut, FindJoint(placement, section.owner)) &&
            (GetTickCount64() - section.registeredAt <= 1000 || IsAt(section.entity, section.origin))) {
            return "section_overlap";
        }
    }
    for (const auto& reservation : reservations) {
        if (FootprintsOverlap(cut, reservation.cut, FindJoint(placement, reservation.owner)) &&
            (reservation.order != nullptr || IsNetworkPreviewLive(reservation.networkKey))) {
            return "queued_overlap";
        }
    }
    return nullptr;
}

template <typename T, typename Predicate>
void EraseIf(std::vector<T>& entries, Predicate predicate) {
    entries.erase(std::remove_if(entries.begin(), entries.end(), predicate), entries.end());
}

bool SameCapPose(const Anchor& a, const Anchor& b) noexcept {
    return a.owner == b.owner && a.side == b.side &&
           DistanceSquared(a.seam, b.seam) < 0.01f && std::fabs(a.seam.z - b.seam.z) < 0.1f &&
           DistanceSquared(a.outward, b.outward) < 0.0001f;
}

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

bool CapHeightChanged(const Anchor& a, const Anchor& b) noexcept {
    return a.hasBoneOffset != b.hasBoneOffset || std::fabs(a.boneOffset - b.boneOffset) > 0.05f;
}

void SyncTrenchCaps() {
    std::unique_lock<std::mutex> lock(anchorMutex);
    capSyncRequested = true;
    if (capSyncRunning) {
        return;
    }
    capSyncRunning = true;
    while (capSyncRequested) {
        capSyncRequested = false;
        std::vector<Anchor> desired;
        for (const auto& anchor : anchors) {
            if (anchor.replicated && !networkCapsEnabled) {
                continue;
            }
            // Pending anchors only exist after acceptance. Completed sections
            // may spend their first tick waiting for insertion in the entity tree.
            const auto section = std::find_if(sections.begin(), sections.end(), [&](const Section& entry) {
                return entry.owner == anchor.owner;
            });
            const bool open = anchor.replicated ? IsFree(anchor) : !IsClaimed(anchor) &&
                (section == sections.end() || GetTickCount64() - section->registeredAt <= 1000 ||
                 IsAt(section->entity, section->origin));
            if (open) {
                desired.push_back(anchor);
            }
        }
        // GEM callbacks can reenter the order hooks. Keep the state lock free
        // while the engine mutates the scene, then take a fresh snapshot if needed.
        lock.unlock();
        EraseIf(endCaps, [&](const EndCap& cap) {
            const bool live = IsLive(cap.entity);
            if (cap.anchor.replicated && !live) {
                LogCap("lost", cap.anchor, cap.entity);
                return true;
            }
            if (std::any_of(desired.begin(), desired.end(), [&](const Anchor& anchor) {
                    return SameCapPose(anchor, cap.anchor);
                })) {
                return false;
            }
            if (live) {
                DeleteTrenchCap(hostImage, hostTimestamp, cap.entity.pointer, cap.anchor.replicated);
                LogCap("removed", cap.anchor, cap.entity);
            }
            return true;
        });
        for (const auto& anchor : desired) {
            const auto cap = std::find_if(endCaps.begin(), endCaps.end(), [&](const EndCap& entry) {
                return SameCapPose(anchor, entry.anchor);
            });
            if (cap == endCaps.end()) {
                const auto inward = Offset({}, anchor.outward, -1.0f);
                const auto entity = Identify(SpawnTrenchCap(hostImage, hostTimestamp, anchor.seam, inward,
                                                            anchor.replicated));
                if (entity.id != 0xffffffff) {
                    if (anchor.completed) {
                        SeatCap(anchor, entity);
                    }
                    endCaps.push_back({anchor, entity});
                }
                LogCap(entity.id != 0xffffffff ? (anchor.completed ? "spawned_completed" : "spawned")
                                               : "spawn_failed", anchor, entity, true);
            } else if (anchor.completed && (!cap->anchor.completed || CapHeightChanged(anchor, cap->anchor))) {
                const char* event = cap->anchor.completed ? "height_updated" : "completed";
                cap->anchor = anchor;
                if (IsLive(cap->entity)) {
                    SeatCap(anchor, cap->entity);
                    LogCap(event, anchor, cap->entity, true);
                }
            }
        }
        lock.lock();
    }
    capSyncRunning = false;
}

void RefreshCompletedCapHeights() {
    std::vector<Section> pending;
    {
        std::lock_guard<std::mutex> lock(anchorMutex);
        for (auto& section : sections) {
            if (section.heightSamplesRemaining != 0) {
                --section.heightSamplesRemaining;
                pending.push_back(section);
            }
        }
    }
    bool changed = false;
    for (const auto& section : pending) {
        TrenchTransform transform{};
        float offsets[2]{};
        if (!IsAt(section.entity, section.origin) ||
            !Read(static_cast<const unsigned char*>(section.entity.pointer) + 0x50, transform) ||
            !ReadStraightTrenchCapOffsets(hostImage, hostTimestamp, section.entity.pointer, transform, offsets)) {
            continue;
        }
        std::lock_guard<std::mutex> lock(anchorMutex);
        for (auto& anchor : anchors) {
            if (anchor.owner != section.owner) {
                continue;
            }
            auto updated = anchor;
            updated.hasBoneOffset = true;
            updated.boneOffset = offsets[anchor.side];
            if (CapHeightChanged(anchor, updated)) {
                anchor = updated;
                networkCapsDirty |= anchor.replicated;
                changed |= !anchor.replicated;
            }
        }
    }
    if (changed) {
        SyncTrenchCaps();
    }
}

void SceneQuantWithCapRefresh(void* scene) {
    originalSceneQuant(scene);
    const DWORD lastError = GetLastError();
    RefreshCompletedCapHeights();
    bool sync = false;
    {
        std::lock_guard<std::mutex> lock(anchorMutex);
        const auto now = GetTickCount64();
        sync = networkCapsEnabled && (networkCapsDirty ||
            (now >= nextNetworkCapCheck && std::any_of(anchors.begin(), anchors.end(),
                [](const Anchor& anchor) { return anchor.replicated; })));
        if (sync) {
            networkCapsDirty = false;
            nextNetworkCapCheck = now + 500;
        }
    }
    if (sync) {
        SyncTrenchCaps();
    }
    SetLastError(lastError);
}

void DropAnchors(std::uint64_t owner) {
    EraseIf(anchors, [owner](const Anchor& anchor) { return anchor.owner == owner; });
    EraseIf(joints, [owner](const Joint& joint) { return joint.owner == owner || joint.neighbor == owner; });
}

void AddAnchors(std::uint64_t owner, const TrenchModule& module, const TrenchTransform& transform,
                bool completed = false, const float* boneOffsets = nullptr, bool replicated = false) {
    for (unsigned side = 0; side < module.socketCount; ++side) {
        auto anchor = ModuleAnchor(owner, module, transform, side);
        anchor.completed = completed;
        anchor.replicated = replicated;
        if (boneOffsets != nullptr) {
            anchor.hasBoneOffset = true;
            anchor.boneOffset = boneOffsets[side];
        }
        anchors.push_back(anchor);
    }
}

TrenchJoint MakeJoint(const Anchor& neighbor, unsigned side, float overlap, float axialOffset) noexcept {
    return {neighbor.owner, neighbor.side, side, neighbor.seam, neighbor.outward,
            overlap + (std::max)(0.0f, -axialOffset), neighbor.overlap};
}

void FindOtherJoints(TrenchPlacement& placement, unsigned primarySide,
                     std::uint64_t replicatedOwner = 0) noexcept {
    const auto& module = *placement.module;
    const bool replicated = replicatedOwner != 0;
    for (unsigned side = 0; side < module.socketCount; ++side) {
        if (side == primarySide) {
            continue;
        }
        const auto candidate = ModuleAnchor(replicatedOwner, module, placement.transform, side);
        if (replicated && IsClaimed(candidate)) {
            continue;
        }
        const Anchor* best = nullptr;
        float bestDistance = replicated ? 22.0f * 22.0f : 100.0f;
        float bestAxial = 0.0f;
        for (const auto& anchor : anchors) {
            const float dx = candidate.seam.x - anchor.seam.x;
            const float dy = candidate.seam.y - anchor.seam.y;
            const float axial = dx * anchor.outward.x + dy * anchor.outward.y;
            const float lateral = dx * anchor.outward.y - dy * anchor.outward.x;
            const float facing = candidate.outward.x * anchor.outward.x + candidate.outward.y * anchor.outward.y;
            const float distance = dx * dx + dy * dy;
            // Remote clients infer joints from the final pose. The host can use
            // any configured axial offset, and terrain settling can move Z.
            const float minAxial = replicated ? -21.0f : -candidate.overlap - anchor.overlap;
            const float maxAxial = replicated ? 21.0f : 3.0f;
            const float heightTolerance = replicated ? 40.0f : 8.0f;
            if (distance < bestDistance && facing <= -0.9848f && std::fabs(lateral) <= 3.0f &&
                axial >= minAxial && axial <= maxAxial &&
                std::fabs(candidate.seam.z - anchor.seam.z) <= heightTolerance &&
                anchor.owner != replicatedOwner &&
                FindJoint(placement, anchor.owner) == nullptr && IsFree(anchor)) {
                best = &anchor;
                bestDistance = distance;
                bestAxial = axial;
            }
        }
        if (best != nullptr) {
            placement.joints[placement.jointCount++] = MakeJoint(*best, side, candidate.overlap, bestAxial);
        }
    }
}

void AddNetworkJoints(std::uint64_t owner, TrenchPlacement& placement) {
    FindOtherJoints(placement, noTrenchSocket, owner);
    for (unsigned i = 0; i < placement.jointCount; ++i) {
        const auto& joint = placement.joints[i];
        joints.push_back({owner, joint.neighbor, joint.socket, joint.neighborSide});
    }
}

void* DestroyTrenchOrder(void* order, unsigned flags) {
    const DWORD lastError = GetLastError();
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(anchorMutex);
        EraseIf(reservations, [order, &removed](const Reservation& entry) {
            if (entry.order != order) {
                return false;
            }
            DropAnchors(entry.owner);
            removed = true;
            return true;
        });
    }
    SetLastError(lastError);
    auto* result = originalDestroy(order, flags);
    if (removed) {
        const DWORD destroyError = GetLastError();
        SyncTrenchCaps();
        SetLastError(destroyError);
    }
    return result;
}

void* EnqueueTrenchOrder(void* actor, void* order, const void* options) {
    auto* scope = activeTrenchOrder;
    if (scope == nullptr || order == nullptr || *static_cast<void***>(order) != distributorVtable ||
        *reinterpret_cast<const void* const*>(static_cast<const unsigned char*>(order) + 0x390) != scope->action) {
        return originalEnqueue(actor, order, options);
    }

    const DWORD lastError = GetLastError();
    const auto& placement = scope->placement;
    const auto cut = WorldCut(*placement.module, placement.transform);
    std::uint64_t owner = 0;
    {
        std::lock_guard<std::mutex> lock(anchorMutex);
        scope->blocked = PlacementBlocker(placement, cut);
        if (scope->blocked == nullptr) {
            owner = nextOwner++;
            reservations.push_back({owner, order, placement, cut, false});
            for (unsigned i = 0; i < placement.jointCount; ++i) {
                const auto& joint = placement.joints[i];
                joints.push_back({owner, joint.neighbor, joint.socket, joint.neighborSide});
            }
        }
    }
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
    {
        std::lock_guard<std::mutex> lock(anchorMutex);
        const auto pending = std::find_if(reservations.begin(), reservations.end(), [owner](const Reservation& entry) {
            return entry.owner == owner;
        });
        if (pending != reservations.end()) {
            if (accepted != nullptr) {
                pending->accepted = true;
                AddAnchors(owner, *placement.module, placement.transform);
            } else {
                DropAnchors(owner);
                reservations.erase(pending);
            }
        }
    }
    SyncTrenchCaps();
    SetLastError(enqueueError);
    scope->reserved |= accepted != nullptr;
    return accepted;
}

float ReadSnapOutwardOffset(HMODULE module) noexcept {
    constexpr wchar_t filename[] = L"ImmersiveTrenchesMod.ini";
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, path, ARRAYSIZE(path));
    if (length == 0 || length >= ARRAYSIZE(path)) {
        return defaultSnapOutwardOffset;
    }
    wchar_t* slash = std::wcsrchr(path, L'\\');
    if (slash == nullptr || slash + 1 - path > ARRAYSIZE(path) - ARRAYSIZE(filename)) {
        return defaultSnapOutwardOffset;
    }
    CopyMemory(slash + 1, filename, sizeof(filename));

    wchar_t value[64];
    const DWORD copied = GetPrivateProfileStringW(L"TrenchSnap", L"OutwardOffset", L"-5",
                                                   value, ARRAYSIZE(value), path);
    if (copied == 0 || copied >= ARRAYSIZE(value) - 1) {
        return defaultSnapOutwardOffset;
    }
    const _locale_t numericLocale = _create_locale(LC_NUMERIC, "C");
    if (numericLocale == nullptr) {
        return defaultSnapOutwardOffset;
    }
    wchar_t* end = nullptr;
    const float offset = _wcstof_l(value, &end, numericLocale);
    _free_locale(numericLocale);
    return end != value && *end == L'\0' && std::isfinite(offset) &&
           offset >= -20.0f && offset <= 20.0f ? offset : defaultSnapOutwardOffset;
}

} // namespace

void LoadTrenchSnapSettings(HMODULE module) noexcept {
    snapOutwardOffset = ReadSnapOutwardOffset(module);
}

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

bool PrepareTrenchPlacement(const TrenchModule& module, float modelYawOffset, const void* options, TrenchTarget& target,
                            TrenchPlacement& placement) noexcept {
    unsigned char option = 0;
    const float dx = target.second.x - target.first.x;
    const float dy = target.second.y - target.first.y;
    const float dz = target.second.z - target.first.z;
    if (hostImage == nullptr || target.flags != 1 || !Read(options, option) ||
        !std::isfinite(target.first.x) || !std::isfinite(target.first.y) || !std::isfinite(target.first.z) ||
        !std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(anchorMutex);
    float bestDistance = snapRadius * snapRadius;
    const Anchor* nearest = nullptr;
    for (const auto& anchor : anchors) {
        const float distance = DistanceSquared(anchor.seam, target.first);
        const float ahead = (target.first.x - anchor.seam.x) * anchor.outward.x +
                            (target.first.y - anchor.seam.y) * anchor.outward.y;
        if (module.socketCount != 0 && distance < bestDistance && ahead >= -15.0f &&
            std::fabs(anchor.seam.z - target.first.z) <= 40.0f &&
            IsFree(anchor)) {
            nearest = &anchor;
            bestDistance = distance;
        }
    }

    const float yawOffset = (modelYawOffset + ((option & 0x20) ? 90.0f : 0.0f)) * radiansPerDegree;
    float inputYaw = std::atan2(dy, dx);
    if (nearest != nullptr) {
        const float intoNeighbor = std::atan2(-nearest->outward.y, -nearest->outward.x);
        unsigned side = module.defaultSocket;
        if (dx * dx + dy * dy >= 1.0f) {
            float bestAlignment = -2.0f;
            for (unsigned i = 0; i < module.socketCount; ++i) {
                const float outward = inputYaw + yawOffset + module.sockets[i].outwardYawDegrees * radiansPerDegree;
                const float alignment = std::cos(outward - intoNeighbor);
                if (alignment > bestAlignment) {
                    bestAlignment = alignment;
                    side = i;
                }
            }
        }
        const auto& socket = module.sockets[side];
        inputYaw = QuantizedYaw(intoNeighbor - socket.outwardYawDegrees * radiansPerDegree - yawOffset);
        const auto displacement = WorldSocket(LevelPose({}, inputYaw + yawOffset), socket);
        target.first = Offset(nearest->seam, displacement, -1.0f);
        target.first = Offset(target.first, nearest->outward, snapOutwardOffset);
        target.second = Offset(target.first, {std::cos(inputYaw), std::sin(inputYaw), 0.0f}, 10.0f);
        placement.joints[placement.jointCount++] = MakeJoint(*nearest, side, socket.overlap, snapOutwardOffset);
    } else {
        // These buttons place one module. Keep the drag heading but cap its length
        // so eFortify cannot create a line of extra positions with multiple builders.
        const float dragLength = std::hypot(std::hypot(dx, dy), dz);
        constexpr float maxDrag = 40.0f;
        if (dragLength > maxDrag) {
            target.second = Offset(target.first, {dx, dy, dz}, maxDrag / dragLength);
        }
    }

    placement.module = &module;
    placement.transform = LevelPose(target.first, QuantizedYaw(inputYaw) + yawOffset);
    if (placement.jointCount != 0) {
        FindOtherJoints(placement, placement.joints[0].socket);
    }
    return true;
}

const char* CheckTrenchPlacement(const TrenchPlacement& placement) noexcept {
    const auto cut = WorldCut(*placement.module, placement.transform);
    std::lock_guard<std::mutex> lock(anchorMutex);
    return PlacementBlocker(placement, cut);
}

unsigned TrackNetworkTrenchPreview(std::uint64_t key, const TrenchModule& module,
                                   TrenchVector position, float yaw) {
    if (hostImage == nullptr || !std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) || !std::isfinite(yaw)) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(anchorMutex);
    networkCapsDirty = true;
    EraseIf(reservations, [](const Reservation& entry) {
        if (entry.order != nullptr || IsNetworkPreviewLive(entry.networkKey)) {
            return false;
        }
        DropAnchors(entry.owner);
        return true;
    });
    if (!IsNetworkPreviewLive(key) || std::any_of(reservations.begin(), reservations.end(), [&](const Reservation& entry) {
            return entry.order == nullptr && entry.networkKey == key && entry.placement.module == &module &&
                   DistanceSquared(entry.placement.transform.origin, position) < 1.0f;
        })) {
        return 0;
    }
    const auto owner = nextOwner++;
    TrenchPlacement placement{&module, LevelPose(position, yaw)};
    AddNetworkJoints(owner, placement);
    reservations.push_back({owner, nullptr, placement, WorldCut(module, placement.transform), true, key});
    AddAnchors(owner, module, placement.transform, false, nullptr, true);
    return module.socketCount - placement.jointCount;
}

bool RemoveNetworkTrenchPreview(std::uint64_t key) {
    std::lock_guard<std::mutex> lock(anchorMutex);
    const auto before = reservations.size();
    EraseIf(reservations, [key](const Reservation& entry) {
        if (entry.order != nullptr || entry.networkKey != key) {
            return false;
        }
        DropAnchors(entry.owner);
        return true;
    });
    const bool removed = reservations.size() != before;
    networkCapsDirty |= removed;
    return removed;
}

bool EnableNetworkTrenchCaps() noexcept {
    std::lock_guard<std::mutex> lock(anchorMutex);
    networkCapsEnabled = originalSceneQuant != nullptr;
    return networkCapsEnabled;
}

void ClearNetworkTrenches() {
    std::lock_guard<std::mutex> lock(anchorMutex);
    networkCapsDirty = true;
    EraseIf(reservations, [](const Reservation& entry) {
        if (entry.order != nullptr) {
            return false;
        }
        DropAnchors(entry.owner);
        return true;
    });
    EraseIf(sections, [](const Section& section) {
        if (!section.replicated) {
            return false;
        }
        DropAnchors(section.owner);
        return true;
    });
}

unsigned TrackTrenchSection(const TrenchModule& module, const TrenchTransform& transform,
                            const void* section, unsigned& joinedCount, bool useStraightBones,
                            bool replicated) {
    joinedCount = 0;
    if (hostImage == nullptr) {
        return 0;
    }
    const float length = std::hypot(transform.xAxis.x, transform.xAxis.y);
    const auto identity = Identify(section);
    if (!std::isfinite(length) || length < 0.999f || identity.id == 0xffffffff) {
        return 0;
    }
    float boneOffsets[2]{};
    const bool hasBoneOffsets = useStraightBones && module.socketCount == ARRAYSIZE(boneOffsets) &&
        ReadStraightTrenchCapOffsets(hostImage, hostTimestamp, section, transform, boneOffsets);
    const ULONGLONG now = GetTickCount64();
    std::unique_lock<std::mutex> lock(anchorMutex);
    EraseIf(sections, [now](const Section& existing) {
        if (now - existing.registeredAt <= 1000 || IsAt(existing.entity, existing.origin)) {
            return false;
        }
        DropAnchors(existing.owner);
        return true;
    });
    if (std::any_of(sections.begin(), sections.end(), [&](const Section& existing) {
            return existing.entity.pointer == identity.pointer && existing.entity.id == identity.id;
        })) {
        return 0;
    }

    const auto reservation = std::find_if(reservations.begin(), reservations.end(), [&](const Reservation& entry) {
        const auto& pose = entry.placement.transform;
        return (entry.order == nullptr) == replicated && entry.placement.module == &module &&
               DistanceSquared(pose.origin, transform.origin) < 1.0f &&
               pose.xAxis.x * transform.xAxis.x + pose.xAxis.y * transform.xAxis.y > 0.999f;
    });
    const auto owner = reservation != reservations.end() ? reservation->owner : nextOwner++;
    if (reservation != reservations.end()) {
        reservations.erase(reservation);
    }
    if (replicated) {
        TrenchPlacement placement{&module, transform};
        AddNetworkJoints(owner, placement);
    }
    EraseIf(anchors, [owner](const Anchor& anchor) { return anchor.owner == owner; });
    AddAnchors(owner, module, transform, true, hasBoneOffsets ? boneOffsets : nullptr, replicated);
    // Terrain alignment may arrive in a later scene update than install's callback.
    constexpr unsigned heightSamples = 8;
    const unsigned pendingSamples = useStraightBones && module.socketCount == ARRAYSIZE(boneOffsets)
        ? heightSamples : 0;
    sections.push_back({owner, identity, transform.origin, WorldCut(module, transform), now, pendingSamples, replicated});
    EraseIf(joints, [&](const Joint& joint) {
        if (joint.owner != owner && joint.neighbor != owner) {
            return false;
        }
        const auto other = joint.owner == owner ? joint.neighbor : joint.owner;
        if (std::none_of(sections.begin(), sections.end(), [other](const Section& entry) { return entry.owner == other; })) {
            return false;
        }
        EraseIf(anchors, [&](const Anchor& anchor) { return joint.Contains(anchor); });
        ++joinedCount;
        return true;
    });
    const auto tracked = static_cast<unsigned>(std::count_if(anchors.begin(), anchors.end(), [owner](const Anchor& anchor) {
        return anchor.owner == owner;
    }));
    networkCapsDirty |= replicated;
    lock.unlock();
    if (!replicated) {
        SyncTrenchCaps();
    }
    return tracked;
}
