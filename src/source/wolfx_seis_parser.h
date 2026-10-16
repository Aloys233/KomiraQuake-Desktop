#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <optional>

namespace komira {

/// 测站列表条目（`GET https://api.wolfx.jp/seis_list.json`，对象以 UUID 为键）。
struct WolfxSeisCatalogStation {
    QString id;              ///< 测站 UUID
    QString location;        ///< 展示位置（列表 `location`）
    double latitude = 0.0;
    double longitude = 0.0;
    bool enabled = true;     ///< 列表 `enable`
};

/// `/all_seis` 单站实时帧。报文自带坐标与震度，故无需先取得台站表即可展示。
struct WolfxSeisLiveFrame {
    QString id;              ///< 测站 UUID（报文 `type`）
    QString region;          ///< 帧内位置（`region`），列表缺失时作展示名回退
    double latitude = 0.0;
    double longitude = 0.0;
    double calcShindo = 0.0; ///< 合成 JMA 計測震度（连续值，静稳可为负）
    int csis = -1;           ///< 中国地震烈度（报文 `Intensity`，整数 0–12）；缺失为 -1
    QString shindo;          ///< 上游分档文本（如 `0`、`5弱`）
    QString updatedAt;       ///< 数据更新时间（上游 `update_at`）
};

/// Wolfx SeisJS 报文的纯解析函数（不依赖 QObject，便于单测）。
struct WolfxSeisParser {
    /// 报文是否为心跳包（`type == "heartbeat"`）。
    static bool isHeartbeat(const QJsonObject& frame);
    /// 报文是否为心跳应答（`type == "pong"`）。
    static bool isPong(const QJsonObject& frame);
    /// 解析单站实时帧。心跳/pong、或缺少 `type`/`latitude`/`longitude` 返回 nullopt。
    static std::optional<WolfxSeisLiveFrame> parseStation(const QJsonObject& frame);
    /// 解析测站列表。非对象成员一律跳过；返回可能为空的列表。
    static QVector<WolfxSeisCatalogStation> parseCatalog(const QJsonObject& root);
};

} // namespace komira
