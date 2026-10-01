#pragma once

#include <vector>

namespace komira {

/// P/S 走时表与双线性插值（Qt 无关，便于纯 C++ 单测）。《NATIVE_PORT_SPEC》 §4.2。
class TravelTable {
public:
    TravelTable() = default;
    TravelTable(std::vector<double> depths,
                std::vector<double> distances,
                std::vector<std::vector<double>> pTimes,
                std::vector<std::vector<double>> sTimes);

    bool valid() const { return !depths_.empty() && !distances_.empty(); }

    double estimateP(double depthKm, double distanceKm) const;
    double estimateS(double depthKm, double distanceKm) const;
    double distanceForTime(double depthKm, double seconds, bool isPWave) const;

private:
    struct Bracket {
        int lo = 0;
        int hi = 0;
        double w = 0.0;
    };

    static Bracket bracket(const std::vector<double>& values, double v);
    static double lerp(double a, double b, double w);
    static double interpRow(const std::vector<double>& distances,
                            const std::vector<double>& times,
                            double d);
    static double invertRow(const std::vector<double>& distances,
                            const std::vector<double>& times,
                            double seconds);

    std::vector<double> depths_;
    std::vector<double> distances_;
    std::vector<std::vector<double>> pTimes_;
    std::vector<std::vector<double>> sTimes_;
};

} // namespace komira
