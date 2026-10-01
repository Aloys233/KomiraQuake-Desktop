#pragma once

#include <utility>

namespace komira {

/// WGS-84 <-> GCJ-02 纠偏。《NATIVE_PORT_SPEC》 §4.5。
class CoordinateTransform {
public:
    // 中国范围包围盒，与 GCJ-02 适用边界一致；也用作波前是否已离开中国的判定范围。
    static constexpr double kChinaMinLat = 0.8293;
    static constexpr double kChinaMaxLat = 55.8271;
    static constexpr double kChinaMinLon = 72.004;
    static constexpr double kChinaMaxLon = 137.8347;

    static std::pair<double, double> wgs84ToGcj02(double lat, double lng);
    static std::pair<double, double> gcj02ToWgs84(double lat, double lng);

private:
    static bool outOfChina(double lat, double lng);
    static double transformLat(double x, double y);
    static double transformLng(double x, double y);
};

} // namespace komira
