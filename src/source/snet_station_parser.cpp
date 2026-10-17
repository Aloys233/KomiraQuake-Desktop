#include "source/snet_station_parser.h"

#include <QJsonArray>
#include <QJsonValue>

namespace komira {

std::optional<QVector<SnetStationInfo>> SnetStationParser::parseStations(const QJsonObject& frame) {
    const QJsonValue value = frame.value(QStringLiteral("stations"));
    if (!value.isArray()) return std::nullopt;
    const QJsonArray array = value.toArray();
    if (array.isEmpty()) return std::nullopt;

    QVector<SnetStationInfo> stations;
    stations.reserve(array.size());
    for (const QJsonValue& entry : array) {
        if (!entry.isObject()) continue;
        const QJsonObject obj = entry.toObject();
        SnetStationInfo station;
        station.code = obj.value(QStringLiteral("code")).toString();
        station.latitude = obj.value(QStringLiteral("lat")).toDouble();
        station.longitude = obj.value(QStringLiteral("lon")).toDouble();
        station.name = obj.value(QStringLiteral("name")).toString();
        station.stationType = obj.value(QStringLiteral("type")).toString();
        stations.push_back(station);
    }
    if (stations.isEmpty()) return std::nullopt;
    return stations;
}

std::optional<QVector<double>> SnetStationParser::parseIntensity(const QJsonObject& frame) {
    const QJsonValue value = frame.value(QStringLiteral("int"));
    if (!value.isArray()) return std::nullopt;
    const QJsonArray array = value.toArray();

    QVector<double> intensity;
    intensity.reserve(array.size());
    for (const QJsonValue& entry : array) {
        intensity.push_back(entry.toDouble());
    }
    return intensity;
}

} // namespace komira
