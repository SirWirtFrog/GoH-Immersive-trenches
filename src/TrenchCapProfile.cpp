#include "TrenchCapProfile.h"
#include <cmath>

namespace trench {
namespace {
bool Finite(TrenchVector value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
TrenchVector Interpolate(TrenchVector a, TrenchVector b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}
}
bool SampleCapRim(const std::vector<CapRimPoint>& rim,
                  std::vector<CapRimPoint>& output, std::string& reason) {
    output.clear(); reason.clear();
    if (rim.size() < 3 || rim.size() > 32) { reason = "rim requires 3..32 points"; return false; }
    std::vector<CapRimPoint> samples;
    for (const auto& point : rim) {
        if (!Finite(point.lip) || !Finite(point.skirt) ||
            point.lip.x != point.skirt.x || point.lip.y != point.skirt.y ||
            point.skirt.z >= point.lip.z) {
            reason = "rim requires finite lip/skirt with equal XY and skirt below lip";
            return false;
        }
    }
    samples.push_back(rim.front());
    for (std::size_t i = 1; i < rim.size(); ++i) {
        const auto& a = rim[i - 1]; const auto& b = rim[i];
        const double dx = static_cast<double>(b.lip.x) - a.lip.x;
        const double dy = static_cast<double>(b.lip.y) - a.lip.y;
        const double distance = std::hypot(dx, dy);
        if (distance == 0) { reason = "duplicate adjacent rim XY"; return false; }
        const double count = std::ceil(distance / 5.0);
        if (count > 127 || samples.size() + static_cast<std::size_t>(count) > 128) {
            reason = "rim sampling exceeds 128 points"; return false;
        }
        const auto steps = static_cast<unsigned>(count);
        for (unsigned step = 1; step <= steps; ++step) {
            const float t = static_cast<float>(step) / steps;
            CapRimPoint point{step == steps ? b.name : std::string{},
                Interpolate(a.lip, b.lip, t), Interpolate(a.skirt, b.skirt, t)};
            // A repeated XY on a different segment is ambiguous: its vertical
            // profile need not agree. Reject rather than discard a constraint.
            for (const auto& previous : samples) if (previous.lip.x == point.lip.x && previous.lip.y == point.lip.y) {
                reason = "repeated rim sample XY"; return false;
            }
            samples.push_back(std::move(point));
        }
    }
    output = std::move(samples);
    return true;
}
const CapProfile* FindCapProfile(const TrenchModule& module, unsigned socketId) noexcept {
    for (unsigned i = 0; i < module.socketCount && i < maxTrenchSockets; ++i)
        if (module.socketIds[i] == socketId) return module.capProfiles[i];
    return nullptr;
}
} // namespace trench
