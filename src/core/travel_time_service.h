#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>

#include "core/travel_table.h"

namespace komira {

/// 走时表服务：加载 travel_times.json 并对外提供插值查询。《NATIVE_PORT_SPEC》 §4.2。
class TravelTimeService {
public:
    static TravelTimeService& instance();

    void loadFromJson(const QByteArray& raw);
    bool loadFromFile(const QString& path);
    bool isLoaded() const;

    static constexpr const char* kDefaultTable = "jma2001";

    std::pair<double, double> estimate(double depthKm,
                                       double distanceKm,
                                       const QString& table = QStringLiteral("jma2001")) const;
    double distanceForTime(double depthKm,
                           double seconds,
                           bool isPWave,
                           const QString& table = QStringLiteral("jma2001")) const;

private:
    TravelTimeService() = default;

    QHash<QString, TravelTable> tables_;
};

} // namespace komira
