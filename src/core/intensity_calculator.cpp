#include "core/intensity_calculator.h"

#include <algorithm>
#include <cmath>

namespace komira {

namespace {
const char* const kRoman[] = {
    "0", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI", "XII",
};
constexpr int kRomanCount = 13;
} // namespace

double IntensityCalculator::rawCsis(double magnitude, double distanceKm, double depthKm) {
    if (magnitude <= 0.0) return 0.0;
    const double r = std::sqrt(distanceKm * distanceKm + depthKm * depthKm);
    const double safeR = std::max(1.0, r);
    const double i = 0.92 + 1.63 * magnitude - 3.49 * std::log10(safeR + 7.0);
    return std::max(0.0, i);
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
