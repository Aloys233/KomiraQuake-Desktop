#include "core/source_estimator.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>

#include "core/intensity_calculator.h"
#include "core/quake_calculator.h"

namespace komira {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinCosLat = 0.05;
/// 强度模型的近场软化半径（km）：与 CSIS 衰减关系的 +15 km 同量级，
/// 避免震中附近 log10(0) 发散。
constexpr double kNearDistanceKm = 15.0;
/// I(R) = a − c·log10(R + r0) 中的距离系数。4.5 与中国 CSIS 衰减关系的
/// −4.368·log10(D+15) 同量级（略陡，补偿没有距离方向项）。
constexpr double kLogDistanceCoefficient = 4.5;

double clamp(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }

/// 搜索内部的快算震中距（等距圆柱近似，km）。
///
/// 一次评估要算几十到上百个站，深度候选 × 极坐标扫描 × 模式搜索合计上万次评估；
/// 每次都用 atan2 的 Haversine 会让单次推算涨到几十毫秒。地震推算的搜索范围通常在
/// 数百公里内（触发站包络 + padding，上限 6°），该近似误差 < 1%，对定位的影响远小于
/// 强度本身的量化步长。**对外展示的距离仍用 Haversine**（见 estimate 结尾）。
inline double fastDistanceKm(double lat0, double lon0, double lat, double lon) {
    constexpr double kKmPerDeg = 111.32;
    const double meanLat = 0.5 * (lat0 + lat);
    const double dy = (lat - lat0) * kKmPerDeg;
    const double dx = (lon - lon0) * kKmPerDeg *
                      std::cos(meanLat * kPi / 180.0);
    return std::sqrt(dx * dx + dy * dy);
}

double huber(double residual, double delta) {
    const double a = std::fabs(residual);
    return a <= delta ? 0.5 * residual * residual : delta * (a - 0.5 * delta);
}

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    const std::size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<long>(mid), values.end());
    const double hi = values[mid];
    if (values.size() % 2 == 1) return hi;
    std::nth_element(values.begin(), values.begin() + static_cast<long>(mid - 1), values.end());
    return 0.5 * (values[mid - 1] + hi);
}

/// 方位角（度，0 = 正北，顺时针）。
double bearingDegrees(double fromLat, double fromLon, double toLat, double toLon) {
    const double p = kPi / 180.0;
    const double y = std::sin((toLon - fromLon) * p) * std::cos(toLat * p);
    const double x = std::cos(fromLat * p) * std::sin(toLat * p) -
                     std::sin(fromLat * p) * std::cos(toLat * p) * std::cos((toLon - fromLon) * p);
    double deg = std::atan2(y, x) / p;
    if (deg < 0.0) deg += 360.0;
    return deg;
}

/// 触发站抽稀：按网格每格只留最强站，直到不超上限。
/// 保证空间分布不被近场密集台网吃掉（近场几百站会把远场信号权重压没）。
std::vector<StationObservation> decimate(const std::vector<StationObservation>& stations,
                                         int limit) {
    if (limit <= 0 || static_cast<int>(stations.size()) <= limit) return stations;
    double cell = 0.1;
    while (cell <= 20.0) {
        // std::map 保序：同一输入永远得到同一组站，便于回归对比。
        std::map<long long, StationObservation> cells;
        for (const StationObservation& s : stations) {
            const long long key = static_cast<long long>(std::floor(s.latitude / cell)) * 100000LL +
                                  static_cast<long long>(std::floor(s.longitude / cell));
            const auto found = cells.find(key);
            if (found == cells.end()) {
                cells.emplace(key, s);
            } else if (s.intensity > found->second.intensity) {
                found->second = s;
            }
        }
        if (static_cast<int>(cells.size()) <= limit) {
            std::vector<StationObservation> kept;
            kept.reserve(cells.size());
            for (const auto& entry : cells) kept.push_back(entry.second);
            return kept;
        }
        cell *= 1.6;
    }
    // 极端情况（上万站铺满全球）：直接按强度降序截断，保证有界。
    std::vector<StationObservation> sorted = stations;
    std::sort(sorted.begin(), sorted.end(), [](const StationObservation& a,
                                               const StationObservation& b) {
        return a.intensity > b.intensity;
    });
    sorted.resize(static_cast<std::size_t>(limit));
    return sorted;
}

struct Candidate {
    double loss = std::numeric_limits<double>::infinity();
    double latitude = 0.0;
    double longitude = 0.0;
    double intercept = 0.0;
    double rms = 0.0;
};

/// 单个候选位置在给定深度下的损失。
///
/// 正证据：I_obs + c·log10(R + r0) 应在所有触发站上相等（= 截距 a）。
/// 用 Huber 残差拟合 a（中位数起算，再迭代两轮加权），得到稳健的平移不变损失。
/// 负证据：预测强度超过「触发阈值 + 余量」的未触发站计入惩罚 —— 这是 HINET 那类
/// 「尚未触发也是观测」的删失思想，只是这里没有到时，只能按强度上界使用。
Candidate score(const std::vector<StationObservation>& triggered,
                const std::vector<StationObservation>& quiet,
                double latitude, double longitude, double depthKm,
                const SourceEstimatorConfig& config, double depthPenalty) {
    Candidate candidate;
    candidate.latitude = latitude;
    candidate.longitude = longitude;

    const std::size_t n = triggered.size();
    std::vector<double> corrected;
    corrected.reserve(n);
    for (const StationObservation& s : triggered) {
        const double epicentral = fastDistanceKm(latitude, longitude, s.latitude, s.longitude);
        const double hypo = std::sqrt(epicentral * epicentral + depthKm * depthKm);
        corrected.push_back(s.intensity +
                            kLogDistanceCoefficient * std::log10(hypo + kNearDistanceKm));
    }
    double intercept = median(corrected);
    for (int iteration = 0; iteration < 2; ++iteration) {
        double sum = 0.0;
        double weight = 0.0;
        for (double value : corrected) {
            const double r = value - intercept;
            const double w = std::fabs(r) <= config.robustDelta ? 1.0
                                                               : config.robustDelta / std::fabs(r);
            sum += value * w;
            weight += w;
        }
        if (weight > 0.0) intercept = sum / weight;
    }
    candidate.intercept = intercept;

    double loss = 0.0;
    double square = 0.0;
    for (double value : corrected) {
        const double r = value - intercept;
        loss += huber(r, config.robustDelta);
        square += r * r;
    }
    loss /= static_cast<double>(n);
    candidate.rms = std::sqrt(square / static_cast<double>(n));

    // 无梯度保护：所有站强度几乎一样时，任何位置都能把残差压到 0，此时解没有意义，
    // 交给上层按 rms + 置信度判断（loss 仍返回，但会被 gradient 检查拦下）。
    if (config.useQuietStations && !quiet.empty()) {
        double quietLoss = 0.0;
        int counted = 0;
        for (const StationObservation& s : quiet) {
            const double epicentral = fastDistanceKm(latitude, longitude, s.latitude, s.longitude);
            if (epicentral > config.quietRadiusKm) continue;
            const double hypo = std::sqrt(epicentral * epicentral + depthKm * depthKm);
            const double predicted =
                intercept - kLogDistanceCoefficient * std::log10(hypo + kNearDistanceKm);
            // 预测强度必须明显超过门槛才算矛盾；把超出量截断，避免个别坏站主导搜索结果。
            const double excess =
                std::min(predicted - (config.triggerLevel + config.quietMargin), 2.0);
            if (excess > 0.0) {
                quietLoss += excess * excess;
                ++counted;
            }
        }
        if (counted > 0) {
            // 归一化到「每站均摊、再按正证据站数折回」，与正证据同一量纲。
            loss += config.quietWeight * (quietLoss / static_cast<double>(counted)) /
                    static_cast<double>(n);
        }
    }
    candidate.loss = loss + depthPenalty;
    return candidate;
}

/// 深度先验：离散深度里偏向浅源。地震目录中浅源占多数，且深源证据在强度场上
/// 几乎无法与「远一点的浅源」区分；没有先验时搜索会系统性地把深度推到候选边角。
double depthPenalty(double depthKm, const SourceEstimatorConfig& config) {
    if (config.depthPriorWeight <= 0.0) return 0.0;
    const double normalized = std::log10(std::max(1.0, depthKm)) / std::log10(150.0);
    return config.depthPriorWeight * normalized;
}

