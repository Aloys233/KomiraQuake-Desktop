#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <optional>

namespace komira {

/// 单个 PancakesSeis 测站元数据。来自 `type:"stations"` 报文的 `data` 数组，
/// 数组下标即后续加速度帧 `s`（sid）引用的索引。
struct PancakesSeisStation {
    int sid = -1;            ///< 测站短索引，数据帧 `s` 与之对应
    double latitude = 0.0;
    double longitude = 0.0;
    QString name;            ///< 观测点名称（如「四川省成都市郫都区」）
    QString region;          ///< 行政区
};

/// 单个测站的一帧三轴加速度。`p`/`x`/`y`/`z` 单位均为 Gal（cm/s²）。
struct PancakesSeisSample {
    int sid = -1;            ///< 测站索引（对应台站表 `sid`）
    double pga = 0.0;        ///< 合成峰值加速度 PGA（Gal）
    double x = 0.0;          ///< X 轴加速度（Gal）
    double y = 0.0;          ///< Y 轴加速度（Gal）
    double z = 0.0;          ///< Z 轴加速度（Gal）
    long long timestamp = 0; ///< 毫秒时间戳
};

/// `/api/v1/seis/stream` 报文的纯解析函数（不依赖 QObject，便于单测）。
struct PancakesSeisParser {
    /// 解析台站表（`type:"stations"` 的 `data` 数组）。type 不符 / `data` 缺失或
    /// 非数组 / 空表均返回 nullopt。
    static std::optional<QVector<PancakesSeisStation>> parseStations(const QJsonObject& frame);

    /// 解析单站加速度帧。缺 `s` 或 `p` 返回 nullopt；`e`（芯片温度）不解析。
    static std::optional<PancakesSeisSample> parseSample(const QJsonObject& frame);
};

/// PGA（Gal）→ JMA 計測震度近似值，仅用于套用现有测站色阶做定性着色。
/// 采用常用经验式 `I = 2·log10(PGA) + 0.94`；PGA <= 0（缺测）返回 -3（色阶最低档）。
double shindoFromPga(double pgaGal);

} // namespace komira
