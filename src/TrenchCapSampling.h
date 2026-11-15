#pragma once
#include "TrenchCapRuntime.h"
#include <limits>
#include <string_view>

namespace trench {
enum class CapSamplingStatus {
    Complete, BackendNotAdmitted, InvalidRequest, StaleIdentity,
    ReadFailed, InvalidValue, IncoherentSample
};
enum class CapSamplingStage { None, RootBefore, ContactBefore, Ground, ContactAfter, RootAfter, Stamp };

struct CapSamplingResult {
    CapSamplingStatus status = CapSamplingStatus::InvalidRequest;
    CapSamplingStage stage = CapSamplingStage::None;
    std::size_t pointIndex = (std::numeric_limits<std::size_t>::max)();
};

struct CapSamplingRequest {
    const CapProfile* profile = nullptr;
    CapSampleStamp stamp{};
    TrenchVector seam{};
    TrenchPoint inward{};
};

// Bound to one section/cap identity by the adapter. Every native operation must
// itself guard that identity before dereferencing GEM objects. ReadStamp must
// refuse invalidating scenes before inspecting their former native pointers.
struct CapSamplingSource {
    virtual ~CapSamplingSource() = default;
    // Separate native admission: unchanged roots/bones cannot prove this.
    virtual bool CoherentReadAdmitted() const noexcept { return false; }
    virtual std::uint64_t Now() const = 0;
    virtual bool ReadStamp(CapSampleStamp& stamp) const = 0;
    virtual bool ReadRoot(TrenchTransform& root) = 0;
    // World position of the named, cache-refreshed section locator. No bind
    // extrapolation, section root substitution or guessed pointN fallback.
    virtual bool ReadBoneWorld(std::string_view name, TrenchVector& point) = 0;
    virtual bool ReadGroundHeight(TrenchVector point, float& height) = 0;
};

// One synchronous post-scene operation. Revalidates the stamp around each
// source callback, then rereads every locator and the root in the same pass.
// Ground is queried once for each authored lip/skirt XY. No partial result is
// published on failure, and no native data is retained for a later frame.
// Success says nothing about locator-to-rendered-edge certification.
CapSamplingResult BuildTrenchCapSample(CapSamplingSource& source,
    const CapSamplingRequest& request, CapRuntimeSample& output);
} // namespace trench