/// 极坐标粗扫：以触发站加权质心为极点，环数与环向点数受配置约束。
/// 返回扫描到的最优候选（尚未细化）；`outRadiusDeg` 回填外层实际用的扫描半径，
/// 供 `refineLocation` 用同一个圆盘做边界（两个盘一致，粗扫的边界才有意义）。
Candidate scanLocation(const std::vector<StationObservation>& triggered,
                       const std::vector<StationObservation>& quiet, double depthKm,
                       const SourceEstimatorConfig& config, double* outRadiusDeg = nullptr) {
    // 加权质心：强度越高权重越大（截断，避免单个饱和站拉偏）。
    double weightSum = 0.0;
    double latSum = 0.0;
    double lonSum = 0.0;
    for (const StationObservation& s : triggered) {
        const double w = clamp(s.intensity - config.triggerLevel + 0.5, 0.05, 20.0);
        weightSum += w;
        latSum += s.latitude * w;
        lonSum += s.longitude * w;
    }
    const double centerLat = latSum / weightSum;
    const double centerLon = lonSum / weightSum;

    double minLat = triggered.front().latitude;
    double maxLat = minLat;
    double minLon = triggered.front().longitude;
    double maxLon = minLon;
    for (const StationObservation& s : triggered) {
        minLat = std::min(minLat, s.latitude);
        maxLat = std::max(maxLat, s.latitude);
        minLon = std::min(minLon, s.longitude);
        maxLon = std::max(maxLon, s.longitude);
    }
    const double extent = std::max(maxLat - minLat, maxLon - minLon);
    const double radius = clamp(extent + config.searchPaddingDeg, config.minSearchRadiusDeg,
                                config.maxSearchRadiusDeg);
    if (outRadiusDeg) *outRadiusDeg = radius;

    // 环向点数按周长与步长求；总点数受 maxScanPoints 约束（大范围时自动降密）。
    const double penalty = depthPenalty(depthKm, config);
    int ringPoints =
        std::max(4, static_cast<int>(std::ceil(2.0 * kPi * radius /
                                               std::max(0.02, config.scanStepDeg))));
    ringPoints = std::min(ringPoints, 360);
    const int rings = std::max(1, config.scanRings);
    while ((rings + 1) * ringPoints > std::max(120, config.maxScanPoints) && ringPoints > 8) {
        ringPoints = std::max(8, ringPoints * 3 / 4);
    }

    Candidate best;
    for (int ring = 0; ring <= rings; ++ring) {
        const double r = radius * static_cast<double>(ring) / rings;
        const int points = ring == 0 ? 1 : ringPoints;
        for (int index = 0; index < points; ++index) {
            const double angle = 2.0 * kPi * static_cast<double>(index) / points;
            const double lat = centerLat + r * std::sin(angle);
            const double lon = centerLon + r * std::cos(angle) /
                                            std::max(kMinCosLat, std::cos(centerLat * kPi / 180.0));
            if (lat < -85.0 || lat > 85.0 || lon < -180.0 || lon > 180.0) continue;
            const Candidate candidate =
                score(triggered, quiet, lat, lon, depthKm, config, penalty);
            if (candidate.loss < best.loss) best = candidate;
        }
    }
    return best;
}

/// 模式搜索细化：单步最优方向 + 失败折半。比再扫一层细网格便宜，且不会被粗网格锁死。
///
/// `boundRadiusDeg` 是粗扫用的极坐标半径。细化**必须**受同一个半径约束：模式搜索是
/// 单调下坡，而强度残差这个目标在「离所有触发站都很远」的方向上会单调变好（见
/// `solveCluster` 里远端退化检测的注释），没有边界时细化会一路走到几百公里外的海上，
/// 给出一个残差极低、却离所有台站都上千公里的解。
Candidate refineLocation(const std::vector<StationObservation>& triggered,
                         const std::vector<StationObservation>& quiet, Candidate best,
                         double depthKm, double initialStepDeg, double boundRadiusDeg,
                         const SourceEstimatorConfig& config) {
    const double penalty = depthPenalty(depthKm, config);
    double step = std::max(initialStepDeg, config.refineFloorDeg);
    // 细化允许走的范围 = 粗扫圆盘（以同一加权质心为心）。粗扫本身就在这个盘里选点，
    // 细化若不受同一约束，粗扫的边界就形同虚设。
    double centerLat = 0.0;
    double centerLon = 0.0;
    {
        double weightSum = 0.0;
        double latSum = 0.0;
        double lonSum = 0.0;
        for (const StationObservation& s : triggered) {
            const double w = clamp(s.intensity - config.triggerLevel + 0.5, 0.05, 20.0);
            weightSum += w;
            latSum += s.latitude * w;
            lonSum += s.longitude * w;
        }
        centerLat = latSum / weightSum;
        centerLon = lonSum / weightSum;
    }
    const double limitKm = boundRadiusDeg * 111.32;
    for (int iteration = 0; iteration < config.maxRefineSteps; ++iteration) {
        bool improved = false;
        const double cosLat = std::max(kMinCosLat, std::cos(best.latitude * kPi / 180.0));
        const double dLat = step;
        const double dLon = step / cosLat;
        const double moves[8][2] = {{dLat, 0.0},   {-dLat, 0.0}, {0.0, dLon},   {0.0, -dLon},
                                    {dLat, dLon},  {dLat, -dLon}, {-dLat, dLon}, {-dLat, -dLon}};
        Candidate moved;
        for (const auto& move : moves) {
            const double lat = best.latitude + move[0];
            const double lon = best.longitude + move[1];
            if (lat < -85.0 || lat > 85.0 || lon < -180.0 || lon > 180.0) continue;
            if (fastDistanceKm(centerLat, centerLon, lat, lon) > limitKm) continue;
            const Candidate candidate = score(triggered, quiet, lat, lon, depthKm, config, penalty);
            if (candidate.loss < moved.loss) moved = candidate;
        }
        if (moved.loss < best.loss - 1e-12) {
            best = moved;
            improved = true;
        }
        if (!improved) {
            step *= 0.5;
            if (step < config.refineFloorDeg) break;
        }
    }
    return best;
}

/// 方位角最大空隙（度）。用于判断触发的测站是否只覆盖震中的一侧。
double azimuthGap(double latitude, double longitude,
                  const std::vector<StationObservation>& stations) {
    if (stations.empty()) return 360.0;
    if (stations.size() == 1) return 360.0;
    std::vector<double> bearings;
    bearings.reserve(stations.size());
    for (const StationObservation& s : stations) {
        if (s.latitude == latitude && s.longitude == longitude) continue;
        bearings.push_back(bearingDegrees(latitude, longitude, s.latitude, s.longitude));
    }
    if (bearings.size() < 2) return 360.0;
    std::sort(bearings.begin(), bearings.end());
    double largest = 0.0;
    for (std::size_t i = 1; i < bearings.size(); ++i) {
        largest = std::max(largest, bearings[i] - bearings[i - 1]);
    }
    largest = std::max(largest, 360.0 - bearings.back() + bearings.front());
    return largest;
}

} // namespace

const char* intensityFamilyId(IntensityFamily family) {
    switch (family) {
    case IntensityFamily::Jma: return "jma";
    case IntensityFamily::Kma: return "kma";
    case IntensityFamily::Csis: break;
    }
    return "csis";
}

const char* intensityFamilyLabel(IntensityFamily family) {
    switch (family) {
    case IntensityFamily::Jma: return "計測震度";
    case IntensityFamily::Kma: return "韩国 MMI";
    case IntensityFamily::Csis: break;
    }
    return "中国烈度";
}

namespace {

/// 台湾框：本岛 + 澎湖 + 兰屿/绿岛一带。金门马祖不在此框内（离大陆台网更近，
/// 归中国区不会把两片触发区串错）。
///
/// 只用这一个框做地理判定，其余区域靠**制式**认，是因为制式本来就是更强的证据：
/// NIED/S-Net 上报 JMA 計測震度、KMA 上报 MMI、Pancakes/Wolfx 上报 CSIS，几乎
/// 一对一。用矩形去切日本海 / 黄海 / 东海反而会把辽宁、山东沿海的站误判进日本区。
constexpr double kTaiwanMinLat = 21.5;
constexpr double kTaiwanMaxLat = 25.6;
constexpr double kTaiwanMinLon = 119.3;
constexpr double kTaiwanMaxLon = 122.2;

inline bool inTaiwanBox(double latitude, double longitude) {
    return latitude >= kTaiwanMinLat && latitude <= kTaiwanMaxLat
           && longitude >= kTaiwanMinLon && longitude <= kTaiwanMaxLon;
}

/// 同一制式内的「中国烈度等效」换算。与 `SourceEstimationService` 原先的
/// familyTrigger 同一组系数（CSIS 3 ≈ JMA 2.5 ≈ MMI 4），只是搬进纯 C++ 层，
/// 让设置页、服务层与单测共用一份定义。
constexpr double kJmaScale = 0.75;
constexpr double kJmaOffset = 0.2;
constexpr double kMmiOffset = 1.0;

} // namespace

const std::array<SourceRegion, 4>& sourceRegions() {
    static const std::array<SourceRegion, 4> kRegions = {
        SourceRegion::China, SourceRegion::Japan, SourceRegion::Korea, SourceRegion::Taiwan};
    return kRegions;
}

const char* sourceRegionId(SourceRegion region) {
    switch (region) {
    case SourceRegion::Japan: return "japan";
    case SourceRegion::Korea: return "korea";
    case SourceRegion::Taiwan: return "taiwan";
    case SourceRegion::China: break;
    }
    return "china";
}

const char* sourceRegionLabel(SourceRegion region) {
    switch (region) {
    case SourceRegion::Japan: return "日本";
    case SourceRegion::Korea: return "韩国";
    case SourceRegion::Taiwan: return "台湾";
    case SourceRegion::China: break;
    }
    return "中国";
}

IntensityFamily sourceRegionFamily(SourceRegion region) {
    switch (region) {
    case SourceRegion::Japan: return IntensityFamily::Jma;
    case SourceRegion::Korea: return IntensityFamily::Kma;
    // 台湾目前只有 Wolfx SeisJS 的 CSIS 测站；将来接入 CWA（震度与 JMA 同形）时，
    // 该区的 JMA 观测会自动单独成桶参与拟合，默认阈值口径仍是 CSIS。
    case SourceRegion::Taiwan:
    case SourceRegion::China: break;
    }
    return IntensityFamily::Csis;
}

bool sourceRegionFromId(const std::string& id, SourceRegion* out) {
    for (const SourceRegion region : sourceRegions()) {
        if (id == sourceRegionId(region)) {
            if (out) *out = region;
            return true;
        }
    }
    return false;
}

bool sourceRegionFromId(const char* id, SourceRegion* out) {
    if (!id) return false;
    return sourceRegionFromId(std::string(id), out);
}

