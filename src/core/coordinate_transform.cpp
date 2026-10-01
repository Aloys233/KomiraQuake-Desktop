#include "core/coordinate_transform.h"

#include <cmath>

namespace komira {

namespace {
constexpr double kA = 6378245.0;
constexpr double kEe = 0.00669342162296594323;
constexpr double kPi = 3.14159265358979323846;
} // namespace

std::pair<double, double> CoordinateTransform::wgs84ToGcj02(double lat, double lng) {
    if (outOfChina(lat, lng)) return {lat, lng};

    double dLat = transformLat(lng - 105.0, lat - 35.0);
    double dLng = transformLng(lng - 105.0, lat - 35.0);
    const double radLat = lat / 180.0 * kPi;
    double magic = std::sin(radLat);
    magic = 1 - kEe * magic * magic;
    const double sqrtMagic = std::sqrt(magic);
    dLat = (dLat * 180.0) / ((kA * (1 - kEe)) / (magic * sqrtMagic) * kPi);
    dLng = (dLng * 180.0) / (kA / sqrtMagic * std::cos(radLat) * kPi);
    return {lat + dLat, lng + dLng};
}

std::pair<double, double> CoordinateTransform::gcj02ToWgs84(double lat, double lng) {
    if (outOfChina(lat, lng)) return {lat, lng};

    double dLat = 0.0;
    double dLng = 0.0;
    for (int i = 0; i < 3; ++i) {
        const auto [gLat, gLng] = wgs84ToGcj02(lat + dLat, lng + dLng);
        dLat += lat - gLat;
        dLng += lng - gLng;
    }
    return {lat + dLat, lng + dLng};
}

bool CoordinateTransform::outOfChina(double lat, double lng) {
    return lng < kChinaMinLon || lng > kChinaMaxLon || lat < kChinaMinLat || lat > kChinaMaxLat;
}

double CoordinateTransform::transformLat(double x, double y) {
    double ret = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * std::sqrt(std::fabs(x));
    ret += (20.0 * std::sin(6.0 * x * kPi) + 20.0 * std::sin(2.0 * x * kPi)) * 2.0 / 3.0;
    ret += (20.0 * std::sin(y * kPi) + 40.0 * std::sin(y / 3.0 * kPi)) * 2.0 / 3.0;
    ret += (160.0 * std::sin(y / 12.0 * kPi) + 320 * std::sin(y * kPi / 30.0)) * 2.0 / 3.0;
    return ret;
}

double CoordinateTransform::transformLng(double x, double y) {
    double ret = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * std::sqrt(std::fabs(x));
    ret += (20.0 * std::sin(6.0 * x * kPi) + 20.0 * std::sin(2.0 * x * kPi)) * 2.0 / 3.0;
    ret += (20.0 * std::sin(x * kPi) + 40.0 * std::sin(x / 3.0 * kPi)) * 2.0 / 3.0;
    ret += (150.0 * std::sin(x / 12.0 * kPi) + 300.0 * std::sin(x / 30.0 * kPi)) * 2.0 / 3.0;
    return ret;
}

} // namespace komira
