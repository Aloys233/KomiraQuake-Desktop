#include "core/quake_calculator.h"

#include <algorithm>
#include <cmath>

namespace komira {

namespace {
constexpr double kPi = 3.14159265358979323846;

bool finite(double v) { return std::isfinite(v); }
} // namespace

double QuakeCalculator::haversineDistance(double lat1, double lon1, double lat2, double lon2) {
    if (!finite(lat1) || !finite(lon1) || !finite(lat2) || !finite(lon2)) return 0.0;

    const double dLat = (lat2 - lat1) * kPi / 180.0;
    const double dLon = (lon2 - lon1) * kPi / 180.0;
    const double rLat1 = lat1 * kPi / 180.0;
    const double rLat2 = lat2 * kPi / 180.0;

    const double sinDLat = std::sin(dLat / 2.0);
    const double sinDLon = std::sin(dLon / 2.0);
    const double h = sinDLat * sinDLat + std::cos(rLat1) * std::cos(rLat2) * sinDLon * sinDLon;
    const double c = 2.0 * std::atan2(std::sqrt(h), std::sqrt(1.0 - h));
    return kEarthRadiusKm * c;
}

double QuakeCalculator::hypocenterDistance(double epicentralKm, double depthKm) {
    const double d = std::max(depthKm, 0.0);
    return std::sqrt(epicentralKm * epicentralKm + d * d);
}

std::pair<double, double> QuakeCalculator::estimateTravelTimes(double epicentralKm, double depthKm) {
    const double hypo = hypocenterDistance(epicentralKm, depthKm);
    return {hypo / kPWaveSpeed, hypo / kSWaveSpeed};
}

bool QuakeCalculator::isValidCoordinate(double lat, double lon) {
    return finite(lat) && finite(lon) && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

double QuakeCalculator::elapsedSeconds(long long originEpochMs, long long nowEpochMs) {
    return static_cast<double>(nowEpochMs - originEpochMs) / 1000.0;
}

bool QuakeCalculator::isSameQuake(long long timestampA, double latA, double lonA,
                                  long long timestampB, double latB, double lonB) {
    if (timestampA <= 0 || timestampB <= 0) return false;
    const long long delta = timestampA > timestampB ? timestampA - timestampB : timestampB - timestampA;
    if (delta > kSameQuakeWindowMs) return false;
    return haversineDistance(latA, lonA, latB, lonB) <= kSameQuakeRadiusKm;
}

} // namespace komira