const SourceRegionDefaults& sourceRegionDefaults(SourceRegion region) {
    // 触发阈值都是「该区主导制式」下的数值，不是中国烈度：
    //   中国 CSIS 3.0 / 日本 JMA 2.5 / 韩国 MMI 4.0 / 台湾 CSIS 3.0。
    // 三者的物理量级相同（CSIS 3 ≈ JMA 2.5 ≈ MMI 4），只是各自台网的常用刻度不同。
    static const SourceRegionDefaults kChina{3.0, 6, 250, true};
    // 日本台网：门槛对齐 kanameishi 的 `minInferenceStationCount = 5`（不是按台网密度估）。
    // 参考实现压噪声靠的是**邻域联动 + 着未着惩罚**，不是抬高站数门槛。
    static const SourceRegionDefaults kJapan{2.5, 5, 200, true};
    // 韩国 KMA 约 260 站、间距更大：门槛比中国低一档，合并半径略小（半岛尺度）。
    static const SourceRegionDefaults kKorea{4.0, 5, 200, true};
    // 台湾目前只有 Wolfx SeisJS 的零星测站（CWA 台网尚未接入），门槛给 1 站：
    // 先给「单站粗解」，由卡片标注 provisional，接上台网后用户可自行调高。
    static const SourceRegionDefaults kTaiwan{3.0, 1, 250, true};
    switch (region) {
    case SourceRegion::Japan: return kJapan;
    case SourceRegion::Korea: return kKorea;
    case SourceRegion::Taiwan: return kTaiwan;
    case SourceRegion::China: break;
    }
    return kChina;
}

double csisEquivalent(IntensityFamily family, double value) {
    if (!std::isfinite(value)) return value;
    switch (family) {
    case IntensityFamily::Jma: return (value - kJmaOffset) / kJmaScale;
    case IntensityFamily::Kma: return value - kMmiOffset;
    case IntensityFamily::Csis: break;
    }
    return value;
}

double intensityFromCsis(IntensityFamily family, double csis) {
    if (!std::isfinite(csis)) return csis;
    switch (family) {
    case IntensityFamily::Jma: return std::round((csis * kJmaScale + kJmaOffset) * 10.0) / 10.0;
    case IntensityFamily::Kma: return std::round((csis + kMmiOffset) * 10.0) / 10.0;
    case IntensityFamily::Csis: break;
    }
    return csis;
}

bool sourceRegionForStation(IntensityFamily family, double latitude, double longitude,
                            SourceRegion* out) {
    if (!std::isfinite(latitude) || !std::isfinite(longitude)) return false;
    // 区域几乎完全由**数据源**决定（每路的台网只覆盖一个国家），坐标只用来做一个例外：
    // 台湾。台湾的站目前混在 Wolfx SeisJS（CSIS）里，将来接入 CWA 后会混进 JMA 制式，
    // 两种制式都得先过台湾框，才能在接入前后都被单独算成「台湾区」。
    //
    // 刻意不按「国家多边形」逐站判国界：矩形框一旦拿来切东海/黄海，就会把辽宁、山东
    // 沿海的 CSIS 站划进日本区，或把朝鲜半岛的站划进中国区 —— 那是比现在更糟的错误。
    const bool taiwan = latitude >= kTaiwanMinLat && latitude <= kTaiwanMaxLat
                        && longitude >= kTaiwanMinLon && longitude <= kTaiwanMaxLon;
    if (taiwan && family != IntensityFamily::Kma) {
        if (out) *out = SourceRegion::Taiwan;
        return true;
    }
    switch (family) {
    case IntensityFamily::Kma:
        // KMA 只有韩国台网一路；框只用来剔除明显的坏坐标（例如 (0,0) 的占位站）。
        if (latitude < 32.0 || latitude > 39.5 || longitude < 123.0 || longitude > 131.5) {
            return false;
        }
        if (out) *out = SourceRegion::Korea;
        return true;
    case IntensityFamily::Jma:
        // NIED Kmoni / S-Net 覆盖日本列岛与周边海域。
        if (latitude < 20.0 || latitude > 46.5 || longitude < 122.0 || longitude > 154.0) {
            return false;
        }
        if (out) *out = SourceRegion::Japan;
        return true;
    case IntensityFamily::Csis:
        break;
    }
    // 中国台网（含 PancakesSeis / Wolfx SeisJS 的国内站）。框只做坏坐标剔除，
    // 不做国界判定：台湾已在上面单独取走。
    if (latitude < 3.0 || latitude > 54.5 || longitude < 73.0 || longitude > 135.5) {
        return false;
    }
    if (out) *out = SourceRegion::China;
    return true;
}

bool SourceEstimator::usableIntensity(IntensityFamily family, double intensity) {
    if (!std::isfinite(intensity)) return false;
    switch (family) {
    case IntensityFamily::Jma:
        // JMA 計測震度：缺测 -3，饱和 99（S-Net 上游哨兵）。
        return intensity > -2.5 && intensity < 90.0;
    case IntensityFamily::Kma:
        // 韩国 MMI：缺测 -3，有效 0–10。
        return intensity > -2.5 && intensity <= 10.5;
    case IntensityFamily::Csis:
        break;
    }
    // 中国烈度：缺测 -1，有效 0–12。
    return intensity > -0.5 && intensity <= 12.5;
}

double SourceEstimator::predictedIntensity(IntensityFamily family, double magnitude,
                                           double epicentralKm, double depthKm) {
    if (magnitude <= 0.0) return 0.0;
    const double hypo = std::sqrt(std::max(0.0, epicentralKm) * std::max(0.0, epicentralKm) +
                                  std::max(0.0, depthKm) * std::max(0.0, depthKm));
    switch (family) {
    case IntensityFamily::Jma: {
        // 对齐 IntensityCalculator::jmaValue 的 PGV 型关系（r 用震源距）。
        const double r = std::max(1e-6, hypo);
        return 2.0 * magnitude - 4.68 * std::log10(r) - 0.007 * r - 1.66;
    }
    case IntensityFamily::Kma:
        // 韩国 MMI 没有公开衰减式；MMI 与 CSIS 在中低烈度段接近，按 CSIS 近似。
        return IntensityCalculator::rawCsis(magnitude, std::max(0.0, epicentralKm), depthKm);
    case IntensityFamily::Csis:
        break;
    }
    return IntensityCalculator::rawCsis(magnitude, std::max(0.0, epicentralKm), depthKm);
}

