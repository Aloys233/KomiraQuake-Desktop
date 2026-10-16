#include "source/wolfx_seis_parser.h"

#include <QJsonValue>

#include <cmath>

namespace komira {

bool WolfxSeisParser::isHeartbeat(const QJsonObject& frame) {
    return frame.value(QStringLiteral("type")).toString() == QLatin1String("heartbeat");
}

bool WolfxSeisParser::isPong(const QJsonObject& frame) {
    return frame.value(QStringLiteral("type")).toString() == QLatin1String("pong");
}

std::optional<WolfxSeisLiveFrame> WolfxSeisParser::parseStation(const QJsonObject& frame) {
    const QJsonValue type = frame.value(QStringLiteral("type"));
    if (!type.isString()) return std::nullopt;
    const QString id = type.toString();
    // 心跳/pong 的 `type` 是固定字面量，不是测站 UUID。
    if (id.isEmpty() || id == QLatin1String("heartbeat") || id == QLatin1String("pong")) {
        return std::nullopt;
    }
    const QJsonValue latitude = frame.value(QStringLiteral("latitude"));
    const QJsonValue longitude = frame.value(QStringLiteral("longitude"));
    if (!latitude.isDouble() || !longitude.isDouble()) return std::nullopt;

    WolfxSeisLiveFrame live;
    live.id = id;
    live.region = frame.value(QStringLiteral("region")).toString();
    live.latitude = latitude.toDouble();
    live.longitude = longitude.toDouble();
    live.calcShindo = frame.value(QStringLiteral("CalcShindo")).toDouble();
    // `Intensity` 为中国地震烈度（整数 0–12）；缺失时不伪造，置缺测哨兵。
    const QJsonValue intensity = frame.value(QStringLiteral("Intensity"));
    live.csis = intensity.isDouble() ? static_cast<int>(std::lround(intensity.toDouble())) : -1;
    live.shindo = frame.value(QStringLiteral("Shindo")).toString();
    live.updatedAt = frame.value(QStringLiteral("update_at")).toString();
    return live;
}

QVector<WolfxSeisCatalogStation> WolfxSeisParser::parseCatalog(const QJsonObject& root) {
    QVector<WolfxSeisCatalogStation> catalog;
    catalog.reserve(root.size());
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        if (!it.value().isObject()) continue;
        const QJsonObject obj = it.value().toObject();
        WolfxSeisCatalogStation station;
        station.id = it.key();
        station.location = obj.value(QStringLiteral("location")).toString();
        station.latitude = obj.value(QStringLiteral("latitude")).toDouble();
        station.longitude = obj.value(QStringLiteral("longitude")).toDouble();
        // `enable` 缺省视为启用：列表未给该字段时不应把测站悄悄隐藏。
        station.enabled = obj.value(QStringLiteral("enable")).toBool(true);
        catalog.push_back(station);
    }
    return catalog;
}

} // namespace komira
