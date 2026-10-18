#include "source/pancakes_seis_parser.h"

#include <QJsonArray>
#include <QJsonValue>

#include "core/intensity_calculator.h"

namespace komira {

namespace {

/// 解析 `data` 数组中的一条测站档案；缺 `sid` 的条目丢弃（无法与数据帧对齐）。
std::optional<PancakesSeisStation> parseStationEntry(const QJsonValue& entry) {
    if (!entry.isObject()) return std::nullopt;
    const QJsonObject obj = entry.toObject();
    const QJsonValue sid = obj.value(QStringLiteral("sid"));
    if (!sid.isDouble()) return std::nullopt;
    PancakesSeisStation station;
    station.sid = sid.toInt(-1);
    if (station.sid < 0) return std::nullopt;
    station.name = obj.value(QStringLiteral("name")).toString();
    const QJsonObject location = obj.value(QStringLiteral("location")).toObject();
    station.latitude = location.value(QStringLiteral("lat")).toDouble();
    station.longitude = location.value(QStringLiteral("lon")).toDouble();
    station.altitude = location.value(QStringLiteral("alt")).toDouble();
    station.region = location.value(QStringLiteral("region")).toString();
    return station;
}

std::optional<QVector<PancakesSeisStation>> parseStationArray(const QJsonObject& root) {
    const QJsonValue value = root.value(QStringLiteral("data"));
    if (!value.isArray()) return std::nullopt;
    const QJsonArray array = value.toArray();
    if (array.isEmpty()) return std::nullopt;

    QVector<PancakesSeisStation> stations;
    stations.reserve(array.size());
    for (const QJsonValue& entry : array) {
        if (auto station = parseStationEntry(entry)) stations.push_back(*station);
    }
    if (stations.isEmpty()) return std::nullopt;
    return stations;
}

} // namespace

bool PancakesSeisParser::isStationFrame(const QJsonObject& frame) {
    return frame.value(QStringLiteral("type")).toString() == QLatin1String("stations");
}

std::optional<QVector<PancakesSeisStation>> PancakesSeisParser::parseStations(
    const QJsonObject& frame) {
    if (!isStationFrame(frame)) return std::nullopt;
    return parseStationArray(frame);
}

std::optional<QVector<PancakesSeisStation>> PancakesSeisParser::parseStationList(
    const QJsonObject& root) {
    // HTTP 列表不带 `type`，字段与广播档案的 `data` 相同。
    return parseStationArray(root);
}

std::optional<PancakesSeisSample> PancakesSeisParser::parseSample(const QJsonObject& frame) {
    // 数据帧不含 `type`（含 `type` 的是档案/心跳）；`s` 与 `p` 必现。
    if (frame.contains(QStringLiteral("type"))) return std::nullopt;
    const QJsonValue sid = frame.value(QStringLiteral("s"));
    const QJsonValue pga = frame.value(QStringLiteral("p"));
    if (!sid.isDouble() || !pga.isDouble()) return std::nullopt;
    PancakesSeisSample sample;
    sample.sid = sid.toInt();
    sample.pga = pga.toDouble();
    // 可选字段：上游为 0 时省略，缺省即 0，不需要哨兵。
    // 注意 `x`/`y`/`z` 三轴峰值加速度已由上游移除（实测帧只带 `s`/`p`/`i`/`m`/`t`），
    // 这里不再解析；老报文带了也只是被忽略，不影响其余字段。
    sample.pgv = frame.value(QStringLiteral("v")).toDouble();
    // `i` 按约定必现（静稳为 0）；若上游漏发，按同一 GB/T 17742-2020 公式由 PGA
    // 兜底换算，避免把「有读数」误判成「没带烈度」。
    const QJsonValue intensity = frame.value(QStringLiteral("i"));
    sample.intensity = intensity.isDouble() ? intensity.toDouble()
                                            : instrumentIntensityFromPga(sample.pga);
    sample.maxIntensity = frame.value(QStringLiteral("m")).toDouble();
    sample.errorCode = frame.value(QStringLiteral("ec")).toInt(0);
    sample.timestamp = static_cast<long long>(frame.value(QStringLiteral("t")).toDouble());
    return sample;
}

double instrumentIntensityFromPga(double pgaGal) {
    // 与 IntensityCalculator::csisFromPga 共用同一实现，避免两处公式分叉。
    return IntensityCalculator::csisFromPga(pgaGal);
}

} // namespace komira
