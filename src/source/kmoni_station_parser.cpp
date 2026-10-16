#include "source/kmoni_station_parser.h"

#include <QJsonArray>
#include <QJsonValue>

namespace komira {

std::optional<QVector<KmoniStationInfo>> KmoniStationParser::parseStations(const QJsonObject& frame) {
    const QJsonValue value = frame.value(QStringLiteral("stations"));
    if (!value.isArray()) return std::nullopt;
    const QJsonArray array = value.toArray();
    if (array.isEmpty()) return std::nullopt;

    QVector<KmoniStationInfo> stations;
    stations.reserve(array.size());
    for (const QJsonValue& entry : array) {
        if (!entry.isObject()) continue;
        const QJsonObject obj = entry.toObject();
        KmoniStationInfo station;
        station.code = obj.value(QStringLiteral("code")).toString();
        station.latitude = obj.value(QStringLiteral("lat")).toDouble();
        station.longitude = obj.value(QStringLiteral("lon")).toDouble();
        station.name = obj.value(QStringLiteral("name")).toString();
        station.region = obj.value(QStringLiteral("region")).toString();
        station.stationType = obj.value(QStringLiteral("station_type")).toString();
        stations.push_back(station);
    }
    if (stations.isEmpty()) return std::nullopt;
    return stations;
}

std::optional<QVector<double>> KmoniStationParser::parseIntensity(const QJsonObject& frame) {
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

QString jmaShindoText(double raw) {
    if (raw <= -3.0) return QStringLiteral("?");
    if (raw < 0.5) return QStringLiteral("0");
    if (raw < 1.5) return QStringLiteral("1");
    if (raw < 2.5) return QStringLiteral("2");
    if (raw < 3.5) return QStringLiteral("3");
    if (raw < 4.5) return QStringLiteral("4");
    if (raw < 5.0) return QStringLiteral("5弱");
    if (raw < 5.5) return QStringLiteral("5強");
    if (raw < 6.0) return QStringLiteral("6弱");
    if (raw < 6.5) return QStringLiteral("6強");
    return QStringLiteral("7");
}

} // namespace komira
