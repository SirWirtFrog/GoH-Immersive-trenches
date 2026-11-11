#pragma once
#include <cstdint>

struct TrenchVector {
    float x, y, z;
};

struct TrenchPoint {
    float x, y;
};

// Catalog coordinates are local to the module; outward yaw is in degrees.
struct TrenchSocket {
    TrenchPoint center;
    float outwardYawDegrees;
    float overlap = 5.0f;
};

// Indices into a module's cut outline for optional non-convex decomposition.
struct TrenchTriangle {
    unsigned char a, b, c;
};

constexpr unsigned noTrenchSocket = ~0u;

// Immutable catalog entry. Catalog storage outlives every placement/reservation
// that borrows these pointers; it does not own an engine entity.
struct TrenchModule {
    const char* entityName;
    const char* previewName;
    const TrenchPoint* cut;
    unsigned cutSize;
    unsigned socketCount;
    TrenchSocket sockets[3];
    const TrenchTriangle* triangles = nullptr;
    unsigned triangleCount = 0;
    unsigned defaultSocket = 0;
};

// Native placement target ABI (0xa8 bytes). Preserve unknown trailing data
// when copying a target; only the known geometry fields are adjusted.
struct TrenchTarget {
    std::uint32_t flags;
    TrenchVector first;
    TrenchVector second;
    unsigned char remainder[0xa8 - 0x1c];
};

// Basis vectors followed by origin, matching the verified GEM transform layout.
struct TrenchTransform {
    TrenchVector xAxis, yAxis, zAxis, origin;
};


// A prospective connection to an existing owner/socket. The overlap band
// permits the intended seam intersection while rejecting other cut overlaps.
struct TrenchJoint {
    std::uint64_t neighbor = 0;
    unsigned neighborSide = noTrenchSocket;
    unsigned socket = noTrenchSocket;
    TrenchVector seam{}, outward{};
    float overlapBehind = 0.0f, overlapAhead = 0.0f;
};

// Prepared pose plus up to three inferred connections. Preparation describes
// a candidate; it does not reserve sockets until BeginOrder succeeds.
struct TrenchPlacement {
    const TrenchModule* module = nullptr;
    TrenchTransform transform{};
    TrenchJoint joints[3]{};
    unsigned jointCount = 0;
};

