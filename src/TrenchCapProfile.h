#pragma once
#include "TrenchTypes.h"
#include <filesystem>
#include <string>
#include <vector>

namespace trench {
struct CapContact {
    std::string name, sectionBone;
    TrenchVector sectionBind{}, capBind{};
};
struct CapRimPoint {
    std::string name;
    TrenchVector lip{}, skirt{};
};

// Authoring metadata only. Parsing does not certify that marker bones follow
// the rendered mesh, or admit the native affine-transform backend.
struct CapProfile {
    unsigned socketId = noTrenchSocket;
    std::string capEntity;
    TrenchPoint capOffset{};
    std::vector<CapContact> contact;
    std::vector<CapRimPoint> rim;
    std::vector<CapRimPoint> rimSamples;
};

// Bounded, open-polyline sampling in canonical entity coordinates. On failure
// output is empty. Every generated XY needs one terrain query for lip/skirt.
bool SampleCapRim(const std::vector<CapRimPoint>& rim,
                  std::vector<CapRimPoint>& output, std::string& reason);
const CapProfile* FindCapProfile(const TrenchModule& module, unsigned socketId) noexcept;
// Validates the fixed terrain-cutting cap resource independently of sidecars.
// Only the known trench include/macros and bounded cut/volume model are allowed.
// Does not establish native/backend capabilities or certify rendered geometry.
bool ValidateTrenchCapResource(const std::filesystem::path& entityRoot, std::string& diagnostic);
} // namespace trench
