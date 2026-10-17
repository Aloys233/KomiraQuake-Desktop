#include "source/kma_station_parser.h"

#include <QJsonArray>
#include <QJsonValue>

namespace komira {

std::optional<QVector<KmaStationInfo>> KmaStationParser::parseStations(const QJsonObject& frame) {
    const QJsonValue value = frame.value(QStringLiteral("stations"));
    if (!value.isArray()) return std::nullopt;
    const QJsonArray array = value.toArray();
    if (array.isEmpty()) return std::nullopt;

    QVector<KmaStationInfo> stations;
    stations.reserve(array.size());
    for (const QJsonValue& entry : array) {
        if (!entry.isObject()) continue;
        const QJsonObject obj = entry.toObject();
        KmaStationInfo station;
        station.latitude = obj.value(QStringLiteral("lat")).toDouble();
        station.longitude = obj.value(QStringLiteral("lon")).toDouble();
        stations.push_back(station);
    }
    if (stations.isEmpty()) return std::nullopt;
    return stations;
}

std::optional<QVector<double>> KmaStationParser::parseMmi(const QJsonObject& frame) {
    const QJsonValue value = frame.value(QStringLiteral("mmi"));
    if (!value.isArray()) return std::nullopt;
    const QJsonArray array = value.toArray();

    QVector<double> mmi;
    mmi.reserve(array.size());
    for (const QJsonValue& entry : array) {
        mmi.push_back(entry.toDouble());
    }
    return mmi;
}

} // namespace komira
