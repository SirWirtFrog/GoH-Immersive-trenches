#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstddef>
#include <cmath>
#include <algorithm>

#include "GemTrenchSnap.h"

namespace {
struct NativeString {
    char text[16];
    size_t length;
    size_t capacity;
};

struct SpawnLayout {
    DWORD timestamp;
    DWORD boneNameConstructor;
    DWORD nameConstructor;
    DWORD nameDestructor;
    DWORD stringConstructor;
    DWORD stringDestructor;
    DWORD findBone;
    DWORD setType;
    DWORD setSource;
    DWORD setBone;
    DWORD refreshBone;
    DWORD setPosition;
    DWORD setDirection;
    DWORD execute;
    DWORD finalize;
    DWORD deleteEntity;
    DWORD setTransform;
    DWORD groundHeight;
};

// The same engine helpers called by Interaction::eAction_Spawn for each host.
constexpr SpawnLayout layouts[]{
    {0x6ab3dcc7, 0x556400, 0x556220, 0x5564e0, 0x6741b0, 0x674210,
     0x5c0a90, 0x705990, 0x705b40, 0x705b90, 0x5c2690, 0x705a20, 0x705bc0, 0x7058f0, 0x705cd0,
     0x6fcdc0, 0x6ff260, 0x704380},
    {0x6ab3dd83, 0x3ddb00, 0x3dd8b0, 0x3ddc00, 0x4e9c20, 0x4e9c80,
     0x4379a0, 0x57f920, 0x57fb00, 0x57fb50, 0x439610, 0x57f9e0, 0x57fb80, 0x57f880, 0x57fcd0,
     0x5767a0, 0x578e80, 0x57e110},
};

struct Vector3 {
    float x;
    float y;
    float z;
};

struct ExitPose {
    Vector3 position;
    Vector3 direction;
};

using GroundHeight = float (*)(const TrenchVector*);

float TrenchCapDrop(const TrenchTransform& pose, GroundHeight groundHeight) {
    // Outer boundary of it_trench_end_cap.ply in entity coordinates,
    // including the Y reflection in the MDL. Exclude the edges inside the cut.
    constexpr TrenchVector rim[]{
        {-7.204f, 37.252f, -3.330f},
        {-11.780f, 26.580f, -1.772f},
        {-17.964f, 12.190f, -1.772f},
        {-19.314f, -3.103f, -1.772f},
        {-17.113f, -17.944f, -1.772f},
        {-4.873f, -36.508f, -2.720f},
    };
    constexpr float clearanceTolerance = 0.25f;
    constexpr float maxGroundDelta = 18.0f;
    constexpr float maxDrop = 4.0f;
    float clearance = clearanceTolerance;
    const auto measure = [&](const TrenchVector& local) {
        const TrenchVector world{
            pose.origin.x + local.x * pose.xAxis.x + local.y * pose.yAxis.x + local.z * pose.zAxis.x,
            pose.origin.y + local.x * pose.xAxis.y + local.y * pose.yAxis.y + local.z * pose.zAxis.y,
            pose.origin.z + local.x * pose.xAxis.z + local.y * pose.yAxis.z + local.z * pose.zAxis.z,
        };
        const float ground = groundHeight(&world);
        if (std::isfinite(ground) && std::fabs(ground - pose.origin.z) <= maxGroundDelta) {
            clearance = (std::max)(clearance, world.z - ground);
        }
    };
    measure(rim[0]);
    for (unsigned i = 1; i < ARRAYSIZE(rim); ++i) {
        const auto& a = rim[i - 1];
        const auto& b = rim[i];
        measure({(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f});
        measure(b);
    }
    // Close small gaps without pulling the rigid inner wall far below its seam.
    return (std::min)(maxDrop, clearance - clearanceTolerance);
}

struct alignas(16) SpawnRequest {
    DWORD flags{};
    DWORD padding{};
    NativeString type{};
    unsigned char reserved[8]{};
    DWORD entityId{0xffffffff};
    unsigned char parameters[0x3c]{};
    float spawnDefaults[3]{1.0f, 0.0f, 1.0f};
    DWORD alignmentPadding{};
    alignas(8) unsigned char firstName[0x60]{};
    alignas(8) unsigned char secondName[0x60]{};
};
static_assert(offsetof(SpawnRequest, type) == 0x08);
static_assert(offsetof(SpawnRequest, entityId) == 0x30);
static_assert(offsetof(SpawnRequest, spawnDefaults) == 0x70);
static_assert(offsetof(SpawnRequest, firstName) == 0x80);
static_assert(offsetof(SpawnRequest, secondName) == 0xe0);
static_assert(sizeof(SpawnRequest) == 0x140);

void* ResolveBone(const unsigned char* image, const SpawnLayout& layout, void* skeleton,
                  const NativeString& boneName) {
    using Construct = void (*)(void*, const NativeString*);
    using Find = void* (*)(void*, const void*);
    using Destroy = void (*)(void*);
    alignas(8) unsigned char name[0x60]{};
    reinterpret_cast<Construct>(image + layout.boneNameConstructor)(name, &boneName);
    void* bone = reinterpret_cast<Find>(image + layout.findBone)(skeleton, name);
    reinterpret_cast<Destroy>(image + layout.nameDestructor)(name);
    return bone;
}

bool ReadOffsetPose(const unsigned char* image, const SpawnLayout& layout, void* bone,
                    float outwardOffset, ExitPose& pose) {
    const auto* bytes = static_cast<const unsigned char*>(bone);
    // These are the same cache flag, world X axis, and position read by GEM's
    // built-in spawn request when it uses a bone with the "x" orientation mode.
    if ((bytes[0x10] & 1) != 0) {
        using Refresh = void (*)(void*);
        reinterpret_cast<Refresh>(image + layout.refreshBone)(bone);
    }
    CopyMemory(&pose.direction, bytes + 0xa8, sizeof(pose.direction));
    CopyMemory(&pose.position, bytes + 0xcc, sizeof(pose.position));
    const auto& x = pose.direction;
    const float length = std::sqrt(x.x * x.x + x.y * x.y + x.z * x.z);
    if (!std::isfinite(length) || length == 0.0f) {
        return false;
    }

    // The exit mesh's positive X points back toward the trench.
    const float distance = outwardOffset / length;
    pose.position.x -= distance * x.x;
    pose.position.y -= distance * x.y;
    pose.position.z -= distance * x.z;
    return std::isfinite(pose.position.x) && std::isfinite(pose.position.y) &&
           std::isfinite(pose.position.z);
}

void* SpawnEntity(const unsigned char* image, const SpawnLayout& layout, const NativeString& type,
                  void* section, void* bone, const ExitPose* pose) {
    using Init = void (*)(void*);
    using SetType = void (*)(SpawnRequest*, const NativeString*);
    using SetSource = void (*)(SpawnRequest*, void*, bool);
    using SetBone = void (*)(SpawnRequest*, void*, int);
    using SetVector = void (*)(SpawnRequest*, const Vector3*);
    using Execute = void* (*)(SpawnRequest*);

    SpawnRequest request;
    const auto initName = reinterpret_cast<Init>(image + layout.nameConstructor);
    const auto destroyName = reinterpret_cast<Init>(image + layout.nameDestructor);
    reinterpret_cast<Init>(image + layout.stringConstructor)(&request.type);
    initName(request.firstName);
    initName(request.secondName);

    reinterpret_cast<SetType>(image + layout.setType)(&request, &type);
    if (pose != nullptr) {
        reinterpret_cast<SetVector>(image + layout.setPosition)(&request, &pose->position);
        reinterpret_cast<SetVector>(image + layout.setDirection)(&request, &pose->direction);
    }
    if (section != nullptr) {
        reinterpret_cast<SetSource>(image + layout.setSource)(&request, section, true);
    }
    if (pose == nullptr) {
        constexpr int boneXAxis = 1;
        reinterpret_cast<SetBone>(image + layout.setBone)(&request, bone, boneXAxis);
    }
    void* exit = reinterpret_cast<Execute>(image + layout.execute)(&request);

    // GEM calls finalize again before destroying these fields. The execute bit
    // makes this second call a no-op after the request has been consumed.
    reinterpret_cast<Execute>(image + layout.finalize)(&request);
    destroyName(request.secondName);
    destroyName(request.firstName);
    reinterpret_cast<Init>(image + layout.stringDestructor)(&request.type);
    return exit;
}
}

unsigned SpawnTrenchExits(const unsigned char* image, DWORD timestamp, void* section,
                          float outwardOffset, bool hasLeftExit, unsigned joinedSide,
                          void* (&exits)[2]) {
    for (const auto& layout : layouts) {
        if (layout.timestamp != timestamp) {
            continue;
        }
        void* skeleton = nullptr;
        const auto* address = static_cast<const unsigned char*>(section) + 0xb0;
        if (!ReadProcessMemory(GetCurrentProcess(), address, &skeleton, sizeof(skeleton), nullptr) ||
            skeleton == nullptr) {
            return 0;
        }

        constexpr NativeString anchors[]{
            {"it_exit_left", 12, 15},
            {"it_exit_right", 13, 15},
        };
        void* bones[ARRAYSIZE(anchors)]{};
        ExitPose poses[ARRAYSIZE(anchors)]{};
        const unsigned first = hasLeftExit ? 0 : 1;
        const bool offsetRequested = outwardOffset != 0.0f;
        for (unsigned i = first; i < ARRAYSIZE(anchors); ++i) {
            if (i == joinedSide) {
                continue;
            }
            bones[i] = ResolveBone(image, layout, skeleton, anchors[i]);
            if (bones[i] == nullptr ||
                (offsetRequested && !ReadOffsetPose(image, layout, bones[i], outwardOffset, poses[i]))) {
                return 0;
            }
        }

        constexpr NativeString exitType{"it_trench_exit", 14, 15};
        unsigned created = 0;
        for (unsigned i = first; i < ARRAYSIZE(anchors); ++i) {
            if (i == joinedSide) {
                continue;
            }
            exits[i] = SpawnEntity(image, layout, exitType, section, bones[i], offsetRequested ? &poses[i] : nullptr);
            if (exits[i] != nullptr) {
                ++created;
            }
        }
        return created;
    }
    return 0;
}

void* SpawnTrenchCap(const unsigned char* image, DWORD timestamp, TrenchVector position,
                    TrenchVector inward) {
    for (const auto& layout : layouts) {
        if (layout.timestamp == timestamp) {
            constexpr char name[] = "it_trench_end_cap";
            const char* text = name;
            NativeString type{{}, sizeof(name) - 1, sizeof(name) - 1};
            // SetType copies the input string. A name longer than 15 bytes uses the pointer slot.
            CopyMemory(type.text, &text, sizeof(text));
            const ExitPose pose{{position.x, position.y, position.z}, {inward.x, inward.y, inward.z}};
            return SpawnEntity(image, layout, type, nullptr, nullptr, &pose);
        }
    }
    return nullptr;
}

void DeleteTrenchCap(const unsigned char* image, DWORD timestamp, const void* entity) {
    for (const auto& layout : layouts) {
        if (layout.timestamp == timestamp) {
            // The entity removal routine called by ia:delete with mode zero.
            using Delete = void (*)(const void*, unsigned);
            reinterpret_cast<Delete>(image + layout.deleteEntity)(entity, 0);
            return;
        }
    }
}

void SeatCompletedTrenchCap(const unsigned char* image, DWORD timestamp, const void* entity,
                            TrenchVector position, TrenchVector outward, const float* boneOffset) {
    for (const auto& layout : layouts) {
        if (layout.timestamp == timestamp) {
            auto* bytes = static_cast<unsigned char*>(const_cast<void*>(entity));
            // Grounded placement recomputes Z on terrain updates. Once the
            // trench exists, its socket supplies the cap's world origin.
            constexpr DWORD terrainAlignmentMask = 0x1c;
            auto& flags = *reinterpret_cast<DWORD*>(bytes + 0x38);
            flags &= ~terrainAlignmentMask;

            TrenchTransform transform{
                {-outward.x, -outward.y, 0.0f}, {outward.y, -outward.x, 0.0f},
                {0.0f, 0.0f, 1.0f}, position,
            };
            static_assert(sizeof(transform) == 0x30 && offsetof(TrenchTransform, origin) == 0x24);
            if (boneOffset != nullptr) {
                transform.origin.z += *boneOffset;
            } else {
                const auto groundHeight = reinterpret_cast<GroundHeight>(image + layout.groundHeight);
                transform.origin.z -= TrenchCapDrop(transform, groundHeight);
            }
            using SetTransform = void (*)(void*, unsigned, const TrenchTransform*);
            reinterpret_cast<SetTransform>(image + layout.setTransform)(bytes, 0, &transform);
            return;
        }
    }
}

bool ReadStraightTrenchCapOffsets(const unsigned char* image, DWORD timestamp, const void* section,
                                  const TrenchTransform& transform, float (&offsets)[2]) {
    struct ReferenceBone {
        NativeString name;
        float x, y;
    };
    // Point bones at the two ends of trench_dirt_straight, ordered like the
    // sockets. Y includes the root's reflection, and all four bind Zs match.
    constexpr ReferenceBone tips[2][2]{
        {{{"point1", 6, 15}, -75.0445f, -12.8509f}, {{"point5", 6, 15}, -72.558f, 13.5111f}},
        {{{"point4", 6, 15}, 75.0252f, -14.4702f}, {{"point8", 6, 15}, 83.1338f, 15.3822f}},
    };
    constexpr float bindZ = -3.6684f;
    for (const auto& layout : layouts) {
        if (layout.timestamp != timestamp) {
            continue;
        }
        void* skeleton = nullptr;
        const auto* field = static_cast<const unsigned char*>(section) + 0xb0;
        if (!ReadProcessMemory(GetCurrentProcess(), field, &skeleton, sizeof(skeleton), nullptr) ||
            skeleton == nullptr) {
            return false;
        }
        for (unsigned side = 0; side < ARRAYSIZE(tips); ++side) {
            float sum = 0.0f;
            for (const auto& tip : tips[side]) {
                auto* bone = ResolveBone(image, layout, skeleton, tip.name);
                ExitPose pose{};
                if (bone == nullptr || !ReadOffsetPose(image, layout, bone, 0.0f, pose)) {
                    return false;
                }
                const float bindWorldZ = transform.origin.z + tip.x * transform.xAxis.z +
                                         tip.y * transform.yAxis.z + bindZ * transform.zAxis.z;
                const float offset = pose.position.z - bindWorldZ;
                if (!std::isfinite(offset) || std::fabs(offset) > 18.0f) {
                    return false;
                }
                sum += offset;
            }
            offsets[side] = sum * 0.5f;
        }
        return true;
    }
    return false;
}

bool ReadStraightTrenchLandPose(const unsigned char* image, DWORD timestamp, const void* section,
                                TrenchVector& origin, TrenchVector& xAxis) {
    for (const auto& layout : layouts) {
        if (layout.timestamp == timestamp) {
            void* skeleton = nullptr;
            const auto* field = static_cast<const unsigned char*>(section) + 0xb0;
            if (!ReadProcessMemory(GetCurrentProcess(), field, &skeleton, sizeof(skeleton), nullptr) ||
                skeleton == nullptr) {
                return false;
            }
            constexpr NativeString trenchBone{"trench", 6, 15};
            auto* bone = ResolveBone(image, layout, skeleton, trenchBone);
            ExitPose pose{};
            if (bone == nullptr || !ReadOffsetPose(image, layout, bone, 0.0f, pose)) {
                return false;
            }
            origin = {pose.position.x, pose.position.y, pose.position.z};
            xAxis = {pose.direction.x, pose.direction.y, pose.direction.z};
            return true;
        }
    }
    return false;
}
