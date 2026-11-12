#include "TrenchState.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace trench {
struct State::Impl {
    explicit Impl(Engine& value) : engine(value) {}
    Engine& engine;
    // Cursor-to-socket search radius in world XY units; height is checked separately.
    static constexpr float snapRadius = 120.0f;
    // Shared cut-overlap allowance at every connected socket, in model units.
    static constexpr float seamOverlap = 10.0f;
    static constexpr float radiansPerDegree = 0.01745329252f;
    static constexpr float snapOutwardOffset = -8.0f;
    struct EndCap {
        Anchor anchor;
        Entity entity;
    };

    struct Polygon {
        // Clipping a convex polygon by one half-plane can add one vertex.
        TrenchPoint vertices[maxTrenchCutVertices + 1]{};
        unsigned size = 0;
    };

    // World-space cut polygon and bounds for a fast rejection before polygon tests.
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
        std::uint64_t registeredAt;
        unsigned heightSamplesRemaining = 0;
        bool replicated = false;
    };

    // Local reservations borrow an order pointer; remote ones use networkKey.
    // accepted controls whether temporary sockets can be used by later orders.
    struct Reservation {
        std::uint64_t owner;
        const void* order;
        TrenchPlacement placement;
        Footprint cut;
        bool accepted;
        std::uint64_t networkKey = 0;
    };

    // Claims both endpoint identities while a connection is pending completion.
    struct Joint {
        std::uint64_t owner, neighbor;
        unsigned side, neighborSide;

        bool Contains(const Anchor& anchor) const noexcept {
            return (owner == anchor.owner && side == anchor.side) ||
                   (neighbor == anchor.owner && neighborSide == anchor.side);
        }
    };

    // One process-local model shared by placement and scene callbacks. The pair
    // (owner, side) identifies a socket; owner is not the engine's entity ID.
    std::mutex anchorMutex;
    // Includes completed sockets and temporary sockets of accepted queued orders.
    std::vector<Anchor> anchors;
    std::vector<Section> sections;
    std::vector<Reservation> reservations;
    std::vector<Joint> joints;
    std::vector<EndCap> endCaps;
    bool capSyncRunning = false;
    bool capSyncRequested = false;
    bool networkCapsEnabled = false;
    bool networkCapsDirty = false;
    std::uint64_t nextNetworkCapCheck = 0;
    std::uint64_t nextOwner = 1;


    TrenchVector Offset(TrenchVector point, TrenchVector direction, float distance) noexcept {
        return {point.x + direction.x * distance, point.y + direction.y * distance,
                point.z + direction.z * distance};
    }

    float DistanceSquared(TrenchVector a, TrenchVector b) noexcept {
        const float x = a.x - b.x;
        const float y = a.y - b.y;
        return x * x + y * y;
    }

    bool IsClaimed(const Anchor& anchor) noexcept {
        return std::any_of(joints.begin(), joints.end(), [&](const Joint& joint) { return joint.Contains(anchor); });
    }

    bool IsFree(const Anchor& anchor) noexcept {
        // A reservation can claim a socket before its new section is built.
        if (IsClaimed(anchor)) {
            return false;
        }
        const auto section = std::find_if(sections.begin(), sections.end(), [&](const Section& entry) {
            return entry.owner == anchor.owner;
        });
        if (section != sections.end()) {
            // Reject stale anchors if the engine entity disappeared or moved.
            return engine.IsAt(section->entity, section->origin);
        }
        // Queued sockets become usable only after the engine accepts the order.
        // Remote orders have a preview key instead of a local order pointer.
        return std::any_of(reservations.begin(), reservations.end(), [&](const Reservation& entry) {
            return entry.owner == anchor.owner && entry.accepted &&
                   (entry.order != nullptr || engine.IsNetworkPreviewLive(entry.networkKey));
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
        return {owner, WorldSocket(transform, socket), outward, side};
    }

    bool SameAnchor(const Anchor& anchor, const TrenchJoint& joint) noexcept {
        return anchor.owner == joint.neighbor && anchor.side == joint.neighborSide &&
               DistanceSquared(anchor.seam, joint.seam) < 1.0f;
    }

    // Transform catalog cut vertices once per pose and cache their XY bounds.
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

    // SAT below expects convex polygons. Use explicit triangles for a non-convex
    // outline; an outline without triangles is treated as a single convex part.
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

    // Separating-axis test: project both polygons onto each edge normal of a.
    // Any non-overlapping interval proves separation; test b's normals as well.
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

    // Clip against one side of the allowed seam band. Only overlap outside
    // that band should block an intended connection to its neighbor.
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

    // Reject disjoint bounding boxes first, then test convex parts. For joined
    // neighbors ignore the permitted overlap strip around the socket seam.
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

    // Revalidate socket availability first, then reject intersections with live
    // sections and queued footprints. The caller holds the state mutex.
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
                (engine.Now() - section.registeredAt <= 1000 || engine.IsAt(section.entity, section.origin))) {
                return "section_overlap";
            }
        }
        for (const auto& reservation : reservations) {
            if (FootprintsOverlap(cut, reservation.cut, FindJoint(placement, reservation.owner)) &&
                (reservation.order != nullptr || engine.IsNetworkPreviewLive(reservation.networkKey))) {
                return "queued_overlap";
            }
        }
        return nullptr;
    }

    template <typename T, typename Predicate>
    void EraseIf(std::vector<T>& entries, Predicate predicate) {
        entries.erase(std::remove_if(entries.begin(), entries.end(), predicate), entries.end());
    }

    // Match topology and pose tolerances separately from height-correction data,
    // so seating an existing cap does not require deleting and respawning it.
    bool SameCapPose(const Anchor& a, const Anchor& b) noexcept {
        return a.owner == b.owner && a.side == b.side &&
               DistanceSquared(a.seam, b.seam) < 0.01f && std::fabs(a.seam.z - b.seam.z) < 0.1f &&
               DistanceSquared(a.outward, b.outward) < 0.0001f;
    }

    bool CapHeightChanged(const Anchor& a, const Anchor& b) noexcept {
        return a.hasBoneOffset != b.hasBoneOffset || std::fabs(a.boneOffset - b.boneOffset) > 0.05f;
    }

    void SyncTrenchCaps() {
        std::unique_lock<std::mutex> lock(anchorMutex);
        // Native spawn/delete may reenter this method. Request another pass
        // instead of recursively mutating the cap list during the current pass.
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
                    (section == sections.end() || engine.Now() - section->registeredAt <= 1000 ||
                     engine.IsAt(section->entity, section->origin));
                if (open) {
                    desired.push_back(anchor);
                }
            }
            // GEM callbacks can reenter the order hooks. Keep the state lock free
            // while the engine mutates the scene, then take a fresh snapshot if needed.
            lock.unlock();
            EraseIf(endCaps, [&](const EndCap& cap) {
                const bool live = engine.IsLive(cap.entity);
                if (cap.anchor.replicated && !live) {
                    engine.LogCap("lost", cap.anchor, cap.entity);
                    return true;
                }
                if (std::any_of(desired.begin(), desired.end(), [&](const Anchor& anchor) {
                        return SameCapPose(anchor, cap.anchor);
                    })) {
                    return false;
                }
                if (live) {
                    engine.DeleteCap(cap.entity, cap.anchor.replicated);
                    engine.LogCap("removed", cap.anchor, cap.entity);
                }
                return true;
            });
            for (const auto& anchor : desired) {
                const auto cap = std::find_if(endCaps.begin(), endCaps.end(), [&](const EndCap& entry) {
                    return SameCapPose(anchor, entry.anchor);
                });
                if (cap == endCaps.end()) {
                    const auto inward = Offset({}, anchor.outward, -1.0f);
                    auto capPosition = anchor.seam;
                    // A queued chain inherits its first socket's Z even across slopes.
                    // Ground only the local preview visual at this endpoint; retain
                    // the reservation pose for snapping and completion matching.
                    if (!anchor.completed && !anchor.replicated) {
                        float ground = 0.0f;
                        if (engine.ReadGroundHeight(capPosition, ground) && std::isfinite(ground)) {
                            capPosition.z = ground;
                        }
                    }
                    const auto entity = engine.Identify(engine.SpawnCap(capPosition, inward, anchor.replicated));
                    if (entity.id != 0xffffffff) {
                        if (anchor.completed) {
                            engine.SeatCap(anchor, entity);
                        }
                        endCaps.push_back({anchor, entity});
                    }
                    engine.LogCap(entity.id != 0xffffffff ? (anchor.completed ? "spawned_completed" : "spawned")
                                                   : "spawn_failed", anchor, entity, true);
                } else if (anchor.completed && (!cap->anchor.completed || CapHeightChanged(anchor, cap->anchor))) {
                    const char* event = cap->anchor.completed ? "height_updated" : "completed";
                    cap->anchor = anchor;
                    if (engine.IsLive(cap->entity)) {
                        engine.SeatCap(anchor, cap->entity);
                        engine.LogCap(event, anchor, cap->entity, true);
                    }
                }
            }
            lock.lock();
        }
        capSyncRunning = false;
    }

    // Snapshot pending samples under the lock, read native bones outside it,
    // then apply successful height updates only to matching owners.
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
            if (!engine.IsAt(section.entity, section.origin) ||
                !engine.ReadTransform(section.entity.pointer, transform) ||
                !engine.ReadCapOffsets(section.entity.pointer, transform, offsets)) {
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

    // Refresh delayed terrain deformation, then service dirty remote caps or
    // their periodic liveness check on the scene-update path.
    void SceneUpdated() {
        RefreshCompletedCapHeights();
        bool sync = false;
        {
            std::lock_guard<std::mutex> lock(anchorMutex);
            const auto now = engine.Now();
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
    }

    void DropAnchors(std::uint64_t owner) {
        EraseIf(anchors, [owner](const Anchor& anchor) { return anchor.owner == owner; });
        EraseIf(joints, [owner](const Joint& joint) { return joint.owner == owner || joint.neighbor == owner; });
    }

    void AddAnchors(std::uint64_t owner, const TrenchModule& module, const TrenchTransform& transform,
                    bool completed = false, const float* boneOffsets = nullptr, bool replicated = false) {
        // Catalog sockets are local to the module; store their world-space poses.
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

    TrenchJoint MakeJoint(const Anchor& neighbor, unsigned side, float axialOffset) noexcept {
        return {neighbor.owner, neighbor.side, side, neighbor.seam, neighbor.outward,
                seamOverlap + (std::max)(0.0f, -axialOffset), seamOverlap};
    }

    void FindOtherJoints(TrenchPlacement& placement, unsigned primarySide,
                         std::uint64_t replicatedOwner = 0) noexcept {
        // The primary snap fixes the pose. Other sockets may also meet existing
        // sections, so detect those connections without moving the module again.
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
                // an axial offset between sockets, and terrain settling can move Z.
                const float minAxial = replicated ? -21.0f : -2.0f * seamOverlap;
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
                placement.joints[placement.jointCount++] = MakeJoint(*best, side, bestAxial);
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

    bool PrepareTrenchPlacement(const TrenchModule& module, float modelYawOffset, unsigned char option, TrenchTarget& target,
                                TrenchPlacement& placement) noexcept {
        const float dx = target.second.x - target.first.x;
        const float dy = target.second.y - target.first.y;
        const float dz = target.second.z - target.first.z;
        if (target.flags != 1 ||
            !std::isfinite(target.first.x) || !std::isfinite(target.first.y) || !std::isfinite(target.first.z) ||
            !std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(anchorMutex);
        // Linear nearest-free-socket search. Compare squared XY distances to
        // avoid a square root for every candidate; this is not a spatial index.
        float bestDistance = snapRadius * snapRadius;
        const Anchor* nearest = nullptr;
        for (const auto& anchor : anchors) {
            const float distance = DistanceSquared(anchor.seam, target.first);
            // Projection onto outward rejects approaches more than 15 units
            // behind the seam. Height tolerance is independent of the XY radius.
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
            // Choose the new module's socket that best faces the existing one,
            // using the drag heading (or the catalog default for a short click).
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
            // Match GEM's 10-degree rotation steps, align the chosen socket,
            // then apply the fixed axial overlap to the module origin.
            inputYaw = QuantizedYaw(intoNeighbor - socket.outwardYawDegrees * radiansPerDegree - yawOffset);
            const auto displacement = WorldSocket(LevelPose({}, inputYaw + yawOffset), socket);
            target.first = Offset(nearest->seam, displacement, -1.0f);
            target.first = Offset(target.first, nearest->outward, snapOutwardOffset);
            target.second = Offset(target.first, {std::cos(inputYaw), std::sin(inputYaw), 0.0f}, 10.0f);
            placement.joints[placement.jointCount++] = MakeJoint(*nearest, side, snapOutwardOffset);
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

    // Mirror live remote previews as reservations. Repeated samples at the same
    // key/module/XY position retain the existing record under current policy.
    unsigned TrackNetworkTrenchPreview(std::uint64_t key, const TrenchModule& module,
                                       TrenchVector position, float yaw) {
        if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(position.z) || !std::isfinite(yaw)) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(anchorMutex);
        networkCapsDirty = true;
        EraseIf(reservations, [this](const Reservation& entry) {
            if (entry.order != nullptr || engine.IsNetworkPreviewLive(entry.networkKey)) {
                return false;
            }
            DropAnchors(entry.owner);
            return true;
        });
        if (!engine.IsNetworkPreviewLive(key) || std::any_of(reservations.begin(), reservations.end(), [&](const Reservation& entry) {
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
        EraseIf(reservations, [this, key](const Reservation& entry) {
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

    bool EnableNetworkTrenchCaps(bool available) noexcept {
        std::lock_guard<std::mutex> lock(anchorMutex);
        networkCapsEnabled = available;
        return networkCapsEnabled;
    }

    // Clear remote ownership only; local orders and sections follow their own
    // lifecycle. Cap removal occurs on subsequent synchronization.
    void ClearNetworkTrenches() {
        std::lock_guard<std::mutex> lock(anchorMutex);
        networkCapsDirty = true;
        EraseIf(reservations, [this](const Reservation& entry) {
            if (entry.order != nullptr) {
                return false;
            }
            DropAnchors(entry.owner);
            return true;
        });
        EraseIf(sections, [this](const Section& section) {
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
        const float length = std::hypot(transform.xAxis.x, transform.xAxis.y);
        const auto identity = engine.Identify(section);
        if (!std::isfinite(length) || length < 0.999f || identity.id == 0xffffffff) {
            return 0;
        }
        float boneOffsets[2]{};
        const bool hasBoneOffsets = useStraightBones && module.socketCount == 2u &&
            engine.ReadCapOffsets(section, transform, boneOffsets);
        const std::uint64_t now = engine.Now();
        std::unique_lock<std::mutex> lock(anchorMutex);
        EraseIf(sections, [this, now](const Section& existing) {
            if (now - existing.registeredAt <= 1000 || engine.IsAt(existing.entity, existing.origin)) {
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
        // Reuse the queued owner's identity so already reserved connections
        // survive the transition from a temporary preview to a real section.
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
        const unsigned pendingSamples = useStraightBones && module.socketCount == 2u
            ? heightSamples : 0;
        sections.push_back({owner, identity, transform.origin, WorldCut(module, transform), now, pendingSamples, replicated});
        // Once both sections exist, their joined sockets are permanently removed
        // from snapping. Built sections currently have no user deletion workflow.
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

    // Remove only reservations still associated with this native order. A
    // completed section has already taken ownership and must remain tracked.
    bool ReleaseOrder(const void* order) {
        bool removed = false;
        {
            std::lock_guard<std::mutex> lock(anchorMutex);
            EraseIf(reservations, [this, order, &removed](const Reservation& entry) {
                if (entry.order != order) {
                    return false;
                }
                DropAnchors(entry.owner);
                removed = true;
                return true;
            });
        }
        return removed;
    }

    std::uint64_t BeginOrder(const void* order, const TrenchPlacement& placement, const char*& blocked) {
        // Recheck under the lock and claim joints before entering the native
        // queue, preventing another order from reserving the same socket.
        const auto cut = WorldCut(*placement.module, placement.transform);
        std::uint64_t owner = 0;
        {
            std::lock_guard<std::mutex> lock(anchorMutex);
            blocked = PlacementBlocker(placement, cut);
            if (blocked == nullptr) {
                owner = nextOwner++;
                reservations.push_back({owner, order, placement, cut, false});
                for (unsigned i = 0; i < placement.jointCount; ++i) {
                    const auto& joint = placement.joints[i];
                    joints.push_back({owner, joint.neighbor, joint.socket, joint.neighborSide});
                }
            }
        }
        return owner;
    }

    void FinishOrder(std::uint64_t owner, const TrenchPlacement& placement, bool accepted) {
        // Native enqueue may have destroyed/completed the order synchronously.
        // Only publish temporary anchors if its reservation still exists.
        {
            std::lock_guard<std::mutex> lock(anchorMutex);
            const auto pending = std::find_if(reservations.begin(), reservations.end(), [owner](const Reservation& entry) {
                return entry.owner == owner;
            });
            if (pending != reservations.end()) {
                if (accepted) {
                    pending->accepted = true;
                    AddAnchors(owner, *placement.module, placement.transform);
                } else {
                    DropAnchors(owner);
                    reservations.erase(pending);
                }
            }
        }
        SyncTrenchCaps();
    }

};
State::State(Engine& engine) : impl(std::make_unique<Impl>(engine)) {}
State::~State() = default;
bool State::PrepareTrenchPlacement(const TrenchModule& module, float modelYawOffset, unsigned char option, TrenchTarget& target, TrenchPlacement& placement) noexcept {
    return impl->PrepareTrenchPlacement(module, modelYawOffset, option, target, placement);
}
const char* State::CheckTrenchPlacement(const TrenchPlacement& placement) noexcept {
    return impl->CheckTrenchPlacement(placement);
}
std::uint64_t State::BeginOrder(const void* order, const TrenchPlacement& placement, const char*& blocked) {
    return impl->BeginOrder(order, placement, blocked);
}
void State::FinishOrder(std::uint64_t owner, const TrenchPlacement& placement, bool accepted) {
    impl->FinishOrder(owner, placement, accepted);
}
bool State::ReleaseOrder(const void* order) {
    return impl->ReleaseOrder(order);
}
void State::SyncTrenchCaps() {
    impl->SyncTrenchCaps();
}
void State::SceneUpdated() {
    impl->SceneUpdated();
}
unsigned State::TrackNetworkTrenchPreview(std::uint64_t key, const TrenchModule& module, TrenchVector position, float yaw) {
    return impl->TrackNetworkTrenchPreview(key, module, position, yaw);
}
bool State::RemoveNetworkTrenchPreview(std::uint64_t key) {
    return impl->RemoveNetworkTrenchPreview(key);
}
bool State::EnableNetworkTrenchCaps(bool available) noexcept {
    return impl->EnableNetworkTrenchCaps(available);
}
void State::ClearNetworkTrenches() {
    impl->ClearNetworkTrenches();
}
unsigned State::TrackTrenchSection(const TrenchModule& module, const TrenchTransform& transform, const void* section, unsigned& joinedCount, bool useStraightBones, bool replicated) {
    return impl->TrackTrenchSection(module, transform, section, joinedCount, useStraightBones, replicated);
}
} // namespace trench
