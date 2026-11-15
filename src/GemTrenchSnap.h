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

// Adapter setup: verified hook installation and post-scene refresh.
const char* InitializeTrenchSnap(unsigned char* image, DWORD timestamp, DWORD entityTree, DWORD queueCall,
                                DWORD queueEntry, DWORD orderVtable, DWORD orderDestructor,
                                void (*log)(const char*)) noexcept;
bool InstallTrenchHeightRefresh(unsigned char* image, DWORD sceneQuantJump, DWORD sceneQuantEntry) noexcept;
// Native cap operations. clientOnly uses local spawn and a separate ID pool.
void SetTrenchCapResourceAvailable(bool available) noexcept;
#if defined(IT_CAP_AFFINE_PROBE) && defined(_DEBUG)
// Opt-in diagnostic build only. Never certifies markers or the renderer.
void RecordTrenchCapAffineProbe(const void* entity, const TrenchTransform& expected);
#endif
void* SpawnTrenchCap(const unsigned char* image, DWORD timestamp, TrenchVector position, TrenchVector inward,
                    bool clientOnly);
bool TakeClientTrenchCapRegistration() noexcept;
bool IsLocalTrenchCapSpawn() noexcept;
void DeleteTrenchCap(const unsigned char* image, DWORD timestamp, const void* entity, bool clientOnly);
// Ordinary cutting-cap path confirms native terrain pose without a second
// root transform; the separate diagnostic build explicitly tests affine writes.
bool SeatCompletedTrenchCap(const unsigned char* image, DWORD timestamp, const void* entity,
                            TrenchVector position, TrenchVector outward,
                            bool (*mayRead)(void*) = nullptr, void* guardContext = nullptr);
bool ReadStraightTrenchLandPose(const unsigned char* image, DWORD timestamp, const void* section,
                                TrenchVector& origin, TrenchVector& xAxis);
// Placement facade: prepare may adjust target; check returns nullptr when
// allowed, otherwise a stable diagnostic reason string.
bool PrepareTrenchPlacement(const TrenchModule& module, float modelYawOffset, const void* options, TrenchTarget& target,
                            TrenchPlacement& placement) noexcept;
const char* CheckTrenchPlacement(const TrenchPlacement& placement) noexcept;
// Remote preview lifecycle, keyed by GEM's visualizer key rather than order pointers.
unsigned TrackNetworkTrenchPreview(std::uint64_t key, const TrenchModule& module,
                                   TrenchVector position, float yaw);
bool RemoveNetworkTrenchPreview(std::uint64_t key);
bool EnableNetworkTrenchCaps(bool hooksInstalled) noexcept;
void ClearNetworkTrenches();
// Register a completed entity. Return its remaining anchor count and report
// newly completed connections through joinedCount.
unsigned TrackTrenchSection(const TrenchModule& module, const TrenchTransform& transform,
                            const void* section, unsigned& joinedCount,
                            bool replicated = false, bool networkHost = false);
