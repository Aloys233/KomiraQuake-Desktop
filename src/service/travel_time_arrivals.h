#pragma once

#include "core/source_estimator.h"
#include "core/travel_time_service.h"

namespace komira {

/// 把 `TravelTimeService`（Qt 单例、JSON 资产）适配成 `ArrivalEstimator::TravelTimes`
/// （纯 C++ 抽象）。
///
/// 分成两层是刻意的：推算内核必须能在 `run_core_tests.sh` 的纯 g++ 路径里编译并测，
/// 不能为了读一张走时表就引入 Qt；而读表本身又只有 Qt 侧才有（资产在 qrc/可执行目录）。
///
/// 选表规则与 kanameishi 一致：震中距 ≤ `farFieldKm` 用 `jma2001`（日本列岛近场、
/// 深度 0–700 km），超出后改用 `jb`（全球远场）。两张表都在 `assets/travel_times.json` 里。
class ServiceTravelTimes final : public ArrivalEstimator::TravelTimes {
public:
    explicit ServiceTravelTimes(double farFieldKm = 2000.0) : farFieldKm_(farFieldKm) {}

    bool farField(double distanceKm) const override { return distanceKm > farFieldKm_; }

    double pSeconds(double depthKm, double distanceKm) const override {
        const TravelTimeService& service = TravelTimeService::instance();
        return service.estimate(depthKm, distanceKm, tableFor(distanceKm)).first;
    }

    double sSeconds(double depthKm, double distanceKm) const override {
        const TravelTimeService& service = TravelTimeService::instance();
        return service.estimate(depthKm, distanceKm, tableFor(distanceKm)).second;
    }

private:
    QString tableFor(double distanceKm) const {
        return farField(distanceKm) ? QStringLiteral("jb") : QStringLiteral("jma2001");
    }

    double farFieldKm_ = 2000.0;
};

} // namespace komira
