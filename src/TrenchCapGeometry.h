#pragma once
#include "TrenchCapProfile.h"
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

namespace trench {
enum class CapGeometryStatus { Solved, InvalidSample, ContactMismatch, RimInfeasible, OutOfDomain };

// origin already includes the profile's capOffset; inward is a horizontal unit
// direction (a finite, nonzero input is normalized). All samples are one coherent
// scene snapshot. There is one ground height per profile.rimSamples entry.
struct CapGeometryInput {
    const CapProfile* profile = nullptr;
    TrenchPoint origin{};
    TrenchPoint inward{};
    std::vector<TrenchVector> contactWorld;
    std::vector<double> groundHeights;
    std::optional<TrenchTransform> previous;
};

struct CapGeometryResult {
    CapGeometryStatus status = CapGeometryStatus::InvalidSample;
    TrenchTransform transform{}; // meaningful only when status == Solved
    double a = 0, b = 0, h = 0;
    double maxContactXY = 0, maxContactZ = 0;
    double minLipClearance = 0, maxLipClearance = 0;
    double minSkirtClearance = 0, maxSkirtClearance = 0;
    double rimObjective = 0, previousObjective = 0;
    std::size_t firstInvalidPoint = (std::numeric_limits<std::size_t>::max)();
};

// Pure convex geometry. No GEM APIs, files, mutation, terrain interpolation or
// claims about the unsampled surface. Inequalities are hard constraints.
// The three objectives are solved sequentially in their remaining nullspaces,
// never combined with a penalty weight. Candidate objective ties use a relative
// tolerance of 1e-10; normalized halfspace feasibility uses 2e-8. A final check
// includes the float transform that the native adapter would actually receive.
CapGeometryResult SolveTrenchCapGeometry(const CapGeometryInput& input);
const char* CapGeometryStatusName(CapGeometryStatus status) noexcept;
TrenchVector TransformCapPoint(const TrenchTransform& transform, TrenchVector local) noexcept;
// The native cut apply is one-shot. A late placement may change only height;
// moving its 2D footprint requires a separately proven cut re-registration path.
bool SameCapCutFootprint(const TrenchTransform& current, const TrenchTransform& proposed) noexcept;
} // namespace trench
