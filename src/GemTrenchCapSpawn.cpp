#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstddef>
#include <cmath>

#include "GemTrenchSnap.h"
#if defined(IT_CAP_AFFINE_PROBE) && defined(_DEBUG)
#include "TrenchCapGeometry.h"
#endif

namespace {
// One-shot marker consumed by the nested scene-registration hook. Keeping
// it thread-local avoids changing IDs for unrelated concurrent spawns.
thread_local bool registerClientTrenchCap = false;
bool capResourceAvailable = false;

// ABI representation only: ownership and destruction use GEM helpers,
// not this module's C++ runtime.
struct NativeString {
    char text[16];
    size_t length;
    size_t capacity;
};

// Native helper RVAs selected only after the host PE build was validated.
struct SpawnLayout {
    DWORD timestamp;
    DWORD boneNameConstructor;
    DWORD nameConstructor;
    DWORD nameDestructor;
    DWORD stringConstructor;
    DWORD stringDestructor;
    DWORD findBone;
    DWORD setType;
    DWORD refreshBone;
    DWORD setPosition;
    DWORD setDirection;
    DWORD execute;
    DWORD finalize;
    DWORD deleteEntity;
    DWORD setTransform;
};

// The same engine helpers called by Interaction::eAction_Spawn for each host.
constexpr SpawnLayout layouts[]{
    {0x6ab3dcc7, 0x556400, 0x556220, 0x5564e0, 0x6741b0, 0x674210,
     0x5c0a90, 0x705990, 0x5c2690, 0x705a20, 0x705bc0, 0x7058f0, 0x705cd0,
     0x6fcdc0, 0x6ff260},
    {0x6ab3dd83, 0x3ddb00, 0x3dd8b0, 0x3ddc00, 0x4e9c20, 0x4e9c80,
     0x4379a0, 0x57f920, 0x439610, 0x57f9e0, 0x57fb80, 0x57f880, 0x57fcd0,
     0x5767a0, 0x578e80},
};

struct Vector3 {
    float x;
    float y;
    float z;
};

struct SpawnPose {
    Vector3 position;
    Vector3 direction;
};

bool Finite(TrenchVector value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

// Reconstructed request layout passed directly to GEM. Offsets, alignment
// and engine-side field initialization must match the supported executable.
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

// Create the engine's bone-name wrapper temporarily and destroy it with
// the matching native allocator after lookup.
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

// Refresh a dirty bone cache before reading its world pose.
bool ReadBonePose(const unsigned char* image, const SpawnLayout& layout, void* bone, SpawnPose& pose) {
    const auto* bytes = static_cast<const unsigned char*>(bone);
    // These are the same cache flag, world X axis, and position read by GEM's
    // built-in spawn request when it uses a bone with the "x" orientation mode.
    unsigned char cacheFlags = 0;
    if (bone == nullptr ||
        !ReadProcessMemory(GetCurrentProcess(), bytes + 0x10, &cacheFlags, sizeof(cacheFlags), nullptr)) return false;
    if ((cacheFlags & 1) != 0) {
        using Refresh = void (*)(void*);
        reinterpret_cast<Refresh>(image + layout.refreshBone)(bone);
    }
    if (!ReadProcessMemory(GetCurrentProcess(), bytes + 0xa8, &pose.direction, sizeof(pose.direction), nullptr) ||
        !ReadProcessMemory(GetCurrentProcess(), bytes + 0xcc, &pose.position, sizeof(pose.position), nullptr)) return false;
    const auto& x = pose.direction;
    const float length = std::sqrt(x.x * x.x + x.y * x.y + x.z * x.z);
    return std::isfinite(length) && length != 0.0f &&
           std::isfinite(pose.position.x) && std::isfinite(pose.position.y) && std::isfinite(pose.position.z);
}

// Reproduce the native request lifecycle: initialize owned fields, configure,
// execute/finalize, then destroy fields with the same engine helpers.
void* SpawnEntity(const unsigned char* image, const SpawnLayout& layout, const NativeString& type,
                  const SpawnPose& pose, bool clientOnly) {
    using Init = void (*)(void*);
    using SetType = void (*)(SpawnRequest*, const NativeString*);
    using SetVector = void (*)(SpawnRequest*, const Vector3*);
    using Execute = void* (*)(SpawnRequest*);

    SpawnRequest request;
    const auto initName = reinterpret_cast<Init>(image + layout.nameConstructor);
    const auto destroyName = reinterpret_cast<Init>(image + layout.nameDestructor);
    reinterpret_cast<Init>(image + layout.stringConstructor)(&request.type);
    initName(request.firstName);
    initName(request.secondName);

    reinterpret_cast<SetType>(image + layout.setType)(&request, &type);
    reinterpret_cast<SetVector>(image + layout.setPosition)(&request, &pose.position);
    reinterpret_cast<SetVector>(image + layout.setDirection)(&request, &pose.direction);
    if (clientOnly) {
        // The game's local spawn setter bypasses the multiplayer spawn path.
        using SetLocal = SpawnRequest* (*)(SpawnRequest*);
        reinterpret_cast<SetLocal>(image + 0x705cc0)(&request);
    }
    const bool previousRegistration = registerClientTrenchCap;
    registerClientTrenchCap = clientOnly;
    void* exit = reinterpret_cast<Execute>(image + layout.execute)(&request);
    registerClientTrenchCap = previousRegistration;

    // GEM calls finalize again before destroying these fields. The execute bit
    // makes this second call a no-op after the request has been consumed.
    reinterpret_cast<Execute>(image + layout.finalize)(&request);
    destroyName(request.secondName);
    destroyName(request.firstName);
    reinterpret_cast<Init>(image + layout.stringDestructor)(&request.type);
    return exit;
}
}

// Consume once: only the cap registration directly inside this spawn should
// select the client-local ID pool.
bool TakeClientTrenchCapRegistration() noexcept {
    const bool clientOnly = registerClientTrenchCap;
    registerClientTrenchCap = false;
    return clientOnly;
}

bool IsLocalTrenchCapSpawn() noexcept {
    return registerClientTrenchCap;
}

// Use the same terrain query as completed-cap seating, with the verified host layout.
void SetTrenchCapResourceAvailable(bool available) noexcept {
    capResourceAvailable = available;
}

// Caps use explicit world-space poses and can be local to a network client.
void* SpawnTrenchCap(const unsigned char* image, DWORD timestamp, TrenchVector position,
                    TrenchVector inward, bool clientOnly) {
    if (!capResourceAvailable || !Finite(position) || !Finite(inward)) return nullptr;
    for (const auto& layout : layouts) {
        if (layout.timestamp == timestamp) {
            // The end wall needs its authored cut/land_patch to clear the soil
            // inside the cap. Queue anchors never enter this spawn path.
            constexpr char resource[] = "it_trench_dirt_end_cap";
            const char* text = resource;
            constexpr auto length = sizeof(resource) - 1;
            NativeString type{{}, length, length};
            // SetType copies the input string. A name longer than 15 bytes uses the pointer slot.
            CopyMemory(type.text, &text, sizeof(text));
            const SpawnPose pose{{position.x, position.y, position.z}, {inward.x, inward.y, inward.z}};
            return SpawnEntity(image, layout, type, pose, clientOnly);
        }
    }
    return nullptr;
}

// Client-only caps bypass authoritative deletion; callers check entity
// identity/liveness before entering these native routines.
void DeleteTrenchCap(const unsigned char* image, DWORD timestamp, const void* entity, bool clientOnly) {
    for (const auto& layout : layouts) {
        if (layout.timestamp == timestamp) {
            // The entity removal routine called by ia:delete with mode zero.
            using Delete = void (*)(const void*, unsigned);
            const DWORD entry = clientOnly ? 0x6febc0 : layout.deleteEntity;
            reinterpret_cast<Delete>(image + entry)(entity, 0);
            return;
        }
    }
}

#if defined(IT_CAP_AFFINE_PROBE) && defined(_DEBUG)
namespace {
// The probe verifies the written matrix after rechecking the entity lease.
bool ApplyTrenchCapTransform(const unsigned char* image, DWORD timestamp, const void* entity,
                             const TrenchTransform& transform) {
    if (!entity || !Finite(transform.origin) || !Finite(transform.xAxis) ||
        !Finite(transform.yAxis) || !Finite(transform.zAxis) ||
        std::fabs(transform.zAxis.x) > 0.0001f || std::fabs(transform.zAxis.y) > 0.0001f ||
        std::fabs(transform.zAxis.z - 1.0f) > 0.0001f) return false;
    for (const auto& layout : layouts) {
        if (layout.timestamp != timestamp) continue;
        auto* bytes = static_cast<unsigned char*>(const_cast<void*>(entity));
        TrenchTransform before{};
        DWORD flags = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), bytes + 0x50, &before, sizeof(before), nullptr) ||
            !trench::SameCapCutFootprint(before, transform) ||
            !ReadProcessMemory(GetCurrentProcess(), bytes + 0x38, &flags, sizeof(flags), nullptr)) return false;
        flags &= ~DWORD{0x1c};
        if (!WriteProcessMemory(GetCurrentProcess(), bytes + 0x38, &flags, sizeof(flags), nullptr)) return false;
        using SetTransform = void (*)(void*, unsigned, const TrenchTransform*);
        reinterpret_cast<SetTransform>(image + layout.setTransform)(bytes, 0, &transform);
        return true;
    }
    return false;
}

}
#endif