/// 单簇求解：给定一簇触发站与（已按距离过滤的）未触发站，反演出一个震源。
///
/// 这是原来 `estimate()` 的全部内容，抽出来是为了支持「先分簇、再对每簇单独求解」：
/// 两场地震同时发生时，把两簇强度塞进同一个拟合会得到一个**位于两者之间、并不存在**的
/// 震中（而且残差可能仍然很小，看起来还很"可信"）。
///
/// 返回的 `valid == true` 表示这一簇算出了可用的解；是否发布由调用方按门槛判定。
namespace {

SourceEstimate solveCluster(const std::vector<StationObservation>& triggered,
                            const std::vector<StationObservation>& quiet,
                            const SourceEstimatorConfig& config) {
    SourceEstimate result;
    result.family = config.family;
    result.region = config.region;

    // 触发站必须有强度梯度：全站同一个值说明还没分出远近，此时任何位置都"最优"。
    double minIntensity = triggered.front().intensity;
    double maxIntensity = minIntensity;
    for (const StationObservation& s : triggered) {
        minIntensity = std::min(minIntensity, s.intensity);
        maxIntensity = std::max(maxIntensity, s.intensity);
    }
    if (maxIntensity - minIntensity < 0.5) return result;

    result.stationCount = static_cast<int>(triggered.size());
    const std::vector<StationObservation> used =
        decimate(triggered, std::max(3, config.maxStations));
    result.usedStationCount = static_cast<int>(used.size());
    const std::vector<StationObservation> quietUsed =
        config.useQuietStations ? decimate(quiet, std::max(0, config.maxQuietStations))
                                : std::vector<StationObservation>{};
    result.quietCount = static_cast<int>(quietUsed.size());

    // 深度：只做离散候选。强度场对深度不敏感（远场站几乎分不出「深一点」与「远一点」），
    // 连续扫会过拟合，故先用粗扫挑深度，再只对该深度做一次位置细化。
    Candidate best;
    double bestDepth = config.depthCandidatesKm.empty() ? 10.0 : config.depthCandidatesKm.front();
    double scanRadiusDeg = config.minSearchRadiusDeg;
    for (double depth : config.depthCandidatesKm) {
        if (depth < 0.0) continue;
        const Candidate candidate = scanLocation(used, quietUsed, depth, config, &scanRadiusDeg);
        if (candidate.loss < best.loss) {
            best = candidate;
            bestDepth = depth;
        }
    }
    if (!std::isfinite(best.loss)) return result;
    // 初始步长取粗扫最内环的半径量级；细化会自行折半。
    const double coarseStep = std::max(config.refineFloorDeg,
                                       std::min(config.scanStepDeg, 0.25));
    best = refineLocation(used, quietUsed, best, bestDepth, coarseStep, scanRadiusDeg, config);
    result.depthKm = bestDepth;

    result.latitude = best.latitude;
    result.longitude = best.longitude;
    result.rmsIntensity = best.rms;

    double nearest = std::numeric_limits<double>::infinity();
    double distanceSum = 0.0;
    for (const StationObservation& s : used) {
        const double d = QuakeCalculator::haversineDistance(best.latitude, best.longitude,
                                                            s.latitude, s.longitude);
        nearest = std::min(nearest, d);
        distanceSum += d;
    }
    result.nearestStationKm = std::isfinite(nearest) ? nearest : 0.0;
    result.meanDistanceKm = distanceSum / static_cast<double>(used.size());
    result.azimuthGapDeg = azimuthGap(best.latitude, best.longitude, used);

    // 远端退化：解被推到离所有触发站都上千公里的地方时，强度残差会「变好」——
    // 所有站的距离几乎相等，log 项就成了一个常数平移，Huber 残差自然很小。
    // 那不是震源，是目标函数的一个**退化极小**（实测：川滇两片触发区被单链聚类串成
    // 一簇喂进来，解会跑到青海 1187 km 外，M 反演出 8.9，残差反而从 1.59 降到 0.93）。
    //
    // 判据用「最近的触发站也比重证据半径还远」：本服务的触发站都在陆地上、震中就在
    // 有感区内部，一个离所有触发站都超过 `quietRadiusKm`（默认 400 km）的解一定不是
    // 真的震源 —— 那是上面那条退化链。此时该簇判为无解，服务层保留上一帧的解。
    if (result.nearestStationKm > config.quietRadiusKm) return result;
    result.valid = true;

    // 边界贴靠：最佳解落在搜索框（触发站包络 + padding）的一次扫描步长内。
    double minLat = used.front().latitude;
    double maxLat = minLat;
    double minLon = used.front().longitude;
    double maxLon = minLon;
    for (const StationObservation& s : used) {
        minLat = std::min(minLat, s.latitude);
        maxLat = std::max(maxLat, s.latitude);
        minLon = std::min(minLon, s.longitude);
        maxLon = std::max(maxLon, s.longitude);
    }
    const double margin = config.scanStepDeg;
    result.boundaryHit = best.latitude <= minLat - config.searchPaddingDeg + margin
                         || best.latitude >= maxLat + config.searchPaddingDeg - margin
                         || best.longitude <= minLon - config.searchPaddingDeg + margin
                         || best.longitude >= maxLon + config.searchPaddingDeg - margin;

    // 震级：不拿强度模型的截距外推（那是 log 近似的产物，量纲对不上衰减关系），
    // 而是直接按**该制式的衰减关系**对全部触发站做一维最小二乘：
    // 找到让 Huber(观测 − predictedIntensity(M, 距离, 深度)) 之和最小的 M。
    // 这样震级与地图/预警使用的烈度公式严格同源。
    {
        std::vector<double> distances;
        std::vector<double> intensities;
        distances.reserve(used.size());
        intensities.reserve(used.size());
        for (const StationObservation& s : used) {
            distances.push_back(QuakeCalculator::haversineDistance(best.latitude, best.longitude,
                                                                   s.latitude, s.longitude));
            intensities.push_back(s.intensity);
        }
        const auto lossAt = [&](double magnitude) {
            double loss = 0.0;
            for (std::size_t i = 0; i < distances.size(); ++i) {
                const double predicted = SourceEstimator::predictedIntensity(
                    config.family, magnitude, distances[i], result.depthKm);
                loss += huber(intensities[i] - predicted, config.robustDelta);
            }
            return loss;
        };
        // 黄金分割：loss 关于 M 近似凸（预测强度随 M 单调增、Huber 凸）。
        double lo = 0.0;
        double hi = 9.5;
        const double phi = 0.6180339887498949;
        double x1 = hi - phi * (hi - lo);
        double x2 = lo + phi * (hi - lo);
        double f1 = lossAt(x1);
        double f2 = lossAt(x2);
        for (int i = 0; i < 40; ++i) {
            if (f1 < f2) {
                hi = x2;
                x2 = x1;
                f2 = f1;
                x1 = hi - phi * (hi - lo);
                f1 = lossAt(x1);
            } else {
                lo = x1;
                x1 = x2;
                f1 = f2;
                x2 = lo + phi * (hi - lo);
                f2 = lossAt(x2);
            }
        }
        result.magnitude = std::round(0.5 * (lo + hi) * 10.0) / 10.0;
    }

    // 不确定度启发式：台站越少、残差越大、方位覆盖越差，水平误差越大。
    // 这不是概率校准值，卡片上按「±N km」展示，并明确标注为估计。
    const double coverage = clamp(result.azimuthGapDeg / 180.0, 0.75, 2.4);
    const double support = 1.0 / std::sqrt(static_cast<double>(used.size()));
    result.uncertaintyKm = clamp(result.meanDistanceKm * (0.25 + 0.5 * result.rmsIntensity) *
                                     support * coverage,
                                 2.0, 400.0);

    // 置信度启发式：台站数（对数饱和）+ 拟合（残差）+ 几何（方位空隙）。
    const double supportScore =
        clamp(std::log2(static_cast<double>(used.size()) / 3.0) / 3.0, 0.0, 1.0);
    const double fitScore = clamp(1.0 - result.rmsIntensity / 1.2, 0.0, 1.0);
    const double geometryScore = clamp(1.0 - result.azimuthGapDeg / 300.0, 0.0, 1.0);
    result.confidence =
        clamp(0.10 + 0.38 * supportScore + 0.33 * fitScore + 0.19 * geometryScore, 0.0, 0.96);

    if (result.confidence >= 0.80 && used.size() >= 12) result.grade = "S";
    else if (result.confidence >= 0.66) result.grade = "A";
    else if (result.confidence >= 0.50) result.grade = "B";
    else if (result.confidence >= 0.34) result.grade = "C";
    else result.grade = "D";

    if (!(result.magnitude > 0.0) || !std::isfinite(result.magnitude)) {
        result.magnitude = 0.0;
    }
    return result;
}

/// 按地理邻近度把触发站分成若干簇（单链接 / 并查集）。
///
/// 为什么必须分簇：**同时**发生两场地震时（例如四川一次、云南一次），两簇强度会被
/// 当成"一场大地震"的强度场来拟合，最优解落在两簇之间 —— 那是一个不存在的震中，而且
/// 因为每簇内部仍满足衰减规律，残差可能仍然很小。按「任意两站距离 > 半径就切断」分簇，
/// 可以保证发布出来的震源只由**一片连通的触发区**支撑。
///
/// 单链接（而非 k-means）是刻意的：一场地震的有感区本身是连成一片的，只要相邻测站
/// 距离不超过半径，整片就会连成一簇，不需要预先知道有几场地震。
std::vector<std::vector<StationObservation>> clusterByProximity(
    const std::vector<StationObservation>& stations, double radiusKm) {
    const std::size_t n = stations.size();
    std::vector<std::size_t> parent(n);
    for (std::size_t i = 0; i < n; ++i) parent[i] = i;
    const std::function<std::size_t(std::size_t)> find = [&](std::size_t i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            if (QuakeCalculator::haversineDistance(stations[i].latitude, stations[i].longitude,
                                                   stations[j].latitude,
                                                   stations[j].longitude) <= radiusKm) {
                const std::size_t a = find(i);
                const std::size_t b = find(j);
                if (a != b) parent[a] = b;
            }
        }
    }
    // 用 map 汇总（按根首次出现顺序），结果与输入顺序稳定对应，便于回归对比。
    std::vector<std::vector<StationObservation>> clusters;
    std::map<std::size_t, std::size_t> rootToIndex;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t root = find(i);
        auto found = rootToIndex.find(root);
        if (found == rootToIndex.end()) {
            rootToIndex.emplace(root, clusters.size());
            clusters.emplace_back();
            clusters.back().push_back(stations[i]);
        } else {
            clusters[found->second].push_back(stations[i]);
        }
    }
    return clusters;
}

/// 取一簇的包络中心与对角半径（度），用于筛掉离这簇太远的未触发站。
void clusterBounds(const std::vector<StationObservation>& cluster, double* centerLat,
                   double* centerLon, double* radiusDeg) {
    double minLat = cluster.front().latitude;
    double maxLat = minLat;
    double minLon = cluster.front().longitude;
    double maxLon = minLon;
    for (const StationObservation& s : cluster) {
        minLat = std::min(minLat, s.latitude);
        maxLat = std::max(maxLat, s.latitude);
        minLon = std::min(minLon, s.longitude);
        maxLon = std::max(maxLon, s.longitude);
    }
    *centerLat = 0.5 * (minLat + maxLat);
    *centerLon = 0.5 * (minLon + maxLon);
    *radiusDeg = 0.5 * std::hypot(maxLat - minLat, maxLon - minLon);
}

/// 给定观测强度与假定的震源距，按该制式的衰减关系反解震级。
///
/// 预测强度关于震级单调递增，直接二分即可。只用于 1–2 站这类「距离未知」的退化路径：
/// 正常路径的震级是对全部触发站做一维拟合，不需要假设距离。
double magnitudeForIntensity(IntensityFamily family, double intensity, double epicentralKm,
                             double depthKm) {
    double lo = 0.0;
    double hi = 9.5;
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (SourceEstimator::predictedIntensity(family, mid, epicentralKm, depthKm) < intensity) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return std::round(clamp(0.5 * (lo + hi), 1.0, 9.5) * 10.0) / 10.0;
}

