#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <optional>

namespace komira {

/// 单个 NIED 测站元数据。来自 `/kmoni` **首包**的 `stations` 数组，
/// 数组顺序与后续帧 `int[]` 一一对应。
struct KmoniStationInfo {
    QString code;         ///< 测站代码（如 ABSH01）
    double latitude = 0.0;
    double longitude = 0.0;
    QString name;         ///< 观测点名称（日文）
    QString region;       ///< 都道府県
    QString stationType;  ///< K-NET / KiK-net
};

/// `/kmoni` 报文的纯解析函数（不依赖 QObject，便于单测）。
struct KmoniStationParser {
    /// 解析首包的 `stations` 元数据数组。字段缺失/非数组返回 nullopt。
    static std::optional<QVector<KmoniStationInfo>> parseStations(const QJsonObject& frame);

    /// 解析增量帧的 `int` 连续震度数组（JMA 震度阶，缺测 -3）。非数组返回 nullopt。
    static std::optional<QVector<double>> parseIntensity(const QJsonObject& frame);
};

/// 连续震度原始值 → JMA 震度阶显示文本（`0`/`1`/…/`5弱`/`5強`/`6弱`/`6強`/`7`）。
/// `raw <= -3`（缺测）返回 `"?"`。阈值与 kanameishi `getShindoFromInstShindo` 对齐。
QString jmaShindoText(double raw);

} // namespace komira
