#pragma once

#include <string>

namespace komira {

enum class IntensityStandard { Csis, Jma };

/// 《NATIVE_PORT_SPEC》 §4.3 烈度。
class IntensityCalculator {
public:
    static double rawCsis(double magnitude, double distanceKm, double depthKm = 10.0);
    static std::string formatCsis(double raw);
    static std::string formatJma(double magnitude, double distanceKm, double depthKm = 10.0);
    static std::string description(double raw);
};

} // namespace komira
