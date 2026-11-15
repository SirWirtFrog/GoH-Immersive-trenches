#include "TrenchCapRuntime.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace trench {
namespace {
constexpr std::uint64_t fastInterval = 50, slowInterval = 500, samplingTimeout = 2000;
constexpr double pointTolerance = 0.05, matrixTolerance = 0.0001;
constexpr double directionCosine = 0.9999984769132877; // cos(0.1 degrees)
bool Finite(double value) noexcept { return std::isfinite(value); }
bool Finite(TrenchPoint p) noexcept { return Finite(p.x) && Finite(p.y); }
bool Finite(TrenchVector p) noexcept { return Finite(p.x) && Finite(p.y) && Finite(p.z); }
bool Finite(const TrenchTransform& t) noexcept {
    return Finite(t.xAxis) && Finite(t.yAxis) && Finite(t.zAxis) && Finite(t.origin);
}
double Distance(TrenchVector a, TrenchVector b) noexcept {
    return std::hypot(std::hypot(static_cast<double>(a.x) - b.x, static_cast<double>(a.y) - b.y),
        static_cast<double>(a.z) - b.z);
}
double Length(TrenchVector a) noexcept { return Distance(a, {}); }
bool ValidRoot(const TrenchTransform& t) noexcept {
    if (!Finite(t)) return false;
    const double lengths=Length(t.xAxis)*Length(t.yAxis)*Length(t.zAxis);
    if (!(lengths>1e-12)) return false;
    const double determinant=static_cast<double>(t.xAxis.x)*(static_cast<double>(t.yAxis.y)*t.zAxis.z-static_cast<double>(t.yAxis.z)*t.zAxis.y)-
        static_cast<double>(t.xAxis.y)*(static_cast<double>(t.yAxis.x)*t.zAxis.z-static_cast<double>(t.yAxis.z)*t.zAxis.x)+
        static_cast<double>(t.xAxis.z)*(static_cast<double>(t.yAxis.x)*t.zAxis.y-static_cast<double>(t.yAxis.y)*t.zAxis.x);
    return std::fabs(determinant/lengths)>1e-8;
}
bool SameDirection(TrenchVector a, TrenchVector b) noexcept {
    const double lengths = Length(a) * Length(b);
    return lengths > 1e-12 && (static_cast<double>(a.x)*b.x + static_cast<double>(a.y)*b.y +
        static_cast<double>(a.z)*b.z) / lengths >= directionCosine;
}
bool SameRoot(const TrenchTransform& a, const TrenchTransform& b) noexcept {
    if (!Finite(a) || !Finite(b) || Distance(a.origin, b.origin) > pointTolerance) return false;
    const TrenchVector aa[]{a.xAxis,a.yAxis,a.zAxis}, bb[]{b.xAxis,b.yAxis,b.zAxis};
    for (unsigned i = 0; i < 3; ++i)
        if (!SameDirection(aa[i],bb[i]) || std::fabs(Length(aa[i])-Length(bb[i])) > pointTolerance) return false;
    return true;
}
bool SameInstance(const CapSampleStamp& a, const CapSampleStamp& b) noexcept {
    return a.sceneEpoch == b.sceneEpoch && a.owner == b.owner && a.socketId == b.socketId &&
        a.sectionGeneration == b.sectionGeneration && a.capGeneration == b.capGeneration;
}
bool ValidStamp(const CapSampleStamp& stamp) noexcept {
    return stamp.sceneEpoch != 0 && stamp.owner != 0 && stamp.sectionGeneration != 0 &&
        stamp.capGeneration != 0 && stamp.socketId != noTrenchSocket && stamp.frame != 0;
}
std::uint64_t Later(std::uint64_t now, std::uint64_t delay) noexcept {
    const auto limit = (std::numeric_limits<std::uint64_t>::max)();
    return now > limit-delay ? limit : now+delay;
}
void StartSampling(CapVisualState& state, std::uint64_t now) {
    state.quality = CapVisualQuality::Sampling;
    state.reason = CapRuntimeReason::None;
    state.samplingStarted = now;
    state.stableCount = 0;
    state.stableBaseline.reset();
}
void Bind(CapVisualState& state, const CapSampleStamp& expected, std::uint64_t now) {
    if (!state.initialized || !SameInstance(state.identity, expected)) {
        const auto serial = state.decisionSerial;
        state = {};
        state.decisionSerial = serial; // same serial cannot re-confirm an old decision
        state.initialized = true;
        StartSampling(state, now);
    } else if (state.identity.desiredRevision != expected.desiredRevision) {
        StartSampling(state, now);
        state.lastSample.reset();
        state.pendingSerial = 0;
    }
    state.identity = expected;
}
CapRuntimeDecision Decision(CapVisualState& state, const CapSampleStamp& expected) {
    CapRuntimeDecision result;
    result.stamp = expected;
    result.serial = ++state.decisionSerial;
    state.pendingSerial = 0;
    return result;
}
bool SamePoints(const CapRuntimeSample& a, const CapRuntimeSample& b) noexcept {
    if (a.contactAfter.size() != b.contactAfter.size() || a.groundHeights.size() != b.groundHeights.size() ||
        !SameRoot(a.rootAfter,b.rootAfter) ||
        std::hypot(static_cast<double>(a.origin.x)-b.origin.x,static_cast<double>(a.origin.y)-b.origin.y) > pointTolerance ||
        !SameDirection({a.inward.x,a.inward.y,0},{b.inward.x,b.inward.y,0})) return false;
    for (std::size_t i=0;i<a.contactAfter.size();++i)
        if (Distance(a.contactAfter[i],b.contactAfter[i]) > pointTolerance) return false;
    for (std::size_t i=0;i<a.groundHeights.size();++i)
        if (std::fabs(a.groundHeights[i]-b.groundHeights[i]) > pointTolerance) return false;
    return true;
}
bool SameReadback(const TrenchTransform& a, const TrenchTransform& b) noexcept {
    if (!Finite(a) || !Finite(b)) return false;
    const TrenchVector aa[]{a.xAxis,a.yAxis,a.zAxis,a.origin}, bb[]{b.xAxis,b.yAxis,b.zAxis,b.origin};
    for (unsigned i=0;i<4;++i)
        if (std::fabs(static_cast<double>(aa[i].x)-bb[i].x)>matrixTolerance ||
            std::fabs(static_cast<double>(aa[i].y)-bb[i].y)>matrixTolerance ||
            std::fabs(static_cast<double>(aa[i].z)-bb[i].z)>matrixTolerance) return false;
    return true;
}
bool SameRenderedPoints(const CapProfile& p, const TrenchTransform& a, const TrenchTransform& b) noexcept {
    for (const auto& c:p.contact)
        if (Distance(TransformCapPoint(a,c.capBind),TransformCapPoint(b,c.capBind))>pointTolerance) return false;
    for (const auto& r:p.rimSamples)
        for (const auto q:{r.lip,r.skirt})
            if (Distance(TransformCapPoint(a,q),TransformCapPoint(b,q))>pointTolerance) return false;
    return true;
}
// Suppression is allowed only while the already confirmed pose remains within
// every hard bound. A tiny candidate movement must not retain a failing seam.
bool FitsSample(const TrenchTransform& t, const CapRuntimeSample& sample, const CapProfile& profile) noexcept {
    constexpr double slope = 0.36397023426620234;
    if (!Finite(t) || std::fabs(t.xAxis.z)>slope || std::fabs(t.yAxis.z)>slope) return false;
    double seam = 0;
    for (std::size_t i=0;i<profile.contact.size();++i) {
        const auto q=profile.contact[i].capBind, actual=TransformCapPoint(t,q), wanted=sample.contactAfter[i];
        if (std::hypot(static_cast<double>(actual.x)-wanted.x,static_cast<double>(actual.y)-wanted.y)>0.25 ||
            std::fabs(static_cast<double>(actual.z)-wanted.z)>0.5) return false;
        seam+=(static_cast<double>(wanted.z)-q.z)/static_cast<double>(profile.contact.size());
    }
    if (std::fabs(static_cast<double>(t.origin.z)-seam)>64) return false;
    for (std::size_t i=0;i<profile.rimSamples.size();++i) {
        const auto& r=profile.rimSamples[i];
        const double lip=static_cast<double>(TransformCapPoint(t,r.lip).z)-sample.groundHeights[i];
        const double skirt=static_cast<double>(TransformCapPoint(t,r.skirt).z)-sample.groundHeights[i];
        if (lip < -4 || lip > 0.5 || skirt < -8 || skirt > -0.5) return false;
    }
    return true;
}
CapRuntimeReason GeometryReason(CapGeometryStatus status) noexcept {
    switch (status) {
    case CapGeometryStatus::Solved:return CapRuntimeReason::None;
    case CapGeometryStatus::ContactMismatch:return CapRuntimeReason::ContactMismatch;
    case CapGeometryStatus::RimInfeasible:return CapRuntimeReason::RimInfeasible;
    case CapGeometryStatus::OutOfDomain:return CapRuntimeReason::OutOfDomain;
    default:return CapRuntimeReason::InvalidSample;
    }
}
void FinishSchedule(CapVisualState& state, std::uint64_t now) noexcept {
    state.nextSampleAt=Later(now,state.quality==CapVisualQuality::Sampling ? fastInterval : slowInterval);
}
} // namespace

