#include "TrenchState.h"
#include "TrenchCapProfile.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace trench {
namespace {
bool SameEntity(Entity a, Entity b) noexcept {
    return a.pointer == b.pointer && a.id == b.id && a.generation == b.generation;
}
}
struct State::Impl {
    explicit Impl(Engine& value) : engine(value) {}
    Engine& engine;
    // Cursor-to-socket search radius in world XY units; height is checked separately.
    static constexpr float snapRadius = 120.0f;
    // Shared cut-overlap allowance at every connected socket, in model units.
    static constexpr float seamOverlap = 10.0f;
    static constexpr float radiansPerDegree = 0.01745329252f;
    static constexpr float snapOutwardOffset = 0.0f;
    struct EndCap {
        Anchor anchor;
        Entity entity;
        std::uint64_t nextAttempt = 0;
        std::uint64_t nextFailureLog = 0;
        bool deleting = false;
        std::uint64_t nextDeleteAttempt = 0;
        std::uint64_t nextDeleteLog = 0;
        // A non-null Spawn result is not proof that Identify can read its ID.
        // Block new cap spawns until scene reset without dereferencing it again.
        bool identityUnavailable = false;
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
        const TrenchModule* module = nullptr;
        TrenchTransform reference{};
        std::uint64_t nextHeightSample = 0;
        std::uint64_t samplingStarted = 0;
        unsigned stableSamples = 0;
        bool replicated = false;
        bool visualReady = false;
        std::uint64_t nextSpawnAttempt[maxTrenchSockets]{};
        std::uint64_t nextSpawnLog[maxTrenchSockets]{};
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
    bool sceneReady = true;
    std::uint64_t sceneEpoch = 1;
    std::uint64_t desiredRevision = 1;
    bool networkCapsEnabled = false;
    std::uint64_t nextOwner = 1;
    std::uint64_t nextCapCheck = 0;


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

