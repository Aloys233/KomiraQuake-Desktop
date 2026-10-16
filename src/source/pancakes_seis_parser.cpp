#include "source/pancakes_seis_parser.h"

#include <QJsonArray>
#include <QJsonValue>

#include <cmath>

namespace komira {

std::optional<QVector<PancakesSeisStation>> PancakesSeisParser::parseStations(
    const QJsonObject& frame) {
    if (frame.value(QStringLiteral("type")).toString() != QLatin1String("stations")) {
        return std::nullopt;
    }
    const QJsonValue value = frame.value(QStringLiteral("data"));
    if (!value.isArray()) return std::nullopt;
    const QJsonArray array = value.toArray();
    if (array.isEmpty()) return std::nullopt;

    QVector<PancakesSeisStation> stations;
    stations.reserve(array.size());
    for (const QJsonValue& entry : array) {
        if (!entry.isObject()) continue;
        const QJsonObject obj = entry.toObject();
        PancakesSeisStation station;
        station.sid = obj.value(QStringLiteral("sid")).toInt(-1);
        station.name = obj.value(QStringLiteral("name")).toString();
        const QJsonObject location = obj.value(QStringLiteral("location")).toObject();
        station.latitude = location.value(QStringLiteral("lat")).toDouble();
        station.longitude = location.value(QStringLiteral("lon")).toDouble();
        station.region = location.value(QStringLiteral("region")).toString();
        stations.push_back(station);
    }
    if (stations.isEmpty()) return std::nullopt;
    return stations;
}

std::optional<PancakesSeisSample> PancakesSeisParser::parseSample(const QJsonObject& frame) {
    const QJsonValue sid = frame.value(QStringLiteral("s"));
    const QJsonValue pga = frame.value(QStringLiteral("p"));
    if (!sid.isDouble() || !pga.isDouble()) return std::nullopt;
    PancakesSeisSample sample;
    sample.sid = sid.toInt();
    sample.pga = pga.toDouble();
    sample.x = frame.value(QStringLiteral("x")).toDouble();
    sample.y = frame.value(QStringLiteral("y")).toDouble();
    sample.z = frame.value(QStringLiteral("z")).toDouble();
    sample.timestamp = static_cast<long long>(frame.value(QStringLiteral("t")).toDouble());
    return sample;
}

double shindoFromPga(double pgaGal) {
    if (pgaGal <= 0.0) return -3.0;
    return 2.0 * std::log10(pgaGal) + 0.94;
}

} // namespace komira
