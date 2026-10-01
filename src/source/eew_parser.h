#pragma once

#include <QJsonObject>
#include <QString>
#include <optional>
#include <utility>

#include "core/intensity_calculator.h"
#include "model/earthquake_event.h"

namespace komira {

/// EEW / CENC 目录解析。《NATIVE_PORT_SPEC》 §2.3 / §2.4。
/// 定位为可选：无定位时不计算距离/烈度/走时，等级仅按震级判定。
class EewParser {
public:
    static constexpr double kDefaultDepth = 10.0;
    /// 距离未知的哨兵值（无定位）。
    static constexpr double kUnknownDistance = -1.0;

    using UserLocation = std::optional<std::pair<double, double>>;

    static std::optional<EarthquakeEvent> parse(const QJsonObject& obj,
                                                const UserLocation& user,
                                                IntensityStandard standard,
                                                const QString& sourceTitle,
                                                const QString& idPrefix,
                                                long long nowMs);

    static std::optional<EarthquakeEvent> parseCencDirectory(const QJsonObject& obj,
                                                             const UserLocation& user,
                                                             IntensityStandard standard,
                                                             long long nowMs);

    /// 有定位：烈度 + 震级联合判定。
    static WarningLevel deriveLevel(double rawIntensity, double magnitude, bool isCanceled);
    /// 无定位：仅按震级判定。
    static WarningLevel deriveLevelByMagnitude(double magnitude, bool isCanceled);
    static void recompute(EarthquakeEvent& event, const UserLocation& user,
                          IntensityStandard standard);
    static long long parseTime(const QString& raw, bool* ok = nullptr);
};

} // namespace komira