    bool IsFree(const Anchor& anchor, bool ignorePendingClaim = false) noexcept {
        // A reservation can claim a socket before its new section is built.
        if (!ignorePendingClaim && IsClaimed(anchor)) {
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
        Anchor anchor{owner, WorldSocket(transform, socket), outward, side};
        anchor.socketId = module.socketIds[side];
        return anchor;
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

    bool SameSocket(const Anchor& a, const Anchor& b) const noexcept {
        return a.owner == b.owner && a.side == b.side;
    }

    bool CapHeightChanged(const Anchor& a, const Anchor& b) const noexcept {
        return std::fabs(a.seam.z - b.seam.z) > 0.05f;
    }

    void DirtyLocked() noexcept {
        capSyncRequested = true;
        ++desiredRevision;
    }

    bool Current(std::uint64_t epoch, std::uint64_t revision) {
        std::lock_guard<std::mutex> lock(anchorMutex);
        return sceneReady && sceneEpoch == epoch && desiredRevision == revision;
    }

    bool SameScene(std::uint64_t epoch) {
        std::lock_guard<std::mutex> lock(anchorMutex);
        return sceneReady && sceneEpoch == epoch;
    }

    void CheckCapsBy(std::uint64_t deadline) {
        std::lock_guard<std::mutex> lock(anchorMutex);
        nextCapCheck = (std::min)(nextCapCheck, deadline);
    }

    // A stale cutting spawn still owns a terrain patch until native deletion
    // is observed. Keep its handle in the executor rather than forgetting it.
    void RetireCap(std::uint64_t epoch, const Anchor& anchor, Entity entity) {
        std::lock_guard<std::mutex> lock(anchorMutex);
        if (!sceneReady || sceneEpoch != epoch || entity.pointer == nullptr) return;
        for (auto& cap : endCaps) {
            if (SameEntity(cap.entity, entity)) {
                cap.deleting = true;
                DirtyLocked();
                return;
            }
        }
        EndCap cap{anchor, entity};
        cap.deleting = true;
        cap.identityUnavailable = entity.id == 0xffffffff;
        endCaps.push_back(cap);
        DirtyLocked();
    }

    // Native reads are outside the model lock. A failed read preserves the last
    // successful seating and is retried; no finite number of frames exhausts it.
    void RefreshCompletedCapHeights(std::uint64_t epoch) {
        std::vector<Section> pending;
        const auto now = engine.Now();
        {
            std::lock_guard<std::mutex> lock(anchorMutex);
            for (const auto& section : sections) {
                if (now >= section.nextHeightSample) pending.push_back(section);
            }
        }
        for (const auto& sample : pending) {
            if (!SameScene(epoch)) return;
            auto live = engine.QueryEntity(sample.entity);
            if (!SameScene(epoch)) return;
            TrenchTransform pose{};
            const bool havePose = live == Liveness::Alive && engine.ReadTransform(sample.entity.pointer, pose);
            if (!SameScene(epoch)) return;
            if (havePose) live = engine.QueryEntity(sample.entity);
            if (!SameScene(epoch)) return;
            const auto finite = [](TrenchVector p) {
                return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
            };
            const bool validPose = havePose && live == Liveness::Alive && finite(pose.origin) && finite(pose.xAxis) && finite(pose.yAxis) &&
                std::hypot(pose.xAxis.x, pose.xAxis.y) > 0.001f;
            const float yaw = validPose ? std::atan2(pose.xAxis.y, pose.xAxis.x) : 0.0f;
            const float referenceYaw = std::atan2(sample.reference.xAxis.y, sample.reference.xAxis.x);
            const bool moved = validPose && (DistanceSquared(pose.origin, sample.origin) > 1.0f ||
                std::fabs(std::remainder(yaw - referenceYaw, 6.28318530718f)) > 0.5f * radiansPerDegree);
            std::lock_guard<std::mutex> lock(anchorMutex);
            auto section = std::find_if(sections.begin(), sections.end(), [&](const Section& s) {
                return s.owner == sample.owner && SameEntity(s.entity, sample.entity);
            });
            if (section == sections.end()) continue;
            // A new registration may not yet be in the scene tree. It cannot
            // spawn a cap until Alive, but absence during this grace is not removal.
            if (moved || (live == Liveness::Gone && now - sample.registeredAt > 1000)) {
                DropAnchors(sample.owner);
                sections.erase(section);
                DirtyLocked();
                continue;
            }
            bool changed = false;
            const bool visualReady = validPose && !moved;
            if (section->visualReady != visualReady) {
                section->visualReady = visualReady;
                DirtyLocked();
            }
            if (validPose && !moved) {
                for (auto& anchor : anchors) {
                    if (anchor.owner != sample.owner) continue;
                    auto updated = anchor;
                    updated.seam.z = WorldSocket(pose, sample.module->sockets[anchor.side]).z;
                    if (CapHeightChanged(anchor, updated)) {
                        anchor = updated;
                        changed = true;
                    }
                }
            }
            if (changed) {
                section->samplingStarted = now;
                section->stableSamples = 0;
                DirtyLocked();
            } else if (validPose) {
                ++section->stableSamples;
            } else {
                section->stableSamples = 0;
            }
            const bool fast = section->stableSamples < 3 && now - section->samplingStarted < 2000;
            section->nextHeightSample = now + (fast ? 50 : 500);
        }
    }

    // One structural desired-state snapshot. Identity is owner/socket rather than
    // height, so a late terrain update never respawns an otherwise healthy cap.
    void ReconcileCaps(std::uint64_t epoch) {
        struct Desired { Anchor anchor; Entity section; bool visualReady; std::uint64_t nextSpawn; std::uint64_t nextLog; const CapProfile* profile; };
        std::vector<Desired> desired;
        std::vector<EndCap> existing;
        std::uint64_t revision = 0;
        {
            std::lock_guard<std::mutex> lock(anchorMutex);
            if (!sceneReady || sceneEpoch != epoch) return;
            revision = desiredRevision;
            capSyncRequested = false;
            for (const auto& anchor : anchors) {
                if (!anchor.completed || IsClaimed(anchor) || (anchor.localCap && !networkCapsEnabled)) continue;
                const auto section = std::find_if(sections.begin(), sections.end(), [&](const Section& s) { return s.owner == anchor.owner; });
                if (section != sections.end()) desired.push_back({anchor, section->entity, section->visualReady,
                    section->nextSpawnAttempt[anchor.side], section->nextSpawnLog[anchor.side], section->module->capProfiles[anchor.side]});
            }
            existing = endCaps;
        }
        for (const auto& cap : existing) {
            if (!Current(epoch, revision)) return;
            // We cannot prove lifetime or safely delete an unidentified native
            // result. In particular, do not interpret its invalid ID as Gone.
            if (cap.identityUnavailable) continue;
            const auto live = engine.QueryEntity(cap.entity);
            if (!Current(epoch, revision)) return;
            const bool wanted = std::any_of(desired.begin(), desired.end(), [&](const Desired& d) { return SameSocket(d.anchor, cap.anchor); });
            if (!wanted && live == Liveness::Unavailable) {
                std::lock_guard<std::mutex> lock(anchorMutex);
                if (!sceneReady || sceneEpoch != epoch || desiredRevision != revision) return;
                for (auto& entry : endCaps) if (SameEntity(entry.entity, cap.entity))
                    entry.deleting = true;
            }
            if (live == Liveness::Unavailable || (wanted && live == Liveness::Alive && !cap.deleting)) continue;
            if (live == Liveness::Alive) {
                const auto now = engine.Now();
                if (cap.deleting && now < cap.nextDeleteAttempt) {
                    CheckCapsBy(cap.nextDeleteAttempt);
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(anchorMutex);
                    if (!sceneReady || sceneEpoch != epoch) return;
                    for (auto& entry : endCaps) if (SameEntity(entry.entity, cap.entity)) {
                        entry.deleting = true;
                        entry.nextDeleteAttempt = now + 500;
                    }
                }
                engine.DeleteCap(cap.entity, cap.anchor.localCap);
                if (!SameScene(epoch)) return;
                const auto afterDelete = engine.QueryEntity(cap.entity);
                if (!SameScene(epoch)) return;
                if (afterDelete != Liveness::Gone) {
                    bool log = false;
                    {
                        std::lock_guard<std::mutex> lock(anchorMutex);
                        for (auto& entry : endCaps) if (SameEntity(entry.entity, cap.entity)) {
                            if (now >= entry.nextDeleteLog) {
                                entry.nextDeleteLog = now + 5000;
                                log = true;
                            }
                        }
                    }
                    if (log) engine.LogCap("delete_pending", cap.anchor, cap.entity);
                    CheckCapsBy(now + 500);
                    continue;
                }
            }
            engine.LogCap("removed", cap.anchor, cap.entity);
            std::lock_guard<std::mutex> lock(anchorMutex);
            if (sceneEpoch != epoch) return;
            EraseIf(endCaps, [&](const EndCap& e) { return SameEntity(e.entity, cap.entity); });
        }
        for (const auto& wanted : desired) {
            if (!Current(epoch, revision)) return;
            if (engine.QueryEntity(wanted.section) != Liveness::Alive) continue;
            if (!Current(epoch, revision)) return;
            const auto inward = Offset({}, wanted.anchor.outward, -1);
            auto capAnchor = wanted.anchor;
            if (wanted.profile) {
                const auto offset = wanted.profile->capOffset;
                capAnchor.seam.x += inward.x * offset.x - inward.y * offset.y;
                capAnchor.seam.y += inward.y * offset.x + inward.x * offset.y;
            }
            EndCap cap{};
            bool found = false;
            bool blockNewSpawns = false;
            {
                std::lock_guard<std::mutex> lock(anchorMutex);
                const auto it = std::find_if(endCaps.begin(), endCaps.end(), [&](const EndCap& e) { return SameSocket(e.anchor, wanted.anchor); });
                if (it != endCaps.end()) { cap = *it; found = true; }
                blockNewSpawns = std::any_of(endCaps.begin(), endCaps.end(), [](const EndCap& e) { return e.identityUnavailable || e.deleting; });
            }
            if (!found) {
                // Owner IDs can change on network re-registration. A different
                // owner must not bypass a pending cut deletion or an unknown
                // native result merely because SameSocket no longer matches.
                if (blockNewSpawns) continue;
                if (engine.Now() < wanted.nextSpawn) {
                    CheckCapsBy(wanted.nextSpawn);
                    continue;
                }
                auto* pointer = engine.SpawnCap(capAnchor.seam, inward, capAnchor.localCap);
                // Never even identify a pointer returned from a destroyed scene.
                if (!SameScene(epoch)) return;
                const auto entity = pointer != nullptr ? engine.Identify(pointer) : Entity{nullptr, 0xffffffff};
                if (!SameScene(epoch)) return;
                if (!Current(epoch, revision)) {
                    RetireCap(epoch, wanted.anchor, entity);
                    if (entity.pointer != nullptr && entity.id == 0xffffffff)
                        engine.LogCap("spawn_identity_unavailable", wanted.anchor, entity);
                    return;
                }
                if (pointer != nullptr && entity.id == 0xffffffff) {
                    RetireCap(epoch, wanted.anchor, entity);
                    engine.LogCap("spawn_identity_unavailable", wanted.anchor, entity);
                    return;
                }
                if (entity.id == 0xffffffff) {
                    const auto now = engine.Now();
                    {
                        std::lock_guard<std::mutex> lock(anchorMutex);
                        for (auto& section : sections) if (section.owner == wanted.anchor.owner) {
                            section.nextSpawnAttempt[wanted.anchor.side] = now + 500;
                            if (now >= wanted.nextLog) section.nextSpawnLog[wanted.anchor.side] = now + 5000;
                        }
                    }
                    if (now >= wanted.nextLog) engine.LogCap("spawn_failed", wanted.anchor, entity);
                    continue;
                }
                cap = {wanted.anchor, entity};
                cap.anchor.completed = false; // No confirmed SeatCap result yet.
                bool published = false;
                {
                    std::lock_guard<std::mutex> lock(anchorMutex);
                    if (sceneReady && sceneEpoch == epoch && desiredRevision == revision) {
                        endCaps.push_back(cap);
                        published = true;
                    }
                }
                if (!published) {
                    RetireCap(epoch, wanted.anchor, entity);
                    return;
                }
                engine.LogCap("spawned_completed", wanted.anchor, entity);
            }
            if (!Current(epoch, revision)) return;
            if (cap.deleting || cap.identityUnavailable) continue;
            if (cap.anchor.completed && !wanted.visualReady) continue;
            if (cap.anchor.completed && !CapHeightChanged(wanted.anchor, cap.anchor)) continue;
            if (engine.Now() < cap.nextAttempt) {
                CheckCapsBy(cap.nextAttempt);
                continue;
            }
            if (engine.QueryEntity(cap.entity) != Liveness::Alive || !Current(epoch, revision)) continue;
            const bool applied = engine.SeatCap(capAnchor, cap.entity);
            if (!Current(epoch, revision)) return;
            if (engine.QueryEntity(wanted.section) != Liveness::Alive || !Current(epoch, revision)) continue;
            if (engine.QueryEntity(cap.entity) != Liveness::Alive || !Current(epoch, revision)) continue;
            const auto now = engine.Now();
            if (applied) {
                {
                    std::lock_guard<std::mutex> lock(anchorMutex);
                    for (auto& e : endCaps) {
                        if (SameEntity(e.entity, cap.entity)) {
                            e.anchor = capAnchor;
                            e.nextAttempt = 0;
                            e.nextFailureLog = 0;
                        }
                    }
                }
                engine.LogCap(cap.anchor.completed ? "height_updated" : "completed", capAnchor, cap.entity, true);
            } else {
                {
                    std::lock_guard<std::mutex> lock(anchorMutex);
                    for (auto& e : endCaps) if (SameEntity(e.entity, cap.entity)) {
                        e.nextAttempt = now + 500;
                        if (now >= cap.nextFailureLog) e.nextFailureLog = now + 5000;
                    }
                }
                if (now >= cap.nextFailureLog) engine.LogCap("native_apply_failed", capAnchor, cap.entity);
            }
        }
    }

    void SceneUpdated() {
        std::uint64_t epoch = 0;
        {
            std::lock_guard<std::mutex> lock(anchorMutex);
            if (!sceneReady) return;
            if (capSyncRunning) { capSyncRequested = true; return; }
            capSyncRunning = true;
            epoch = sceneEpoch;
        }
        RefreshCompletedCapHeights(epoch);
        bool reconcile = false;
        {
            std::lock_guard<std::mutex> lock(anchorMutex);
            reconcile = capSyncRequested || engine.Now() >= nextCapCheck;
            if (reconcile) nextCapCheck = engine.Now() + 500;
        }
        // Bound callback reentry; remaining work is kept for the next scene tick.
        for (unsigned pass = 0; reconcile && pass < 2 && SameScene(epoch); ++pass) {
            ReconcileCaps(epoch);
            std::lock_guard<std::mutex> lock(anchorMutex);
            if (!capSyncRequested) break;
        }
        std::lock_guard<std::mutex> lock(anchorMutex);
        capSyncRunning = false;
    }

    void SceneInvalidating() noexcept {
        std::lock_guard<std::mutex> lock(anchorMutex);
        sceneReady = false;
        ++sceneEpoch;
        ++desiredRevision;
        anchors.clear(); sections.clear(); reservations.clear(); joints.clear(); endCaps.clear();
        capSyncRequested = false;
        nextCapCheck = 0;
    }

    void SceneReady() noexcept {
        std::lock_guard<std::mutex> lock(anchorMutex);
        sceneReady = true;
        DirtyLocked();
    }

    void DropAnchors(std::uint64_t owner) {
        EraseIf(anchors, [owner](const Anchor& anchor) { return anchor.owner == owner; });
        EraseIf(joints, [owner](const Joint& joint) { return joint.owner == owner || joint.neighbor == owner; });
        DirtyLocked();
    }

    void AddAnchors(std::uint64_t owner, const TrenchModule& module, const TrenchTransform& transform,
                    bool completed = false, bool localCap = false) {
        // Catalog sockets are local to the module; store their world-space poses.
        for (unsigned side = 0; side < module.socketCount; ++side) {
            auto anchor = ModuleAnchor(owner, module, transform, side);
            anchor.completed = completed;
            anchor.localCap = localCap;
            anchors.push_back(anchor);
        }
    }

    TrenchJoint MakeJoint(const Anchor& neighbor, unsigned side, float axialOffset) noexcept {
        return {neighbor.owner, neighbor.side, side, neighbor.seam, neighbor.outward,
                seamOverlap + (std::max)(0.0f, -axialOffset), seamOverlap};
    }

    void FindOtherJoints(TrenchPlacement& placement, unsigned primarySide,
                         std::uint64_t replicatedOwner = 0, bool completingNetwork = false) noexcept {
        // The primary snap fixes the pose. Other sockets may also meet existing
        // sections, so detect those connections without moving the module again.
        const auto& module = *placement.module;
        const bool replicated = replicatedOwner != 0;
        for (unsigned side = 0; side < module.socketCount; ++side) {
            if (side == primarySide) {
                continue;
            }
            const auto candidate = ModuleAnchor(replicatedOwner, module, placement.transform, side);
            const bool claimed = replicated && IsClaimed(candidate);
            if (claimed && !completingNetwork) {
                continue;
            }
            const Anchor* best = nullptr;
            const float searchDistance = replicated ? 22.0f * 22.0f : 100.0f;
            float bestDistance = searchDistance;
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
                // Native terrain settling can leave a 9-unit height step at
                // an otherwise matching second seam when closing a loop.
                const float heightTolerance = replicated ? 40.0f : 12.0f;
                const bool completedPair = completingNetwork && anchor.completed;
                // A matching pair of finished sockets takes precedence over
                // either side's provisional claim, even with a closer preview.
                const bool differentPriority = completingNetwork && best != nullptr && best->completed != anchor.completed;
                const bool better = differentPriority ? completedPair && distance < searchDistance : distance < bestDistance;
                if (better && facing <= -0.9848f && std::fabs(lateral) <= 3.0f &&
                    axial >= minAxial && axial <= maxAxial &&
                    std::fabs(candidate.seam.z - anchor.seam.z) <= heightTolerance &&
                    anchor.owner != replicatedOwner &&
                    FindJoint(placement, anchor.owner) == nullptr &&
                    (!claimed || completedPair) && IsFree(anchor, completedPair)) {
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

    void AddNetworkJoints(std::uint64_t owner, TrenchPlacement& placement, bool completing = false) {
        FindOtherJoints(placement, noTrenchSocket, owner, completing);
        for (unsigned i = 0; i < placement.jointCount; ++i) {
            const auto& joint = placement.joints[i];
            // Preview matching requires free endpoints. Completion can replace
            // stale preview claims at either end of a confirmed pair.
            if (completing) {
                const Anchor socket{owner, {}, {}, joint.socket};
                const Anchor neighbor{joint.neighbor, {}, {}, joint.neighborSide};
                EraseIf(joints, [&](const Joint& pending) {
                    return pending.Contains(socket) || pending.Contains(neighbor);
                });
            }
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
        if (!sceneReady) return false;
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
        if (!sceneReady) return 0;
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
        AddAnchors(owner, module, placement.transform, false, true);
        DirtyLocked();
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
        return removed;
    }

    bool EnableNetworkTrenchCaps(bool available) noexcept {
        std::lock_guard<std::mutex> lock(anchorMutex);
        networkCapsEnabled = available;
        DirtyLocked();
        return networkCapsEnabled;
    }

    // Clear remote ownership only; local orders and sections follow their own
    // lifecycle. Cap removal occurs on subsequent synchronization.
    void ClearNetworkTrenches() {
        std::lock_guard<std::mutex> lock(anchorMutex);
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
                                const void* section, unsigned& joinedCount, bool replicated, bool networkHost) {
        joinedCount = 0;
        std::uint64_t epoch = 0;
        {
            std::lock_guard<std::mutex> lock(anchorMutex);
            if (!sceneReady) return 0;
            epoch = sceneEpoch;
        }
        const float length = std::hypot(transform.xAxis.x, transform.xAxis.y);
        const auto identity = engine.Identify(section);
        if (!std::isfinite(length) || length < 0.999f || identity.id == 0xffffffff) {
            return 0;
        }
        const std::uint64_t now = engine.Now();
        std::unique_lock<std::mutex> lock(anchorMutex);
        if (!sceneReady || sceneEpoch != epoch) return 0;
        if (std::any_of(sections.begin(), sections.end(), [&](const Section& existing) {
                return SameEntity(existing.entity, identity);
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
            // Revalidate links to existing sections against the final model
            // pose. Links to future sections remain provisional.
            EraseIf(joints, [&](const Joint& joint) {
                if (joint.owner != owner && joint.neighbor != owner) return false;
                const auto other = joint.owner == owner ? joint.neighbor : joint.owner;
                return std::any_of(sections.begin(), sections.end(), [other](const Section& existing) {
                    return existing.owner == other;
                });
            });
            TrenchPlacement placement{&module, transform};
            AddNetworkJoints(owner, placement, true);
        }
        EraseIf(anchors, [owner](const Anchor& anchor) { return anchor.owner == owner; });
        AddAnchors(owner, module, transform, true, replicated || networkHost);
        // Read settled heights only from the post-scene executor, for the whole
        // lifetime of the completed section (including models without tip bones).
        sections.push_back({owner, identity, transform.origin, WorldCut(module, transform), now,
            &module, transform, 0, now, 0, replicated});
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
        DirtyLocked();
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
            if (!sceneReady) { blocked = "scene_unavailable"; return 0; }
            blocked = PlacementBlocker(placement, cut);
            if (blocked == nullptr) {
                owner = nextOwner++;
                reservations.push_back({owner, order, placement, cut, false});
                for (unsigned i = 0; i < placement.jointCount; ++i) {
                    const auto& joint = placement.joints[i];
                    joints.push_back({owner, joint.neighbor, joint.socket, joint.neighborSide});
                }
                DirtyLocked();
            }
        }
        return owner;
    }

    void FinishOrder(std::uint64_t owner, const TrenchPlacement& placement, bool accepted) {
        // Native enqueue may have destroyed/completed the order synchronously.
        // Only publish temporary anchors if its reservation still exists.
        std::lock_guard<std::mutex> lock(anchorMutex);
        const auto pending = std::find_if(reservations.begin(), reservations.end(), [owner](const Reservation& entry) {
            return entry.owner == owner;
        });
        if (pending == reservations.end()) return;
        if (accepted) {
            pending->accepted = true;
            AddAnchors(owner, *placement.module, placement.transform);
            DirtyLocked();
        } else {
            DropAnchors(owner);
            reservations.erase(pending);
        }
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
void State::SceneUpdated() {
    impl->SceneUpdated();
}
void State::SceneInvalidating() noexcept {
    impl->SceneInvalidating();
}
void State::SceneReady() noexcept {
    impl->SceneReady();
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
unsigned State::TrackTrenchSection(const TrenchModule& module, const TrenchTransform& transform, const void* section, unsigned& joinedCount, bool replicated, bool networkHost) {
    return impl->TrackTrenchSection(module, transform, section, joinedCount, replicated, networkHost);
}
} // namespace trench
