#include "core/intensity_calculator.h"

#include <algorithm>
#include <cmath>

#include "core/quake_calculator.h"

namespace komira {

namespace {
const char* const kRoman[] = {
    "0", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI", "XII",
};
constexpr int kRomanCount = 13;

/// 中国大陆 CEA 烈度衰减关系（对齐参考实现 kanameishi 的 calcCeaCsis）。
double ceaCsis(double magnitude, double distanceKm) {
    return 1.297 * magnitude - 4.368 * std::log10(distanceKm + 15.0) + 5.363;
}
} // namespace

double IntensityCalculator::rawCsis(double magnitude, double distanceKm, double depthKm) {
    if (magnitude <= 0.0) return 0.0;
    if (distanceKm > 10000.0) return 0.0;
    // 震源到观测点的直线距离（含地球曲率与深度）。深度过浅按 10 km 计，避免近场烈度虚高。
    const double radius = QuakeCalculator::kEarthRadiusKm;
    const double depth = std::max(depthKm, 10.0);
    const double theta = distanceKm / radius;
    const double vertical = radius - depth;
    const double lineDistance =
        std::sqrt(vertical * vertical + radius * radius - 2.0 * vertical * radius * std::cos(theta));
    // 破裂尺度：把有限断层等效为一个可忽略的近场距离，取其与震中距的较大衰减。
    const double rupture = std::pow(10.0, (magnitude - 3.821) / 1.86);
    const double hypoDistance = std::max({lineDistance - 10.0 - rupture, distanceKm - rupture,
                                          0.2 * (lineDistance - 10.0), 0.0});
    return std::max(0.0, (ceaCsis(magnitude, distanceKm) + ceaCsis(magnitude, hypoDistance)) / 2.0);
}

std::string IntensityCalculator::formatCsis(double raw) {
    int idx = static_cast<int>(std::lround(raw));
    idx = std::clamp(idx, 0, kRomanCount - 1);
    return kRoman[idx];
}

std::string IntensityCalculator::formatJma(double magnitude, double distanceKm, double depthKm) {
    const double r = std::max(1e-6, std::sqrt(distanceKm * distanceKm + depthKm * depthKm));
    const double s = 2.0 * magnitude - 4.68 * std::log10(r) - 0.007 * r - 1.66;
    if (s < 0.5) return "0";
    if (s < 1.5) return "1";
    if (s < 2.5) return "2";
    if (s < 3.5) return "3";
    if (s < 4.5) return "4";
    if (s < 5.0) return "5弱";
    if (s < 5.5) return "5强";
    if (s < 6.0) return "6弱";
    if (s < 6.5) return "6强";
    return "7";
}

std::string IntensityCalculator::description(double raw) {
    int level = static_cast<int>(std::lround(raw));
    if (level < 0) level = 0;
    switch (level) {
    case 0: return "无感，仪器仅可记录";
    case 1: return "极轻微，少数敏感人群可感";
    case 2: return "轻微，室内少数人有感，悬挂物微动";
    case 3: return "明显，室内多数人有感，门窗轻微作响";
    case 4: return "较强，室内普遍有感，器皿碰撞作响";
    case 5: return "强烈，室外多数人有感，轻微破坏可能出现";
    case 6: return "剧烈，多数人站立不稳，简易房屋可能出现破坏";
    case 7: return "破坏性，房屋出现破坏，地表可能出现裂缝";
    default: return "毁灭性，建筑物严重破坏，地形显著变形";
    }
}

} // namespace komira
