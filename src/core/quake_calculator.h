#pragma once

#include <utility>

namespace komira {

/// 《NATIVE_PORT_SPEC》 §4.1 距离与常量速度。
class QuakeCalculator {
public:
    static constexpr double kPWaveSpeed = 6.0;
    static constexpr double kSWaveSpeed = 3.5;
    static constexpr double kEarthRadiusKm = 6371.0;

    /// 同一地震判定窗口：发震时刻相差不超过 30 s 且震中相距不超过 25 km。
    static constexpr long long kSameQuakeWindowMs = 30'000;
    static constexpr double kSameQuakeRadiusKm = 25.0;

    static double haversineDistance(double lat1, double lon1, double lat2, double lon2);
    static double hypocenterDistance(double epicentralKm, double depthKm);
    static std::pair<double, double> estimateTravelTimes(double epicentralKm, double depthKm);
    static bool isValidCoordinate(double lat, double lon);
    static double elapsedSeconds(long long originEpochMs, long long nowEpochMs);

    /// 两条报次（实时预警 / 目录）是否描述同一次地震：发震时刻接近且震中邻近。
    static bool isSameQuake(long long timestampA, double latA, double lonA,
                            long long timestampB, double latB, double lonB);
};

} // namespace komira
