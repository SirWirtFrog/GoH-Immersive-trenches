#pragma once

#include <windows.h>
#include <cstdint>

struct TrenchVector {
    float x, y, z;
};

struct TrenchPoint {
    float x, y;
};

struct TrenchSocket {
    TrenchPoint center;
    float outwardYawDegrees;
    float overlap = 5.0f;
};

struct TrenchTriangle {
    unsigned char a, b, c;
};

constexpr unsigned noTrenchSocket = ~0u;

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

struct TrenchTarget {
    DWORD flags;
    TrenchVector first;
    TrenchVector second;
    unsigned char remainder[0xa8 - 0x1c];
};

struct TrenchTransform {
    TrenchVector xAxis, yAxis, zAxis, origin;
};

struct TrenchJoint {
    std::uint64_t neighbor = 0;
    unsigned neighborSide = noTrenchSocket;
    unsigned socket = noTrenchSocket;
    TrenchVector seam{}, outward{};
    float overlapBehind = 0.0f, overlapAhead = 0.0f;
};

struct TrenchPlacement {
    const TrenchModule* module = nullptr;
    TrenchTransform transform{};
    TrenchJoint joints[3]{};
    unsigned jointCount = 0;
};

struct TrenchOrderScope {
    const void* action;
    const TrenchPlacement& placement;
    bool reserved = false;
    const char* blocked = nullptr;
};

extern thread_local TrenchOrderScope* activeTrenchOrder;

void LoadTrenchSnapSettings(HMODULE module) noexcept;
const char* InitializeTrenchSnap(unsigned char* image, DWORD timestamp, DWORD entityTree, DWORD queueCall,
                                DWORD queueEntry, DWORD orderVtable, DWORD orderDestructor,
                                void (*log)(const char*)) noexcept;
bool InstallTrenchHeightRefresh(unsigned char* image, DWORD sceneQuantJump, DWORD sceneQuantEntry) noexcept;
void* SpawnTrenchCap(const unsigned char* image, DWORD timestamp, TrenchVector position, TrenchVector inward);
void DeleteTrenchCap(const unsigned char* image, DWORD timestamp, const void* entity);
void SeatCompletedTrenchCap(const unsigned char* image, DWORD timestamp, const void* entity,
                            TrenchVector position, TrenchVector outward, const float* boneOffset);
bool ReadStraightTrenchLandPose(const unsigned char* image, DWORD timestamp, const void* section,
                                TrenchVector& origin, TrenchVector& xAxis);
bool ReadStraightTrenchCapOffsets(const unsigned char* image, DWORD timestamp, const void* section,
                                  const TrenchTransform& transform, float (&offsets)[2]);
bool PrepareTrenchPlacement(const TrenchModule& module, float modelYawOffset, const void* options, TrenchTarget& target,
                            TrenchPlacement& placement) noexcept;
const char* CheckTrenchPlacement(const TrenchPlacement& placement) noexcept;
unsigned TrackTrenchSection(const TrenchModule& module, const TrenchTransform& transform,
                            const void* section, unsigned& joinedCount, bool useStraightBones = false);