/// 弱证据解：只有 1–2 个触发站时的退化路径。
///
/// 强度场需要**梯度**才能约束距离：一站时任何震源位置都能拟合出同样的残差（0），
/// 两站时也只能约束「谁更近」，不能定出方位。因此这里不做反演，而是给一个**诚实的粗解**：
///   1. 初始位置 = 最强触发站（它一定离震源不远，是位置的唯一硬信息）；
///   2. 用未触发站做一条**方向性**推论：若附近未触发的站都集中在某一侧，说明那一侧
///      离震源更远，震源应当在触发站的反侧 —— 据此微调一小步；
///   3. 给一个与多点解明显不同量级的不确定度，并置 `provisional`，置信度封顶到 D 级。
///
/// 刻意不做「多轮迭代把位置收得更准」：在没有梯度的数据上反复优化只会产出一个
/// 看起来精确、其实没有依据的坐标。这条路径的价值只在波刚扫过第一两个站的早期。
SourceEstimate solveProvisionalCluster(const std::vector<StationObservation>& cluster,
                                       const std::vector<StationObservation>& quiet,
                                       const SourceEstimatorConfig& config) {
    SourceEstimate result;
    result.family = config.family;
    result.region = config.region;
    result.provisional = true;

    const StationObservation& anchor =
        *std::max_element(cluster.begin(), cluster.end(),
                          [](const StationObservation& a, const StationObservation& b) {
                              return a.intensity < b.intensity;
                          });
    result.latitude = anchor.latitude;
    result.longitude = anchor.longitude;
    result.depthKm = config.depthCandidatesKm.empty() ? 10.0 : config.depthCandidatesKm.front();
    result.stationCount = static_cast<int>(cluster.size());
    result.usedStationCount = result.stationCount;

    // 未触发站的方向性推论：取附近未触发站的质心，把解往其反方向挪一小步。
    // 步长上限 25 km —— 单站解本来就没有公里级精度，挪多了就是编造。
    double latSum = 0.0;
    double lonSum = 0.0;
    int counted = 0;
    for (const StationObservation& s : quiet) {
        if (QuakeCalculator::haversineDistance(anchor.latitude, anchor.longitude, s.latitude,
                                               s.longitude) > config.quietRadiusKm) {
            continue;
        }
        latSum += s.latitude;
        lonSum += s.longitude;
        ++counted;
    }
    result.quietCount = counted;
    if (counted >= 3) {
        const double centerLat = latSum / counted;
        const double centerLon = lonSum / counted;
        const double distance = QuakeCalculator::haversineDistance(
            anchor.latitude, anchor.longitude, centerLat, centerLon);
        if (distance > 1.0) {
            const double bearing = bearingDegrees(anchor.latitude, anchor.longitude, centerLat,
                                                  centerLon);
            // 挪动量 = min(25 km, 到未触发站质心距离的 20%)，方向与质心相反。
            const double shiftKm = std::min(25.0, distance * 0.2);
            const double shiftDeg = shiftKm / 111.32;
            const double backBearing = (bearing + 180.0) * kPi / 180.0;
            result.latitude = clamp(anchor.latitude + std::cos(backBearing) * shiftDeg, -85.0, 85.0);
            result.longitude = clamp(
                anchor.longitude + std::sin(backBearing) * shiftDeg /
                                       std::max(kMinCosLat, std::cos(anchor.latitude * kPi / 180.0)),
                -180.0, 180.0);
        }
    }

    double nearest = std::numeric_limits<double>::infinity();
    double distanceSum = 0.0;
    for (const StationObservation& s : cluster) {
        const double d = QuakeCalculator::haversineDistance(result.latitude, result.longitude,
                                                            s.latitude, s.longitude);
        nearest = std::min(nearest, d);
        distanceSum += d;
    }
    result.nearestStationKm = std::isfinite(nearest) ? nearest : 0.0;
    result.meanDistanceKm = distanceSum / static_cast<double>(cluster.size());
    result.azimuthGapDeg = azimuthGap(result.latitude, result.longitude, cluster);
    result.rmsIntensity = 0.0;

    // 震级：仍按该制式的衰减关系反解，但距离只能假设 —— 取「近场典型震源距」30 km。
    // 不能用 `I/1.6` 这类线性折算：实测 CSIS 下 I=5.2 既可能是 20 km 的 M5，也可能是
    // 100 km 的 M6.4，线性折算会系统性地低估 1.5 级以上。既然距离未知，就固定一个
    // 近场假设并**在卡片上标注为粗解**，比给一个偏差极大的数字诚实。
    result.magnitude = magnitudeForIntensity(config.family, anchor.intensity, 30.0,
                                             result.depthKm);
    // 不确定度只由**站数**决定，不掺入几何量：1–2 站时的几何量本身没有统计意义
    // （解被推到哪、站离多远都是任意的），用它算出来的"精度"会自相矛盾
    // （多一个约束反而更差）。这里按「每多一站更紧一档」给，与多点解的几公里
    // 明显区分，也保证单调性：1 站 150 km、2 站 70 km。
    result.uncertaintyKm = cluster.size() <= 1 ? 150.0 : 70.0;
    // 置信度封顶 0.30：永远落在 D 级，卡片与日志都不会把它当成正式解。
    result.confidence = clamp(0.10 + 0.10 * static_cast<double>(cluster.size()), 0.0, 0.30);
    result.grade = "D";
    result.valid = true;
    return result;
}

} // namespace

SourceEstimate SourceEstimator::estimate(const std::vector<StationObservation>& observations,
                                         const SourceEstimatorConfig& config) {
    SourceEstimate result;
    result.family = config.family;
    result.region = config.region;

    std::vector<StationObservation> triggered;
    std::vector<StationObservation> quiet;
    triggered.reserve(observations.size());
    quiet.reserve(observations.size());
    for (const StationObservation& s : observations) {
        if (!QuakeCalculator::isValidCoordinate(s.latitude, s.longitude)) continue;
        if (!usableIntensity(config.family, s.intensity)) continue;
        if (s.intensity >= config.triggerLevel) triggered.push_back(s);
        else quiet.push_back(s);
    }
    // 门槛下限是 1：测站数小于 1 就没什么可算的。
    const int publishFloor = std::max(1, config.minStations);
    const int searchFloor = std::max(1, std::min(config.minSearchStations, publishFloor));
    if (static_cast<int>(triggered.size()) < 1) return result;
    if (static_cast<int>(triggered.size()) < searchFloor) return result;

    // 分簇：每簇单独求解，绝不把相距很远的两片触发区拟合成一个中间震中。
    const double clusterRadius = std::max(1.0, config.clusterRadiusKm);
    const std::vector<std::vector<StationObservation>> clusters =
        clusterByProximity(triggered, clusterRadius);
    result.clusterCount = static_cast<int>(clusters.size());

    SourceEstimate best;
    double bestKey = -1.0;
    int publishable = 0;
    for (const std::vector<StationObservation>& cluster : clusters) {
        if (static_cast<int>(cluster.size()) < searchFloor) continue;
        // 负证据只取这一簇附近的未触发站：远处的站对本地震源没有约束力，
        // 而且它可能是另一簇的"已触发站"，拿来做反证会直接把解推开。
        double centerLat = 0.0;
        double centerLon = 0.0;
        double radiusDeg = 0.0;
        clusterBounds(cluster, &centerLat, &centerLon, &radiusDeg);
        const double limitKm = config.quietRadiusKm + radiusDeg * 111.32;
        std::vector<StationObservation> clusterQuiet;
        clusterQuiet.reserve(quiet.size());
        for (const StationObservation& s : quiet) {
            if (QuakeCalculator::haversineDistance(centerLat, centerLon, s.latitude, s.longitude) <=
                limitKm) {
                clusterQuiet.push_back(s);
            }
        }
        SourceEstimate candidate;
        if (cluster.size() >= 3) {
            candidate = solveCluster(cluster, clusterQuiet, config);
        } else {
            // 1–2 站：强度场没有梯度，衰减拟合无解。仍按用户要求给出一个**可用的粗略解**
            // ——以触发站为初始点、只用未触发站的删失约束收紧位置，并明确标记 provisional。
            candidate = solveProvisionalCluster(cluster, clusterQuiet, config);
        }
        if (!candidate.valid) continue;
        const bool canPublish = static_cast<int>(cluster.size()) >= publishFloor;
        if (canPublish) ++publishable;
        // 选簇规则：优先「够得着发布门槛」的簇，其次比置信度，再比支撑站数。
        // 用组合键而不是先筛后比，是为了在全部簇都不够门槛时仍能报出最有希望的那个。
        //
        // 迟滞（`preferRadiusKm > 0`）：落在上一帧解附近的簇额外加成 `preferBonus`。
        // 两片可比的有感区并存时，两簇的置信度只差千分之几，逐帧噪声就足以让最强簇
        // 每次换人。加成本身不改变「谁更可信」的排序，只把「势均力敌」判成「保持原解」。
        double affinity = 0.0;
        if (config.preferRadiusKm > 0.0 && canPublish) {
            const double d = QuakeCalculator::haversineDistance(
                config.preferLatitude, config.preferLongitude, candidate.latitude,
                candidate.longitude);
            if (d <= config.preferRadiusKm) affinity = config.preferBonus;
        }
        const double key = (canPublish ? 1000.0 : 0.0) + candidate.confidence * 100.0
                           + affinity * 100.0
                           + std::min(99.0, static_cast<double>(cluster.size()) / 10.0);
        if (key > bestKey) {
            bestKey = key;
            best = candidate;
        }
    }
    if (bestKey < 0.0) return result;

    best.clusterCount = result.clusterCount;
    // 除发布簇之外，还有几簇也达到了门槛：卡片据此提示"另有独立触发"。
    best.otherClusterCount = std::max(0, publishable - (best.stationCount >= publishFloor ? 1 : 0));
    // 发布门槛：触发站数不足时不对外发布（上层据此清空/不显示）。
    if (best.stationCount < publishFloor) best.valid = false;
    return best;
}

// ---------------------------------------------------------------------------
// 日本区到时推算（ArrivalEstimator）
//
// 与上面的强度场路径并列、互不影响：强度路径回答「哪里更近」，到时路径回答
// 「各站差了几秒、因此差了几公里」。两者的输入不同（强度 vs 首次越阈时刻），
// 所以不能混在一个目标函数里 —— 混起来会让强震站既拉位置又拉到时。
// ---------------------------------------------------------------------------
namespace {

/// 单站在某候选震源下的相位解释。
struct PhaseMatch {
    char wave = 'O';       ///< 'P' / 'S' / 'O'(两相都不像) / 'L'(权重 0)
    long long originMs = 0;///< 由该站该相反推的发震时刻
    double residual = 0.0; ///< 与当前发震时刻的残差（秒）
    double weight = 0.0;   ///< 0 表示不参与
};

/// 到时权重：强震站更可信；只触发一点点（刚到阈值）的站到时容易被噪声带偏。
double arrivalWeight(const ArrivalObservation& observation) {
    const double strength = std::max(observation.maxIntensity, observation.intensity);
    if (strength >= 4.0) return std::min(0.2 * strength, 2.0);   // 与 kanameishi 同式
    if (strength >= 3.0) return 0.3;
    if (strength >= 2.0) return 0.1;
    return 0.02;   // 仍然参与，但几乎不拉动解
}

/// 加权中位数（用于发震时刻初值：中位数抗离群，比均值稳）。
long long weightedMedianOrigin(std::vector<std::pair<long long, double>> values) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    double total = 0.0;
    for (const auto& entry : values) total += entry.second;
    double running = 0.0;
    for (const auto& entry : values) {
        running += entry.second;
        if (running >= total * 0.5) return entry.first;
    }
    return values.back().first;
}

