#include "TrenchCapSampling.h"
#include <cmath>
#include <utility>

namespace trench {
namespace {
bool Finite(TrenchVector p) noexcept {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
bool Finite(const TrenchTransform& t) noexcept {
    return Finite(t.origin) && Finite(t.xAxis) && Finite(t.yAxis) && Finite(t.zAxis);
}
bool Current(CapSamplingSource& source, const CapSampleStamp& expected) {
    CapSampleStamp actual{};
    return source.CoherentReadAdmitted() && source.ReadStamp(actual) && SameCapSampleStamp(actual, expected);
}
bool ValidRequest(const CapSamplingRequest& request) {
    const auto* profile = request.profile;
    const auto& stamp = request.stamp;
    if (!profile || profile->capEntity != "it_trench_dirt_end_cap" ||
        profile->socketId != stamp.socketId || stamp.socketId == noTrenchSocket ||
        stamp.sceneEpoch == 0 || stamp.owner == 0 || stamp.frame == 0 ||
        stamp.sectionGeneration == 0 || stamp.capGeneration == 0 ||
        !Finite(request.seam) || !std::isfinite(request.inward.x) || !std::isfinite(request.inward.y) ||
        std::hypot(static_cast<double>(request.inward.x), request.inward.y) < 1e-8 ||
        !std::isfinite(profile->capOffset.x) || !std::isfinite(profile->capOffset.y) ||
        profile->contact.size() < 4 || profile->contact.size() > 32 ||
        profile->rimSamples.size() < 3 || profile->rimSamples.size() > 128) return false;
    for (const auto& point : profile->contact) {
        if (point.sectionBone.empty() || point.sectionBone.find('\0') != std::string::npos ||
            !Finite(point.sectionBind) || !Finite(point.capBind)) return false;
    }
    for (std::size_t i = 0; i < profile->rimSamples.size(); ++i) {
        const auto& point = profile->rimSamples[i];
        if (!Finite(point.lip) || !Finite(point.skirt) || point.lip.x != point.skirt.x ||
            point.lip.y != point.skirt.y || point.skirt.z >= point.lip.z) return false;
        for (std::size_t j = 0; j < i; ++j) {
            const auto& previous = profile->rimSamples[j].lip;
            if (previous.x == point.lip.x && previous.y == point.lip.y) return false;
        }
    }
    return true;
}
CapSamplingResult Failed(CapSamplingStatus status, CapSamplingStage stage,
                         std::size_t index = (std::numeric_limits<std::size_t>::max)()) {
    return {status, stage, index};
}
}

CapSamplingResult BuildTrenchCapSample(CapSamplingSource& source,
    const CapSamplingRequest& request, CapRuntimeSample& output) {
    output = {};
    if (!ValidRequest(request)) return {};
    if (!source.CoherentReadAdmitted())
        return Failed(CapSamplingStatus::BackendNotAdmitted, CapSamplingStage::None);
    if (!Current(source, request.stamp))
        return Failed(CapSamplingStatus::StaleIdentity, CapSamplingStage::Stamp);

    CapRuntimeSample sample{};
    sample.stamp = request.stamp;
    sample.capturedAt = source.Now();
    const auto& profile = *request.profile;
    const double length = std::hypot(static_cast<double>(request.inward.x), request.inward.y);
    sample.inward = {static_cast<float>(request.inward.x / length), static_cast<float>(request.inward.y / length)};
    const auto u = sample.inward;
    // V=(-Uy,Ux). Profile data is already in canonical entity coordinates;
    // the MDL's reflected Y must not be reflected a second time here.
    sample.origin = {request.seam.x + u.x * profile.capOffset.x - u.y * profile.capOffset.y,
                     request.seam.y + u.y * profile.capOffset.x + u.x * profile.capOffset.y};
    if (!std::isfinite(sample.origin.x) || !std::isfinite(sample.origin.y))
        return Failed(CapSamplingStatus::InvalidValue, CapSamplingStage::None);

    const auto readRoot = [&](TrenchTransform& root, CapSamplingStage stage) {
        if (!Current(source, request.stamp)) return Failed(CapSamplingStatus::StaleIdentity, stage);
        const bool read = source.ReadRoot(root);
        if (!Current(source, request.stamp)) return Failed(CapSamplingStatus::StaleIdentity, stage);
        if (!read) return Failed(CapSamplingStatus::ReadFailed, stage);
        if (!Finite(root)) return Failed(CapSamplingStatus::InvalidValue, stage);
        return Failed(CapSamplingStatus::Complete, stage);
    };
    auto result = readRoot(sample.rootBefore, CapSamplingStage::RootBefore);
    if (result.status != CapSamplingStatus::Complete) return result;
    const auto readContacts = [&](std::vector<TrenchVector>& points, CapSamplingStage stage) {
        points.reserve(profile.contact.size());
        for (std::size_t i = 0; i < profile.contact.size(); ++i) {
            if (!Current(source, request.stamp)) return Failed(CapSamplingStatus::StaleIdentity, stage, i);
            TrenchVector point{};
            const bool read = source.ReadBoneWorld(profile.contact[i].sectionBone, point);
            if (!Current(source, request.stamp)) return Failed(CapSamplingStatus::StaleIdentity, stage, i);
            if (!read) return Failed(CapSamplingStatus::ReadFailed, stage, i);
            if (!Finite(point)) return Failed(CapSamplingStatus::InvalidValue, stage, i);
            points.push_back(point);
        }
        return Failed(CapSamplingStatus::Complete, stage);
    };
    result = readContacts(sample.contactBefore, CapSamplingStage::ContactBefore);
    if (result.status != CapSamplingStatus::Complete) return result;
    sample.groundHeights.reserve(profile.rimSamples.size());
    for (std::size_t i = 0; i < profile.rimSamples.size(); ++i) {
        const auto q = profile.rimSamples[i].lip;
        const TrenchVector point{sample.origin.x + u.x * q.x - u.y * q.y,
            sample.origin.y + u.y * q.x + u.x * q.y, request.seam.z + q.z};
        if (!Finite(point)) return Failed(CapSamplingStatus::InvalidValue, CapSamplingStage::Ground, i);
        if (!Current(source, request.stamp)) return Failed(CapSamplingStatus::StaleIdentity, CapSamplingStage::Ground, i);
        float height = 0;
        const bool read = source.ReadGroundHeight(point, height);
        if (!Current(source, request.stamp)) return Failed(CapSamplingStatus::StaleIdentity, CapSamplingStage::Ground, i);
        if (!read) return Failed(CapSamplingStatus::ReadFailed, CapSamplingStage::Ground, i);
        if (!std::isfinite(height)) return Failed(CapSamplingStatus::InvalidValue, CapSamplingStage::Ground, i);
        sample.groundHeights.push_back(height);
    }
    result = readContacts(sample.contactAfter, CapSamplingStage::ContactAfter);
    if (result.status != CapSamplingStatus::Complete) return result;
    result = readRoot(sample.rootAfter, CapSamplingStage::RootAfter);
    if (result.status != CapSamplingStatus::Complete) return result;
    if (!source.ReadStamp(sample.afterStamp) || !SameCapSampleStamp(sample.afterStamp, request.stamp))
        return Failed(CapSamplingStatus::StaleIdentity, CapSamplingStage::Stamp);
    sample.coherent = true; // Only the admitted source above establishes this.
    const auto reason = ValidateCoherentCapSample(sample, request.stamp, profile, source.Now());
    if (!Current(source, request.stamp))
        return Failed(CapSamplingStatus::StaleIdentity, CapSamplingStage::Stamp);
    if (reason == CapRuntimeReason::StaleSample)
        return Failed(CapSamplingStatus::StaleIdentity, CapSamplingStage::Stamp);
    if (reason != CapRuntimeReason::None)
        return Failed(reason == CapRuntimeReason::InvalidSample ? CapSamplingStatus::InvalidValue :
            CapSamplingStatus::IncoherentSample, CapSamplingStage::None);
    output = std::move(sample);
    return Failed(CapSamplingStatus::Complete, CapSamplingStage::None);
}

} // namespace trench
