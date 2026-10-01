#include "core/travel_table.h"

#include <algorithm>
#include <cmath>

namespace komira {

TravelTable::TravelTable(std::vector<double> depths,
                         std::vector<double> distances,
                         std::vector<std::vector<double>> pTimes,
                         std::vector<std::vector<double>> sTimes)
    : depths_(std::move(depths)),
      distances_(std::move(distances)),
      pTimes_(std::move(pTimes)),
      sTimes_(std::move(sTimes)) {}

TravelTable::Bracket TravelTable::bracket(const std::vector<double>& values, double v) {
    if (values.empty()) return {};
    if (v <= values.front()) return {0, 0, 0.0};
    if (v >= values.back()) return {static_cast<int>(values.size()) - 1,
                                    static_cast<int>(values.size()) - 1, 0.0};

    int lo = 0;
    while (lo + 1 < static_cast<int>(values.size()) && values[static_cast<size_t>(lo) + 1] < v) ++lo;
    const int hi = lo + 1;
    const double span = values[static_cast<size_t>(hi)] - values[static_cast<size_t>(lo)];
    const double w = span <= 0.0 ? 0.0 : (v - values[static_cast<size_t>(lo)]) / span;
    return {lo, hi, w};
}

double TravelTable::lerp(double a, double b, double w) { return a + (b - a) * w; }

double TravelTable::interpRow(const std::vector<double>& distances,
                              const std::vector<double>& times,
                              double d) {
    if (times.empty()) return 0.0;
    const Bracket b = bracket(distances, d);
    return lerp(times[static_cast<size_t>(b.lo)], times[static_cast<size_t>(b.hi)], b.w);
}

double TravelTable::invertRow(const std::vector<double>& distances,
                              const std::vector<double>& times,
                              double seconds) {
    if (times.empty()) return 0.0;
    if (seconds <= times.front()) return distances.front();
    if (seconds >= times.back()) return distances.back();

    size_t i = 0;
    while (i + 1 < times.size() && times[i + 1] < seconds) ++i;
    const double span = times[i + 1] - times[i];
    const double w = span <= 0.0 ? 0.0 : (seconds - times[i]) / span;
    return lerp(distances[i], distances[i + 1], w);
}

double TravelTable::estimateP(double depthKm, double distanceKm) const {
    const Bracket db = bracket(depths_, depthKm);
    const double lo = interpRow(distances_, pTimes_[static_cast<size_t>(db.lo)], distanceKm);
    const double hi = interpRow(distances_, pTimes_[static_cast<size_t>(db.hi)], distanceKm);
    return lerp(lo, hi, db.w);
}

double TravelTable::estimateS(double depthKm, double distanceKm) const {
    const Bracket db = bracket(depths_, depthKm);
    const double lo = interpRow(distances_, sTimes_[static_cast<size_t>(db.lo)], distanceKm);
    const double hi = interpRow(distances_, sTimes_[static_cast<size_t>(db.hi)], distanceKm);
    return lerp(lo, hi, db.w);
}

double TravelTable::distanceForTime(double depthKm, double seconds, bool isPWave) const {
    const Bracket db = bracket(depths_, depthKm);
    const double lo = invertRow(distances_,
                                isPWave ? pTimes_[static_cast<size_t>(db.lo)]
                                        : sTimes_[static_cast<size_t>(db.lo)],
                                seconds);
    const double hi = invertRow(distances_,
                                isPWave ? pTimes_[static_cast<size_t>(db.hi)]
                                        : sTimes_[static_cast<size_t>(db.hi)],
                                seconds);
    return lerp(lo, hi, db.w);
}

} // namespace komira