/// 给定候选震源，求出最优发震时刻与代价。
///
/// 关键点：每个站既可能被解释为 P 也可能被解释为 S。固定位置/深度后，
/// 每站的两个相位各自给出一个 origin 候选；取与当前 origin 更接近的那个相位
/// （kanameishi 的 `selectClosestOption`）。origin 本身用「加权中位数起算 + 两轮加权均值」
/// 收敛 —— 目标函数关于 origin 是分段二次的，这个迭代足够稳且不需要求导。
struct ArrivalFit {
    long long originMs = 0;
    double loss = std::numeric_limits<double>::infinity();
    double rmsSeconds = 0.0;
    int pCount = 0;
    int sCount = 0;
    int explained = 0;
    double weightedMass = 0.0;
    /// 着未着惩罚：被判为「本该已到波却毫无反应」的未触发站数。
    int penalizedQuiet = 0;
    /// 正证据残差项（秒²/站），与 quietLoss 分开保留以便诊断与限幅。
    double residualTerm = 0.0;
};

/// 「着未着」惩罚的参考震中距。
///
/// kanameishi 的做法（`getInactivePenaltyReferenceDistance`）：取**强触发站**
/// （`maxAscend >= 3 && maxLevel >= 5`）的震中距，按 `penaltyReferenceQuantile`
/// 取分位值；没有强触发站时退化为最近站的距离。这里照做，只是判据换成强度。
double quietReferenceDistanceKm(const std::vector<ArrivalObservation>& stations, double lat,
                                double lon) {
    std::vector<double> distances;
    distances.reserve(stations.size());
    for (const ArrivalObservation& station : stations) {
        const double strength = std::max(station.maxIntensity, station.intensity);
        if (strength < 3.0) continue;   // 优先只用强站做参考
        distances.push_back(
            QuakeCalculator::haversineDistance(lat, lon, station.latitude, station.longitude));
    }
    if (distances.empty()) {
        // 没有强站：退化为**全部触发站**的距离分位（不是最近站）。
        // 用最近站在稀疏/弱震时会得到一个只有几公里的参考半径，罚项形同虚设。
        for (const ArrivalObservation& station : stations) {
            distances.push_back(QuakeCalculator::haversineDistance(lat, lon, station.latitude,
                                                                   station.longitude));
        }
    }
    if (distances.empty()) return 0.0;
    std::sort(distances.begin(), distances.end());
    // 第 `referenceQuantile` 分位（kanameishi 用 0.9）。下标取 floor(n·q)−1 并夹到合法范围：
    // n 很小时不能退化成 0，否则「比参考站更近」的判据会把所有未触发站都排除掉。
    const int index = std::clamp(static_cast<int>(std::floor(distances.size() * 0.9)) - 1, 0,
                                 static_cast<int>(distances.size()) - 1);
    return distances[static_cast<std::size_t>(index)];
}

ArrivalFit fitAt(const std::vector<ArrivalObservation>& stations, double lat, double lon,
                 double depthKm, const ArrivalEstimatorConfig& config,
                 const ArrivalEstimator::TravelTimes& travelTimes,
                 const std::vector<std::pair<double, double>>* quiet = nullptr,
                 long long nowMs = 0) {
    ArrivalFit fit;
    const std::size_t n = stations.size();
    if (n == 0) return fit;

    struct StationPhase {
        long long pOrigin = 0;
        long long sOrigin = 0;
        double weight = 0.0;
        bool hasP = false;
        bool hasS = false;
    };
    std::vector<StationPhase> phases(n);

    for (std::size_t i = 0; i < n; ++i) {
        const ArrivalObservation& station = stations[i];
        const double distance = QuakeCalculator::haversineDistance(lat, lon, station.latitude,
                                                                   station.longitude);
        StationPhase& phase = phases[i];
        phase.weight = arrivalWeight(station);
        if (phase.weight <= 0.0) continue;
        const double pSeconds = travelTimes.pSeconds(depthKm, distance);
        const double sSeconds = travelTimes.sSeconds(depthKm, distance);
        if (std::isfinite(pSeconds) && pSeconds > 0.0) {
            phase.pOrigin = station.onsetEpochMs - static_cast<long long>(std::llround(pSeconds * 1000.0));
            phase.hasP = true;
        }
        if (std::isfinite(sSeconds) && sSeconds > 0.0) {
            phase.sOrigin = station.onsetEpochMs - static_cast<long long>(std::llround(sSeconds * 1000.0));
            phase.hasS = true;
        }
    }

    // 初值：所有「P 候选」的加权中位数。P 是大多数站的真实相位，
    // 用它的中位数起算可以避免被少数被当成 S 的站拉偏。
    std::vector<std::pair<long long, double>> pOrigins;
    pOrigins.reserve(n);
    for (const StationPhase& phase : phases) {
        if (phase.hasP) pOrigins.emplace_back(phase.pOrigin, phase.weight);
    }
    long long origin = weightedMedianOrigin(pOrigins);
    if (origin == 0) return fit;

    // 迭代：每轮按当前 origin 给每站选相位、算残差、用 Huber 权重重估 origin。
    for (int iteration = 0; iteration < 8; ++iteration) {
        double sum = 0.0;
        double mass = 0.0;
        for (const StationPhase& phase : phases) {
            if (phase.weight <= 0.0) continue;
            bool useP = phase.hasP;
            long long candidate = phase.pOrigin;
            if (phase.hasP && phase.hasS) {
                const long long pDiff = std::llabs(phase.pOrigin - origin);
                const long long sDiff = std::llabs(phase.sOrigin - origin);
                useP = pDiff <= sDiff;
                candidate = useP ? phase.pOrigin : phase.sOrigin;
            } else if (!phase.hasP && phase.hasS) {
                useP = false;
                candidate = phase.sOrigin;
            }
            (void)useP;
            const double residual = static_cast<double>(candidate - origin) / 1000.0;
            const double absResidual = std::fabs(residual);
            const double robust = absResidual <= config.robustDeltaSeconds
                                      ? 1.0
                                      : config.robustDeltaSeconds / absResidual;
            sum += static_cast<double>(candidate) * phase.weight * robust;
            mass += phase.weight * robust;
        }
        if (mass <= 0.0) break;
        origin = static_cast<long long>(std::llround(sum / mass));
    }

    fit.originMs = origin;
    double square = 0.0;
    double loss = 0.0;
    double mass = 0.0;
    for (const StationPhase& phase : phases) {
        if (phase.weight <= 0.0) continue;
        char wave = 'O';
        long long candidate = 0;
        if (phase.hasP && phase.hasS) {
            const long long pDiff = std::llabs(phase.pOrigin - origin);
            const long long sDiff = std::llabs(phase.sOrigin - origin);
            wave = pDiff <= sDiff ? 'P' : 'S';
            candidate = wave == 'P' ? phase.pOrigin : phase.sOrigin;
        } else if (phase.hasP) {
            wave = 'P';
            candidate = phase.pOrigin;
        } else if (phase.hasS) {
            wave = 'S';
            candidate = phase.sOrigin;
        } else {
            continue;
        }
        const double residual = static_cast<double>(candidate - origin) / 1000.0;
        const double absResidual = std::fabs(residual);
        // 残差超过 3 倍 Huber 阈值视为「解释不了」：计入 O 相并停止拉动解。
        if (absResidual > config.robustDeltaSeconds * 3.0) {
            ++fit.explained;   // 仍然计数，但下面按 0 权重处理 RMS
            continue;
        }
        ++fit.explained;
        if (wave == 'P') ++fit.pCount;
        else ++fit.sCount;
        loss += huber(residual, config.robustDeltaSeconds) * phase.weight;
        square += residual * residual * phase.weight;
        mass += phase.weight;
    }
    if (mass <= 0.0) {
        fit.loss = std::numeric_limits<double>::infinity();
        return fit;
    }
    fit.weightedMass = mass;
    fit.residualTerm = loss / mass;
    fit.rmsSeconds = std::sqrt(square / mass);

    // 相位数惩罚（kanameishi `calcWaveCountPenalty` 的简化版）：S 明显多于 P 说明
    // 当前候选把「近处的 P」全解释成了「远处的 S」，这种解通常是位置被推到了台网外。
    const int pBase = std::max(1, fit.pCount);
    const double ratio = static_cast<double>(fit.sCount) / static_cast<double>(pBase);
    const double waveCountPenalty = std::clamp((ratio - 3.0) * 1.0, 0.0, 1.0);

    // 着未着惩罚。
    //
    // 语义（与 kotoho7 `@hyp:時間差リスト未着` / kanameishi `inactivePenalty` 同向）：
    // 候选震源预测「某未触发站此刻本该已经到波」，而它毫无反应 —— 这是**矛盾**，该罚。
    //
    // 但**不能直接数矛盾站**：把震源推到远处时，所有站都还没到波，矛盾数自然归零，
    // 目标函数就会偏爱一个「离台网很远、什么都没解释」的解（实测推到 550 km 外
    // 反而更优）。参考实现的做法是**与参考震中距比较**：只有「比参考站更近的
    // 未触发站」才算矛盾 —— 更近却没响才是真的说不通。这里照做。
    if (config.quietPenalty.enabled && quiet && !quiet->empty() && nowMs > 0) {
        const double referenceKm = quietReferenceDistanceKm(stations, lat, lon);
        int penalized = 0;
        int nearby = 0;
        for (const auto& station : *quiet) {
            const double distance =
                QuakeCalculator::haversineDistance(lat, lon, station.first, station.second);
            if (referenceKm > 0.0 && distance > referenceKm) continue;
            ++nearby;
            const double pSeconds = travelTimes.pSeconds(depthKm, distance);
            if (!std::isfinite(pSeconds) || pSeconds <= 0.0) continue;
            if (static_cast<double>(fit.originMs) / 1000.0 + pSeconds <
                static_cast<double>(nowMs) / 1000.0) {
                ++penalized;
            }
        }
        fit.penalizedQuiet = penalized;
        // 分母用**全部**未触发站，不用「参考半径内的」：后者会随候选外推而缩小，
        // 让「跑得越远、罚得越轻」—— 那等于奖励逃跑（实测解被推到 550 km 外）。
        // 用全量做分母，罚额就是「全体未触发站里有多大比例此刻本该已经响了」，
        // 与候选是否跑远无关，只与它解释得对不对有关。
        if (penalized > 0) {
            const double ratio =
                static_cast<double>(penalized) / static_cast<double>(quiet->size());
            fit.residualTerm += config.quietPenalty.maxRatio * ratio;
        }
    }

    fit.loss = fit.residualTerm + waveCountPenalty;
    return fit;
}

} // namespace

