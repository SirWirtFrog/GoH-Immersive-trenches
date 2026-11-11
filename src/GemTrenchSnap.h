#pragma once

#include <windows.h>
#include <cstdint>

#include "TrenchTypes.h"

// Borrowed context valid only during a synchronous native placement call.
struct TrenchOrderScope {
    const void* action;
    const TrenchPlacement& placement;
    bool reserved = false;
    const char* blocked = nullptr;
};

extern thread_local TrenchOrderScope* activeTrenchOrder;

// Adapter setup: settings, verified hook installation and post-scene refresh.
void LoadTrenchSnapSettings(HMODULE module) noexcept;
const char* InitializeTrenchSnap(unsigned char* image, DWORD timestamp, DWORD entityTree, DWORD queueCall,
                                DWORD queueEntry, DWORD orderVtable, DWORD orderDestructor,
                                void (*log)(const char*)) noexcept;
bool InstallTrenchHeightRefresh(unsigned char* image, DWORD sceneQuantJump, DWORD sceneQuantEntry) noexcept;
// Native cap operations. clientOnly uses local spawn and a separate ID pool.
void* SpawnTrenchCap(const unsigned char* image, DWORD timestamp, TrenchVector position, TrenchVector inward,
                    bool clientOnly);
bool TakeClientTrenchCapRegistration() noexcept;
bool ReadTrenchGroundHeight(const unsigned char* image, DWORD timestamp, TrenchVector position,
                           float& height);
void DeleteTrenchCap(const unsigned char* image, DWORD timestamp, const void* entity, bool clientOnly);
void SeatCompletedTrenchCap(const unsigned char* image, DWORD timestamp, const void* entity,
                            TrenchVector position, TrenchVector outward, const float* boneOffset);
bool ReadStraightTrenchLandPose(const unsigned char* image, DWORD timestamp, const void* section,
                                TrenchVector& origin, TrenchVector& xAxis);
bool ReadStraightTrenchCapOffsets(const unsigned char* image, DWORD timestamp, const void* section,
                                  const TrenchTransform& transform, float (&offsets)[2]);
// Placement facade: prepare may adjust target; check returns nullptr when
// allowed, otherwise a stable diagnostic reason string.
bool PrepareTrenchPlacement(const TrenchModule& module, float modelYawOffset, const void* options, TrenchTarget& target,
                            TrenchPlacement& placement) noexcept;
const char* CheckTrenchPlacement(const TrenchPlacement& placement) noexcept;
// Remote preview lifecycle, keyed by GEM's visualizer key rather than order pointers.
unsigned TrackNetworkTrenchPreview(std::uint64_t key, const TrenchModule& module,
                                   TrenchVector position, float yaw);
bool RemoveNetworkTrenchPreview(std::uint64_t key);
bool EnableNetworkTrenchCaps() noexcept;
void ClearNetworkTrenches();
// Register a completed entity. Return its remaining anchor count and report
// newly completed connections through joinedCount.
unsigned TrackTrenchSection(const TrenchModule& module, const TrenchTransform& transform,
                            const void* section, unsigned& joinedCount, bool useStraightBones,
                            bool replicated = false);
