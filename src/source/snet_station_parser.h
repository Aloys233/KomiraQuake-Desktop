#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <optional>

namespace komira {

/// 单个 S-Net 测站元数据。来自 `/s-net` **首包**的 `stations` 数组，
/// 数组顺序与后续帧 `int[]` 一一对应。
struct SnetStationInfo {
    QString code;         ///< 测站代码（如 N.S1N01）
    double latitude = 0.0;
    double longitude = 0.0;
    QString name;         ///< 测站简称（如 S1N01）
    QString stationType;  ///< 观测网/区段（如 S-net、sagami）
};

/// `/s-net` 报文的纯解析函数（不依赖 QObject，便于单测）。
struct SnetStationParser {
    /// 解析首包的 `stations` 元数据数组。字段缺失/非数组返回 nullopt。
    static std::optional<QVector<SnetStationInfo>> parseStations(const QJsonObject& frame);

    /// 解析增量帧的 `int` 连续震度数组（JMA 計測震度，缺测 -3，饱和 99）。
    /// 非数组返回 nullopt。
    static std::optional<QVector<double>> parseIntensity(const QJsonObject& frame);
};

} // namespace komira
