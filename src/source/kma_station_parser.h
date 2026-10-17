#pragma once

#include <QJsonObject>
#include <QVector>

#include <optional>

namespace komira {

/// 单个 KMA 测站元数据。来自 `/kma-station` **首包**（及重连首包）的 `stations`
/// 数组；上游只给坐标，没有代码/名称。数组顺序与后续帧 `mmi[]` 一一对应。
struct KmaStationInfo {
    double latitude = 0.0;
    double longitude = 0.0;
};

/// `/kma-station` 报文的纯解析函数（不依赖 QObject，便于单测）。
struct KmaStationParser {
    /// 解析首包的 `stations` 坐标数组。字段缺失/非数组返回 nullopt。
    static std::optional<QVector<KmaStationInfo>> parseStations(const QJsonObject& frame);

    /// 解析增量帧的 `mmi` 烈度数组（韩国 MMI，缺测 -3）。非数组返回 nullopt。
    static std::optional<QVector<double>> parseMmi(const QJsonObject& frame);
};

} // namespace komira