ArrivalEstimate ArrivalEstimator::estimate(
    const std::vector<ArrivalObservation>& observations, const ArrivalEstimatorConfig& config,
    const TravelTimes& travelTimes, const std::vector<std::pair<double, double>>& quietStations,
    long long nowMs) {
    ArrivalEstimate result;
    result.region = config.region;

    // 1) 过滤 + 同站去重（只保留最早的 onset：同一场地震一个站只该有一个 P 到时）。
    std::vector<ArrivalObservation> stations;
    stations.reserve(observations.size());
    for (const ArrivalObservation& observation : observations) {
        if (!QuakeCalculator::isValidCoordinate(observation.latitude, observation.longitude)) continue;
        if (observation.onsetEpochMs <= 0) continue;
        if (!std::isfinite(observation.intensity)) continue;
        stations.push_back(observation);
    }
    std::sort(stations.begin(), stations.end(),
              [](const ArrivalObservation& a, const ArrivalObservation& b) {
                  return a.onsetEpochMs < b.onsetEpochMs;
              });
    result.stationCount = static_cast<int>(stations.size());
    if (static_cast<int>(stations.size()) < std::max(1, config.minStations)) return result;

    // 2) 关联门：与最早的站相隔「太远 + 太晚」的站不属于同一场。
    //    判据与 kanameishi 的 `arePicksAssociated` 同式：|Δt| ≤ 距离/速度 + 余量。
    {
        const ArrivalObservation& anchor = stations.front();
        std::vector<ArrivalObservation> associated;
        associated.reserve(stations.size());
        associated.push_back(anchor);
        for (std::size_t i = 1; i < stations.size(); ++i) {
            const ArrivalObservation& candidate = stations[i];
            const double distance = QuakeCalculator::haversineDistance(
                anchor.latitude, anchor.longitude, candidate.latitude, candidate.longitude);
            const long long tolerance =
                static_cast<long long>(distance / std::max(0.5, config.associationVelocityKmps) * 1000.0)
                + config.associationPaddingMs;
            if (std::llabs(candidate.onsetEpochMs - anchor.onsetEpochMs) <= tolerance) {
                associated.push_back(candidate);
            }
        }
        stations.swap(associated);
        result.stationCount = static_cast<int>(stations.size());
        result.clusterCount = 1;
        if (static_cast<int>(stations.size()) < std::max(1, config.minStations)) return result;
    }

    // 2b) 邻域联动门槛：参考实现都不是「数到 N 个互不相干的站越阈就发布」。
    //
    //     kanameishi 要求某站 6 个邻居里过半激活且邻域 activity 过表；kotoho7 用
    //     「最近 7 点」做通常许可。这里取两者共同的内核：触发站里必须形成**至少一个
    //     邻居团**，且团里的站占全部触发站的相当比例。远场孤立强噪声站凑不出团。
    {
        std::vector<int> degree(stations.size(), 0);
        int clustered = 0;
        for (std::size_t i = 0; i < stations.size(); ++i) {
            for (std::size_t j = 0; j < stations.size(); ++j) {
                if (i == j) continue;
                if (QuakeCalculator::haversineDistance(
                        stations[i].latitude, stations[i].longitude, stations[j].latitude,
                        stations[j].longitude) <= config.neighborhood.radiusKm) {
                    ++degree[i];
                }
            }
            if (degree[i] + 1 >= config.neighborhood.minClusterStations) ++clustered;
        }
        const double ratio =
            stations.empty() ? 0.0
                             : static_cast<double>(clustered) / static_cast<double>(stations.size());
        // 站数本来就少于「一个邻域团」时（用户把门槛下调到 1–2 站的粗解路径），
        // 没有邻域可言，跳过这道门 —— 那条路径已经由 provisional 标注兜住了。
        const bool tooFewForNeighborhood =
            static_cast<int>(stations.size()) < config.neighborhood.minClusterStations;
        if (!tooFewForNeighborhood
            && (clustered == 0 || ratio < config.neighborhood.minClusteredRatio)) {
            return result;
        }
    }

    // 2c) 未触发站（着未着证据）。服务层已经把「没越过阈值」的日本区站传进来；
    //     它们不参与正证据，只用来否决「落在死寂区中央」的候选解。
    //     只保留候选震源「够得着」的站：全部传进来会让每候选多算几百次 Haversine，
    //     而且远处站本来就还没到波，不构成任何约束。
    // 只保留离触发站包络足够近的未触发站。上限取「触发区对角半径 + 200 km」：
    // 再远的站在任何合理候选下都还没到波，留着只是白算。
    std::vector<std::pair<double, double>> quiet;
    if (!quietStations.empty()) {
        double minLat = stations.front().latitude;
        double maxLat = minLat;
        double minLon = stations.front().longitude;
        double maxLon = minLon;
        for (const ArrivalObservation& station : stations) {
            minLat = std::min(minLat, station.latitude);
            maxLat = std::max(maxLat, station.latitude);
            minLon = std::min(minLon, station.longitude);
            maxLon = std::max(maxLon, station.longitude);
        }
        const double centerLat = 0.5 * (minLat + maxLat);
        const double centerLon = 0.5 * (minLon + maxLon);
        const double radiusKm =
            0.5 * QuakeCalculator::haversineDistance(minLat, minLon, maxLat, maxLon) + 200.0;
        quiet.reserve(quietStations.size());
        for (const auto& station : quietStations) {
            if (QuakeCalculator::haversineDistance(centerLat, centerLon, station.first,
                                                   station.second) <= radiusKm) {
                quiet.push_back(station);
            }
        }
    }

    // 3) 初始位置：触发站加权质心（强震站权重大），深度取候选里最浅的。
    double weightSum = 0.0;
    double latSum = 0.0;
    double lonSum = 0.0;
    for (const ArrivalObservation& station : stations) {
        const double weight = std::max(0.05, arrivalWeight(station));
        weightSum += weight;
        latSum += station.latitude * weight;
        lonSum += station.longitude * weight;
    }
    double bestLat = latSum / weightSum;
    double bestLon = lonSum / weightSum;

    // 4) 位置 + 深度联合搜索：外层逐级缩步（3° → 1° → 0.3° → 0.1° → …），
    //    内层在 8 个方向 + 原地取最优，深度在离散候选里同步挑。
    //    这就是 kanameishi `findBestHypocenter` 的 `createNeighborHypocenters` 结构。
    ArrivalFit bestFit;
    double bestDepth = config.depthCandidatesKm.empty() ? 10.0 : config.depthCandidatesKm.front();
    for (const double depth : config.depthCandidatesKm) {
        const ArrivalFit fit =
            fitAt(stations, bestLat, bestLon, depth, config, travelTimes, &quiet, nowMs);
        if (fit.loss < bestFit.loss) {
            bestFit = fit;
            bestDepth = depth;
        }
    }
    if (!std::isfinite(bestFit.loss)) return result;

    // 搜索范围：限制在**触发站包络外扩 initialStepDeg** 的框内。
    //
    // 参考实现的解之所以不会跑出台网，是因为它从「首站位置」起步、逐级缩步，天然
    // 被限制在局部；而坐标下降 + 未触发站惩罚如果允许无限外推，会出现「跑到远处 →
    // 所有未触发站都还没到波 → 罚项归零」的作弊解（实测被推到 550 km 外的海上）。
    // 显式框住之后，未触发站惩罚才只能在同一片区域里比较候选。
    double envMinLat = stations.front().latitude;
    double envMaxLat = envMinLat;
    double envMinLon = stations.front().longitude;
    double envMaxLon = envMinLon;
    for (const ArrivalObservation& station : stations) {
        envMinLat = std::min(envMinLat, station.latitude);
        envMaxLat = std::max(envMaxLat, station.latitude);
        envMinLon = std::min(envMinLon, station.longitude);
        envMaxLon = std::max(envMaxLon, station.longitude);
    }
    const double pad = std::max(config.initialStepDeg, 1.0);
    const double searchMinLat = envMinLat - pad;
    const double searchMaxLat = envMaxLat + pad;
    const double searchMinLon = envMinLon - pad;
    const double searchMaxLon = envMaxLon + pad;

    double step = config.initialStepDeg;
    const double cosLat = std::max(kMinCosLat, std::cos(bestLat * kPi / 180.0));
    for (int iteration = 0; iteration < config.maxIterations && step >= config.finalStepDeg;
         ++iteration) {
        bool improved = false;
        // 平地 8 邻域 + 原地；深度也一起试（上下各一档）。
        const double moves[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1},
                                    {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
        for (const auto& move : moves) {
            const double lat = bestLat + move[0] * step;
            const double lon = bestLon + move[1] * step / cosLat;
            if (lat < -85.0 || lat > 85.0) continue;
            if (lat < searchMinLat || lat > searchMaxLat) continue;
            if (lon < searchMinLon || lon > searchMaxLon) continue;
            for (const double depth : config.depthCandidatesKm) {
                const ArrivalFit fit =
                    fitAt(stations, lat, lon, depth, config, travelTimes, &quiet, nowMs);
                if (fit.loss < bestFit.loss) {
                    bestFit = fit;
                    bestLat = lat;
                    bestLon = lon;
                    bestDepth = depth;
                    improved = true;
                }
            }
        }
        if (!improved) step *= 0.5;
    }

    result.valid = true;
    result.latitude = bestLat;
    result.longitude = bestLon;
    result.depthKm = bestDepth;
    result.originEpochMs = bestFit.originMs;
    result.rmsSeconds = bestFit.rmsSeconds;
    result.usedStationCount = bestFit.explained;
    result.pCount = bestFit.pCount;
    result.sCount = bestFit.sCount;
    {
        const int total = std::max(1, bestFit.pCount + bestFit.sCount);
        // 相位覆盖：P 与 S 都被解释到时才算「覆盖完整」的一半。
        const double balance =
            std::min(bestFit.pCount, bestFit.sCount) / static_cast<double>(total);
        result.phaseCoverage = std::clamp(0.5 + balance, 0.0, 1.0);
    }

    // 5) 不确定度与置信度。到时反演的精度主要取决于**站数与相位一致性**，
    //    不像强度场那样依赖方位覆盖（到时本身就带方向信息）。
    const double support = 1.0 / std::sqrt(static_cast<double>(std::max(1, bestFit.explained)));
    result.uncertaintyKm = std::clamp(
        30.0 + 40.0 * bestFit.rmsSeconds * support * 10.0, 3.0, 400.0);
    result.uncertaintyDeg = result.uncertaintyKm / 111.32;
    {
        const double supportScore = std::clamp(
            std::log2(static_cast<double>(std::max(1, bestFit.explained)) / 3.0) / 3.0, 0.0, 1.0);
        const double fitScore = std::clamp(1.0 - bestFit.rmsSeconds / 6.0, 0.0, 1.0);
        const double coverageScore = std::clamp(result.phaseCoverage, 0.0, 1.0);
        result.confidence = std::clamp(
            0.15 + 0.40 * supportScore + 0.30 * fitScore + 0.15 * coverageScore, 0.0, 0.97);
    }
    result.penalizedQuietCount = bestFit.penalizedQuiet;

    // 质量分与定级：照抄参考实现的结构，而不是自己发明一套。
    //
    // kanameishi（`calcQualityScore` / `calcQualityRank`）：
    //   qualityScore = 3.8 + √(effectiveCount / 10) × 0.2 − score × 5 / 3
    //   等级 = 站数门槛（S 200 / A 100 / B 30 / C 10）与分数门槛（S 3 / A 2 / B 1 / C 0）
    //         **同时**满足的最高等级；qualityScore < −3 不显示。
    // 这里 score 用 `fit.loss`（残差项 + 相位/未着惩罚），effectiveCount 取
    // 「P/S 相位计数」与「去重站数」的均值 —— 与参考的 `(pickCount + stationCount)/2` 一致。
    {
        const int effectiveCount = std::max(1, (bestFit.pCount + bestFit.sCount
                                                + bestFit.explained) / 2);
        result.qualityScore =
            3.8 + std::sqrt(static_cast<double>(effectiveCount) / 10.0) * 0.2
            - bestFit.loss * 5.0 / 3.0;
        // 两个门槛必须**同时**满足（参考实现 `calcQualityRank` 用 `.find()` 取最高满足项）：
        // 站数够、且质量分够。质量分越大越好。
        const auto rankFor = [&](int minStations, double minScore) {
            return effectiveCount >= minStations && result.qualityScore >= minScore;
        };
        if (rankFor(config.rankMinStationsS, config.rankMinScoreS)) result.grade = "S";
        else if (rankFor(config.rankMinStationsA, config.rankMinScoreA)) result.grade = "A";
        else if (rankFor(config.rankMinStationsB, config.rankMinScoreB)) result.grade = "B";
        else if (rankFor(config.rankMinStationsC, config.rankMinScoreC)) result.grade = "C";
        else result.grade = "D";
        // 低于显示门槛：解仍给出（服务层会标 provisional），但等级一律 D。
        if (result.qualityScore < config.minDisplayedScore) {
            result.grade = "D";
            result.provisional = true;
        }
    }

    // 边界贴靠：最佳解离触发站包络超过 3 个初始步长，说明证据把解推到了台网外。
    {
        double minLat = stations.front().latitude;
        double maxLat = minLat;
        double minLon = stations.front().longitude;
        double maxLon = minLon;
        for (const ArrivalObservation& station : stations) {
            minLat = std::min(minLat, station.latitude);
            maxLat = std::max(maxLat, station.latitude);
            minLon = std::min(minLon, station.longitude);
            maxLon = std::max(maxLon, station.longitude);
        }
        const double margin = config.initialStepDeg;
        result.boundaryHit = bestLat <= minLat - margin || bestLat >= maxLat + margin
                             || bestLon <= minLon - margin || bestLon >= maxLon + margin;
    }

    // 6) 震级：到时反演只给位置/深度/发震时刻，不给震级。位置已定之后，
    //    震级就是对各站强度的一维拟合（与该制式的衰减关系严格同源），
    //    比强度路径的震级稳 —— 因为震中距不再是搜索变量。
    {
        std::vector<double> distances;
        std::vector<double> intensities;
        std::vector<double> weights;
        distances.reserve(stations.size());
        intensities.reserve(stations.size());
        weights.reserve(stations.size());
        for (const ArrivalObservation& station : stations) {
            if (!std::isfinite(station.intensity) || station.intensity < 0.0) continue;
            distances.push_back(QuakeCalculator::haversineDistance(bestLat, bestLon,
                                                                   station.latitude,
                                                                   station.longitude));
            // 到时路径落在 JMA 域；强度值同为 JMA 計測震度。
            intensities.push_back(station.intensity);
            weights.push_back(std::max(0.05, arrivalWeight(station)));
        }
        if (!distances.empty()) {
            const auto lossAt = [&](double magnitude) {
                double loss = 0.0;
                for (std::size_t i = 0; i < distances.size(); ++i) {
                    const double predicted = SourceEstimator::predictedIntensity(
                        IntensityFamily::Jma, magnitude, distances[i], bestDepth);
                    loss += huber(intensities[i] - predicted, config.robustDeltaSeconds) * weights[i];
                }
                return loss;
            };
            // 黄金分割：loss 关于 M 近似凸（预测强度随 M 单调增、Huber 凸）。
            double lo = 0.0;
            double hi = 9.5;
            const double phi = 0.6180339887498949;
            double x1 = hi - phi * (hi - lo);
            double x2 = lo + phi * (hi - lo);
            double f1 = lossAt(x1);
            double f2 = lossAt(x2);
            for (int i = 0; i < 40; ++i) {
                if (f1 < f2) {
                    hi = x2;
                    x2 = x1;
                    f2 = f1;
                    x1 = hi - phi * (hi - lo);
                    f1 = lossAt(x1);
                } else {
                    lo = x1;
                    x1 = x2;
                    f1 = f2;
                    x2 = lo + phi * (hi - lo);
                    f2 = lossAt(x2);
                }
            }
            result.magnitude = std::round(0.5 * (lo + hi) * 10.0) / 10.0;
        }
    }

    // 发布门槛：站数不足时降级为 provisional（保留解，但卡片必须标注）。
    if (bestFit.explained < std::max(1, config.publishMinStations)) {
        result.provisional = true;
        result.confidence = std::min(result.confidence, 0.32);
        result.grade = "D";
        result.uncertaintyKm = std::max(result.uncertaintyKm, 60.0);
        result.uncertaintyDeg = result.uncertaintyKm / 111.32;
    }
    return result;
}

SourceEstimate ArrivalEstimator::toSourceEstimate(const ArrivalEstimate& arrival) {
    SourceEstimate estimate;
    estimate.valid = arrival.valid;
    estimate.region = arrival.region;
    // 日本区是 JMA 制式；结果里的 intensity 字段语义仍是「同制式强度」。
    estimate.family = IntensityFamily::Jma;
    estimate.latitude = arrival.latitude;
    estimate.longitude = arrival.longitude;
    estimate.depthKm = arrival.depthKm;
    estimate.stationCount = arrival.stationCount;
    estimate.usedStationCount = arrival.usedStationCount;
    // 到时路径的「负证据」是**着未着**：本该已到波却没反应的站数。
    // 与强度路径的「未触发站反证」语义相近但判据不同，卡片按 method 区分措辞。
    estimate.quietCount = arrival.penalizedQuietCount;
    // rmsIntensity 在到时路径里装的是**到时残差（秒）**：卡片脚注会按 method 区分展示。
    estimate.rmsIntensity = arrival.rmsSeconds;
    estimate.azimuthGapDeg = 0.0;
    estimate.uncertaintyKm = arrival.uncertaintyKm;
    estimate.confidence = arrival.confidence;
    estimate.grade = arrival.grade;
    estimate.boundaryHit = arrival.boundaryHit;
    estimate.provisional = arrival.provisional;
    estimate.clusterCount = std::max(1, arrival.clusterCount);
    estimate.otherClusterCount = arrival.otherClusterCount;
    estimate.method = arrival.method;
    estimate.originEpochMs = arrival.originEpochMs;
    estimate.pPhaseCount = arrival.pCount;
    estimate.sPhaseCount = arrival.sCount;
    estimate.magnitude = arrival.magnitude;
    return estimate;
}

} // namespace komira
