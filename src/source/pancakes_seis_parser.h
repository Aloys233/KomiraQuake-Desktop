#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <optional>

namespace komira {

/// 单个 PancakesSeis 测站档案。来自 `type:"stations"` 报文的 `data` 数组
/// （或 `GET /api/v1/seis/stations` 的同名字段）；数据帧的 `s` 引用档案里的 `sid`。
struct PancakesSeisStation {
    int sid = -1;            ///< 测站短索引，数据帧 `s` 与之对应
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;   ///< `location.alt`（米）
    QString name;            ///< 测站名称（如「四川成都郫都红光监测站 #01」）
    QString region;          ///< 行政区（`location.region`）
};

/// 单个测站的一帧实时数据（上游每秒窗口峰值）。
///
/// 字段省略规则：`v`/`x`/`y`/`z`/`m`/`ec` 为 0 时上游省略；`i` **必现**（静稳为
/// `0`，用于区分「这一秒烈度就是 0」与「这一帧没带烈度」）。温度不在本通道内。
struct PancakesSeisSample {
    int sid = -1;            ///< 测站短索引（对应档案 `sid`）
    double pga = 0.0;        ///< 合成峰值加速度 PGA（Gal）
    double pgv = 0.0;        ///< 合成峰值速度 PGV（cm/s）
    double x = 0.0;          ///< X 轴峰值加速度（Gal）
    double y = 0.0;          ///< Y 轴峰值加速度（Gal）
    double z = 0.0;          ///< Z 轴峰值加速度（Gal）
    double intensity = 0.0;  ///< 仪器烈度（GB/T 17742-2020，设备端计算，1 位小数）
    double maxIntensity = 0.0; ///< 设备端 2 分钟滑动窗口最大烈度（`m`，省略即 0）
    int errorCode = 0;       ///< 测站健康码（`ec`，省略即 0）；非 0 时整帧必须剔除
    long long timestamp = 0; ///< 服务端广播毫秒时间戳
};

/// `/api/v1/seis/stream` 与 `/api/v1/seis/stations` 的纯解析函数（不依赖 QObject，
/// 便于单测）。
struct PancakesSeisParser {
    /// 解析测站档案（`type:"stations"` 的 `data` 数组）。type 不符 / `data` 缺失或
    /// 非数组 / 空表均返回 nullopt。
    static std::optional<QVector<PancakesSeisStation>> parseStations(const QJsonObject& frame);

    /// 解析 HTTP 在线测站列表（`{"code":200,"total":N,"data":[…]}`）。无 `data`
    /// 数组或列表为空返回 nullopt。
    static std::optional<QVector<PancakesSeisStation>> parseStationList(const QJsonObject& root);

    /// 解析单站实时帧。缺 `s` 或 `p`、或帧里带 `type`（档案/心跳）返回 nullopt。
    /// `i` 按约定必现（静稳为 0）；漏发时由 `p` 按 GB/T 17742-2020 兜底换算。
    static std::optional<PancakesSeisSample> parseSample(const QJsonObject& frame);

    /// 报文是否为测站档案（`type == "stations"`）。数据帧不含 `type`。
    static bool isStationFrame(const QJsonObject& frame);
};

/// PGA（Gal）→ 仪器烈度（GB/T 17742-2020），与设备端换算一致：
/// `PGA <= 0.01` 为 0；`0.01~0.22` 线性映射 `PGA/0.22`；`>= 0.22` 用
/// `1.0 + 1.66·log10(PGA/0.22)`（上限 12），结果保留 1 位小数。
/// 设备端已下发 `i` 时**不要**再用它反推；本函数只用于缺 `i` 的历史报文/自测。
double instrumentIntensityFromPga(double pgaGal);

} // namespace komira
