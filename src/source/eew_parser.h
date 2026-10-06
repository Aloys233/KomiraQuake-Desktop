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

    /// 源报最大烈度（数值 + 展示文本），供 EEW/目录/Pancakes 各解析器复用。
    struct MaxIntensityValue {
        double raw = 0.0;
        QString text;
    };

    /// 从 `MaxIntensity` / `epiIntensity` / `maxIntensity` / `intensity` 中取最大烈度。
    static MaxIntensityValue parseMaxIntensity(const QJsonObject& obj, IntensityStandard standard);

    /// originTimeIsJst：Wolfx 的 jma_eew 报文时刻是无时区的日本标准时间（JST, UTC+9）墙钟，
    /// 需按 JST 解析；中国各局报文时刻是本机所在时区（UTC+8）墙钟，保持本地解析。
    /// eventNamespace：eventId 的频道前缀（如 `jma_eew`），用于与其它聚合商的同名频道对齐合并；
    /// 留空则 eventId 取上游原始 ID。
    static std::optional<EarthquakeEvent> parse(const QJsonObject& obj,
                                                const UserLocation& user,
                                                IntensityStandard standard,
                                                const QString& sourceTitle,
                                                const QString& idPrefix,
                                                long long nowMs,
                                                bool originTimeIsJst = false,
                                                const QString& eventNamespace = QString());

    static std::optional<EarthquakeEvent> parseCencDirectory(const QJsonObject& obj,
                                                             const UserLocation& user,
                                                             IntensityStandard standard,
                                                             long long nowMs);

    /// Wolfx jma_eqlist（JMA 地震情报）条目 → 目录事件。字段与 CENC 目录不同（shindo/depth 带单位/时间格式）。
    static std::optional<EarthquakeEvent> parseJmaDirectory(const QJsonObject& obj,
                                                            const UserLocation& user,
                                                            IntensityStandard standard);

    /// 有定位：烈度 + 震级联合判定。
    static WarningLevel deriveLevel(double rawIntensity, double magnitude, bool isCanceled);
    /// 无定位：仅按震级判定。
    static WarningLevel deriveLevelByMagnitude(double magnitude, bool isCanceled);
    static void recompute(EarthquakeEvent& event, const UserLocation& user,
                          IntensityStandard standard);
    static long long parseTime(const QString& raw, bool* ok = nullptr);
    /// 解析无时区的中国标准时间（UTC+8）墙钟，如 Whews 的 cenc / cea / usgs 等端点。
    /// "2026-08-13 08:47:00" → 00:47:00Z。与 parseTime 的区别仅在时区假设。
    static long long parseUtc8Time(const QString& raw, bool* ok = nullptr);
    /// 解析无时区的日本标准时间（JST, UTC+9）墙钟，如 Wolfx 的 jma_eqlist / jma_eew。
    /// "2026/10/06 13:47:00" → 04:47:00Z。与 parseTime 的区别仅在时区假设。
    static long long parseJstTime(const QString& raw, bool* ok = nullptr);

private:
    /// 把无时区墙钟按固定偏移（秒）解释为 epoch 毫秒。
    static long long parseFixedOffsetTime(const QString& raw, int offsetSeconds, bool* ok = nullptr);
};

} // namespace komira