bool SameCapSampleStamp(const CapSampleStamp& a, const CapSampleStamp& b) noexcept {
    return SameInstance(a,b) && a.desiredRevision==b.desiredRevision && a.frame==b.frame;
}
CapRuntimeReason ValidateCoherentCapSample(const CapRuntimeSample& s, const CapSampleStamp& expected,
    const CapProfile& p, std::uint64_t now) noexcept {
    if (!ValidStamp(expected) || !SameCapSampleStamp(s.stamp,expected) ||
        !SameCapSampleStamp(s.afterStamp,expected) || s.capturedAt>now) return CapRuntimeReason::StaleSample;
    if (!s.coherent) return CapRuntimeReason::IncoherentSample;
    if (p.socketId!=expected.socketId || p.contact.size()<4 || p.contact.size()>32 ||
        p.rimSamples.size()<3 || p.rimSamples.size()>128 || s.contactBefore.size()!=p.contact.size() ||
        s.contactAfter.size()!=p.contact.size() || s.groundHeights.size()!=p.rimSamples.size() ||
        !Finite(s.origin) || !Finite(s.inward) || std::hypot(s.inward.x,s.inward.y)<1e-8 ||
        !ValidRoot(s.rootBefore) || !ValidRoot(s.rootAfter) || (s.capObserved && !Finite(*s.capObserved)))
        return CapRuntimeReason::InvalidSample;
    for (std::size_t i=0;i<s.contactBefore.size();++i)
        if (!Finite(s.contactBefore[i]) || !Finite(s.contactAfter[i])) return CapRuntimeReason::InvalidSample;
    for (const auto height:s.groundHeights) if (!Finite(height)) return CapRuntimeReason::InvalidSample;
    if (!SameRoot(s.rootBefore,s.rootAfter)) return CapRuntimeReason::IncoherentSample;
    for (std::size_t i=0;i<s.contactBefore.size();++i)
        if (Distance(s.contactBefore[i],s.contactAfter[i])>pointTolerance) return CapRuntimeReason::IncoherentSample;
    return CapRuntimeReason::None;
}
bool CapSampleDue(const CapVisualState& state, std::uint64_t now) noexcept {
    return !state.initialized || now>=state.nextSampleAt;
}
CapRuntimeDecision CapSampleFailed(CapVisualState& state, const CapSampleStamp& expected,
    std::uint64_t now, CapRuntimeReason reason) {
    Bind(state,expected,now);
    state.stableCount=0;
    state.stableBaseline.reset();
    const bool timedOut=now>=state.samplingStarted && now-state.samplingStarted>=samplingTimeout;
    if (state.quality!=CapVisualQuality::Sampling || timedOut || reason==CapRuntimeReason::NativeApplyFailed ||
        reason==CapRuntimeReason::CutFootprintChanged) state.quality=CapVisualQuality::Degraded;
    state.reason=timedOut && state.quality==CapVisualQuality::Degraded && reason==CapRuntimeReason::None ?
        CapRuntimeReason::SamplingTimeout : reason;
    FinishSchedule(state,now);
    return Decision(state,expected);
}
CapRuntimeDecision ProcessCapSample(CapVisualState& state, const CapRuntimeSample& sample,
    const CapSampleStamp& expected, const CapProfile& profile, std::uint64_t now) {
    Bind(state,expected,now);
    const auto validation=ValidateCoherentCapSample(sample,expected,profile,now);
    if (validation!=CapRuntimeReason::None) return CapSampleFailed(state,expected,now,validation);
    if (state.lastSample && (sample.stamp.frame<=state.lastSample->stamp.frame || sample.capturedAt<state.lastSample->capturedAt))
        return CapSampleFailed(state,expected,now,CapRuntimeReason::StaleSample);
    const bool samePrevious=state.lastSample && SamePoints(sample,*state.lastSample);
    if (state.quality==CapVisualQuality::Degraded || (state.quality==CapVisualQuality::Settled &&
        (!samePrevious || !state.stableBaseline || !SamePoints(sample,*state.stableBaseline))))
        StartSampling(state,now);
    if (state.quality==CapVisualQuality::Sampling) {
        if (!state.stableBaseline || !samePrevious || !SamePoints(sample,*state.stableBaseline)) {
            state.stableBaseline=sample;
            state.stableCount=1;
        } else if (state.stableCount<3) ++state.stableCount;
    }
    CapGeometryInput input;
    input.profile=&profile; input.origin=sample.origin; input.inward=sample.inward;
    input.contactWorld=sample.contactAfter; input.groundHeights=sample.groundHeights;
    input.previous=state.lastConfirmedApplied;
    const auto geometry=SolveTrenchCapGeometry(input);
    state.lastSample=sample;
    if (geometry.status!=CapGeometryStatus::Solved) {
        auto result=CapSampleFailed(state,expected,now,GeometryReason(geometry.status));
        state.quality=CapVisualQuality::Degraded;
        state.nextSampleAt=Later(now,slowInterval);
        result.geometry=geometry;
        return result;
    }
    if (state.lastConfirmedApplied && !SameCapCutFootprint(*state.lastConfirmedApplied,geometry.transform)) {
        auto result=CapSampleFailed(state,expected,now,CapRuntimeReason::CutFootprintChanged);
        result.geometry=geometry;
        return result;
    }
    state.reason=CapRuntimeReason::None;
    const bool applyNeeded=!state.lastConfirmedApplied || !SameRenderedPoints(profile,*state.lastConfirmedApplied,geometry.transform) ||
        !FitsSample(*state.lastConfirmedApplied,sample,profile) || !sample.capObserved ||
        !SameReadback(*state.lastConfirmedApplied,*sample.capObserved);
    if (applyNeeded && state.quality==CapVisualQuality::Settled) StartSampling(state,now);
    const bool stable=state.stableCount>=3 && state.stableBaseline &&
        sample.capturedAt>=state.stableBaseline->capturedAt && sample.capturedAt-state.stableBaseline->capturedAt>=100;
    if (stable && !applyNeeded) state.quality=CapVisualQuality::Settled;
    else if (state.quality==CapVisualQuality::Sampling && now>=state.samplingStarted && now-state.samplingStarted>=samplingTimeout) {
        state.quality=CapVisualQuality::Degraded;
        state.reason=CapRuntimeReason::SamplingTimeout;
    }
    FinishSchedule(state,now);
    auto result=Decision(state,expected);
    result.geometry=geometry;
    result.accepted=true;
    result.applyNeeded=applyNeeded;
    if (applyNeeded) state.pendingSerial=result.serial;
    return result;
}
bool ConfirmCapApplied(CapVisualState& state, const CapRuntimeDecision& decision,
    const CapSampleStamp& expected, const std::optional<TrenchTransform>& readback, std::uint64_t now) noexcept {
    if (!decision.accepted || !decision.applyNeeded || decision.geometry.status!=CapGeometryStatus::Solved ||
        decision.serial!=state.pendingSerial || !SameCapSampleStamp(decision.stamp,expected) ||
        !SameCapSampleStamp(state.identity,expected)) return false;
    state.pendingSerial=0;
    if (!readback || !SameReadback(decision.geometry.transform,*readback)) {
        state.quality=CapVisualQuality::Degraded;
        state.reason=CapRuntimeReason::NativeApplyFailed;
        state.stableCount=0;
        state.stableBaseline.reset();
        state.nextSampleAt=Later(now,slowInterval);
        return false;
    }
    state.lastConfirmedApplied=*readback;
    if (state.quality==CapVisualQuality::Sampling && state.stableCount>=3 && state.stableBaseline &&
        state.lastSample && state.lastSample->capturedAt>=state.stableBaseline->capturedAt &&
        state.lastSample->capturedAt-state.stableBaseline->capturedAt>=100) {
        state.quality=CapVisualQuality::Settled;
        state.reason=CapRuntimeReason::None;
    }
    FinishSchedule(state,now);
    return true;
}
} // namespace trench
