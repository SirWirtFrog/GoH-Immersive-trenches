#pragma once
#include "TrenchCapGeometry.h"
#include <cstdint>
#include <optional>
#include <vector>

namespace trench {
// Generation fields are adapter-proven lifetime tokens, not pointer/ID hashes.
// A frame names one post-scene pass. No native sample spans several frames.
struct CapSampleStamp {
    std::uint64_t sceneEpoch = 0, owner = 0, desiredRevision = 0;
    std::uint64_t sectionGeneration = 0, capGeneration = 0;
    unsigned socketId = noTrenchSocket;
    std::uint64_t frame = 0;
};

struct CapRuntimeSample {
    CapSampleStamp stamp{}, afterStamp{};
    std::uint64_t capturedAt = 0;
    // This admission comes from a separately proven adapter read contract.
    // Equal root/locator reads alone cannot establish geometry/terrain coherence.
    bool coherent = false;
    TrenchTransform rootBefore{}, rootAfter{};
    std::vector<TrenchVector> contactBefore, contactAfter;
    std::vector<double> groundHeights;
    TrenchPoint origin{}, inward{}; // origin already includes capOffset
    // State reads the owned cap's current matrix with identity/epoch checks.
    // Suppression must notice native overwrites of a previously confirmed pose.
    std::optional<TrenchTransform> capObserved;
};

enum class CapVisualQuality { Sampling, Settled, Degraded };
enum class CapRuntimeReason {
    None, InvalidSample, StaleSample,
    IncoherentSample, SamplingTimeout, ContactMismatch, RimInfeasible,
    OutOfDomain, CutFootprintChanged, NativeApplyFailed
};

struct CapVisualState {
    CapVisualQuality quality = CapVisualQuality::Sampling;
    CapRuntimeReason reason = CapRuntimeReason::None;
    CapSampleStamp identity{};
    bool initialized = false;
    std::uint64_t samplingStarted = 0, nextSampleAt = 0;
    unsigned stableCount = 0;
    std::optional<CapRuntimeSample> stableBaseline, lastSample;
    std::optional<TrenchTransform> lastConfirmedApplied;
    // An old decision cannot confirm a newer candidate after reentry.
    std::uint64_t decisionSerial = 0, pendingSerial = 0;
};

struct CapRuntimeDecision {
    CapSampleStamp stamp{};
    std::uint64_t serial = 0;
    bool accepted = false;
    bool applyNeeded = false;
    CapGeometryResult geometry{};
};

bool SameCapSampleStamp(const CapSampleStamp& a, const CapSampleStamp& b) noexcept;
CapRuntimeReason ValidateCoherentCapSample(const CapRuntimeSample& sample,
    const CapSampleStamp& expected, const CapProfile& profile, std::uint64_t now) noexcept;
bool CapSampleDue(const CapVisualState& state, std::uint64_t now) noexcept;

// Pure state transitions. The caller admits the backend/profile and obtains a
// whole sample before calling; these functions never read or mutate GEM state.
CapRuntimeDecision ProcessCapSample(CapVisualState& state, const CapRuntimeSample& sample,
    const CapSampleStamp& expected, const CapProfile& profile, std::uint64_t now);
CapRuntimeDecision CapSampleFailed(CapVisualState& state, const CapSampleStamp& expected,
    std::uint64_t now, CapRuntimeReason reason);
// Called only after the adapter's post-apply identity/epoch check. A missing or
// unequal readback never updates the confirmed pose and never suppresses retry.
bool ConfirmCapApplied(CapVisualState& state, const CapRuntimeDecision& decision,
    const CapSampleStamp& expected, const std::optional<TrenchTransform>& readback,
    std::uint64_t now) noexcept;
} // namespace trench