// A cutting cap has its own native root/mesh terrain placement. In the
// ordinary build this is a confirmation only: no legacy section-bone Z shift,
// no clearing of alignment flags, and no second SetTransform after land_patch.
// The diagnostic probe remains an explicit, separate matrix experiment.
bool SeatCompletedTrenchCap(const unsigned char* image, DWORD timestamp, const void* entity,
                            TrenchVector position, TrenchVector outward,
                            bool (*mayRead)(void*), void* guardContext) {
    if (!entity || !Finite(position) || !Finite(outward) ||
        std::hypot(outward.x, outward.y) < 0.001f || (mayRead && !mayRead(guardContext))) return false;
    bool supported = false;
    for (const auto& layout : layouts) supported = supported || layout.timestamp == timestamp;
    if (!supported) return false;
    const auto* bytes = static_cast<const unsigned char*>(entity);
    TrenchTransform actual{};
    if (!ReadProcessMemory(GetCurrentProcess(), bytes + 0x50, &actual, sizeof(actual), nullptr) ||
        !Finite(actual.origin) || !Finite(actual.xAxis) || !Finite(actual.yAxis) || !Finite(actual.zAxis) ||
        std::hypot(actual.xAxis.x, actual.xAxis.y) < 0.001f ||
        (mayRead && !mayRead(guardContext))) return false;
    const double determinant = actual.xAxis.x * (double(actual.yAxis.y) * actual.zAxis.z - double(actual.yAxis.z) * actual.zAxis.y) -
        actual.yAxis.x * (double(actual.xAxis.y) * actual.zAxis.z - double(actual.xAxis.z) * actual.zAxis.y) +
        actual.zAxis.x * (double(actual.xAxis.y) * actual.yAxis.z - double(actual.xAxis.z) * actual.yAxis.y);
    if (!std::isfinite(determinant) || std::fabs(determinant) < 1e-6) return false;
#if defined(IT_CAP_AFFINE_PROBE) && defined(_DEBUG)
    DWORD probeId = 0xffffffff;
    if (!ReadProcessMemory(GetCurrentProcess(), bytes + 0xc0, &probeId, sizeof(probeId), nullptr) ||
        probeId == 0xffffffff || (mayRead && !mayRead(guardContext))) return false;
    auto proposed = actual; // Keep native root height, XY and authored direction.
    proposed.xAxis.z = (probeId & 1) ? -0.10f : 0.15f;
    proposed.yAxis.z = (probeId & 1) ? 0.20f : -0.10f;
    if (!ApplyTrenchCapTransform(image, timestamp, entity, proposed) ||
        (mayRead && !mayRead(guardContext))) return false;
    TrenchTransform readback{};
    if (!ReadProcessMemory(GetCurrentProcess(), bytes + 0x50, &readback, sizeof(readback), nullptr) ||
        (mayRead && !mayRead(guardContext))) return false;
    const TrenchVector expected[]{proposed.xAxis, proposed.yAxis, proposed.zAxis, proposed.origin};
    const TrenchVector observed[]{readback.xAxis, readback.yAxis, readback.zAxis, readback.origin};
    for (unsigned i = 0; i < ARRAYSIZE(expected); ++i) {
        if (!Finite(observed[i]) || std::fabs(expected[i].x - observed[i].x) > 0.0001f ||
            std::fabs(expected[i].y - observed[i].y) > 0.0001f ||
            std::fabs(expected[i].z - observed[i].z) > 0.0001f) return false;
    }
    RecordTrenchCapAffineProbe(entity, proposed);
#else
    (void)image;
#endif
    return true;
}

// Read the terrain-aligned trench bone for diagnostics; the entity transform
// alone does not describe internal skeletal deformation.
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
            SpawnPose pose{};
            if (bone == nullptr || !ReadBonePose(image, layout, bone, pose)) {
                return false;
            }
            origin = {pose.position.x, pose.position.y, pose.position.z};
            xAxis = {pose.direction.x, pose.direction.y, pose.direction.z};
            return true;
        }
    }
    return false;
}
