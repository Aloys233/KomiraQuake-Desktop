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

/// 在 [minRadius, maxRadius] 上从 maxOpacity 过渡到 minOpacity（对齐 kanameishi 的 calcOpacity）。
double rampedOpacity(double radius, double minRadius, double maxRadius, double minOpacity, double maxOpacity) {
    if (maxRadius <= minRadius) return radius <= minRadius ? maxOpacity : minOpacity;
    if (radius <= minRadius * 0.2 + maxRadius * 0.8) return maxOpacity;
    if (radius >= maxRadius) return minOpacity;
    const double k = 5.0 * (minOpacity - maxOpacity) / (maxRadius - minRadius);
    const double b = (5.0 * maxOpacity * maxRadius - 4.0 * minOpacity * maxRadius - minOpacity * minRadius) /
                     (maxRadius - minRadius);
    return k * radius + b;
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

double IntensityCalculator::distanceForCsis(double magnitude, double depthKm, double level) {
    if (magnitude <= 0.0) return 0.0;
    if (rawCsis(magnitude, 0.0, depthKm) < level) return 0.0;
    // rawCsis 关于距离单调不增，二分求交；量程上限对齐走时表 10000 km。
    double lo = 0.0;
    double hi = 10000.0;
    for (int i = 0; i < 40; ++i) {
        const double mid = (lo + hi) / 2.0;
        if (rawCsis(magnitude, mid, depthKm) >= level) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

double IntensityCalculator::waveOpacity(double radiusKm, double fadeKm, double hardMaxKm) {
    if (radiusKm <= 0.0 || fadeKm <= 0.0 || radiusKm >= hardMaxKm) return 0.0;
    return radiusKm <= fadeKm ? rampedOpacity(radiusKm, 0.0, fadeKm, 0.25, 1.0)
                              : rampedOpacity(radiusKm, fadeKm, hardMaxKm, 0.0, 0.25);
}

double IntensityCalculator::waveFillOpacity(double radiusKm, double fadeKm) {
    // 与描边不同：填充只画在影响半径内（与 kanameishi 的 sWaveFill 一致），且由 0.25 递减到 0。
    if (radiusKm <= 0.0 || fadeKm <= 0.0 || radiusKm > fadeKm) return 0.0;
    return rampedOpacity(radiusKm, 0.0, fadeKm, 0.0, 0.25);
}

int IntensityCalculator::displayLevel(double raw) {
    return std::clamp(static_cast<int>(std::lround(raw)), 0, kRomanCount - 1);
}

std::string IntensityCalculator::formatCsis(double raw) {
    return kRoman[displayLevel(raw)];
}

double IntensityCalculator::jmaValue(double magnitude, double distanceKm, double depthKm) {
    const double r = std::max(1e-6, std::sqrt(distanceKm * distanceKm + depthKm * depthKm));
    return 2.0 * magnitude - 4.68 * std::log10(r) - 0.007 * r - 1.66;
}

namespace {
/// JMA 分档表：上界、显示文本、级数。formatJma 与 jmaLevel 共用这一张表，
/// 保证「显示成什么」与「过滤按几级比较」不可能分叉。
/// 5弱/5强 同为 5 级、6弱/6强 同为 6 级（过滤不区分强弱，与阈值滑块的 0.5 步长无关）。
struct JmaBand {
    double upper;
    const char* text;
    int level;
};
constexpr JmaBand kJmaBands[] = {
    {0.5, "0", 0},   {1.5, "1", 1},   {2.5, "2", 2},   {3.5, "3", 3},   {4.5, "4", 4},
    {5.0, "5弱", 5}, {5.5, "5强", 5}, {6.0, "6弱", 6}, {6.5, "6强", 6},
};
constexpr int kJmaMaxLevel = 7;
} // namespace

int IntensityCalculator::jmaLevel(double magnitude, double distanceKm, double depthKm) {
    const double s = jmaValue(magnitude, distanceKm, depthKm);
    for (const auto& band : kJmaBands) {
        if (s < band.upper) return band.level;
    }
    return kJmaMaxLevel;
}

double IntensityCalculator::displayedLevel(double magnitude, double rawCsis, double distanceKm,
                                           double depthKm, IntensityStandard standard) {
    return standard == IntensityStandard::Jma
               ? static_cast<double>(jmaLevel(magnitude, distanceKm, depthKm))
               : static_cast<double>(displayLevel(rawCsis));
}

std::string IntensityCalculator::formatJma(double magnitude, double distanceKm, double depthKm) {
    const double s = jmaValue(magnitude, distanceKm, depthKm);
    for (const auto& band : kJmaBands) {
        if (s < band.upper) return band.text;
    }
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
