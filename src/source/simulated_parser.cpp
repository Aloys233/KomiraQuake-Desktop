#include "source/simulated_parser.h"

#include <QJsonValue>

#include "source/simulated_protocol.h"

namespace komira {

namespace {

/// Qt JSON 把所有数字都存成 double，故 epoch 毫秒须经此显式取整。
/// 不走字符串路径：客户端的三道时间闸门（30 分钟活跃窗口 / 未来时刻 / 解析缩放）
/// 都按数字语义处理，墙钟串会按设备本地时区整体偏移。
long long toEpochMs(const QJsonValue& v) {
    if (v.isDouble()) return static_cast<long long>(v.toDouble());
    if (v.isString()) {
        bool ok = false;
        const long long n = v.toString().toLongLong(&ok);
        return ok ? n : 0;
    }
    return 0;
}

double toDouble(const QJsonObject& obj, const QString& key, double fallback) {
    const QJsonValue v = obj.value(key);
    if (v.isDouble()) return v.toDouble();
    if (v.isString()) {
        bool ok = false;
        const double d = v.toString().toDouble(&ok);
        if (ok) return d;
    }
    return fallback;
}

int toInt(const QJsonObject& obj, const QString& key, int fallback) {
    const QJsonValue v = obj.value(key);
    if (v.isDouble()) return static_cast<int>(v.toDouble());
    return fallback;
}

bool toBool(const QJsonObject& obj, const QString& key) {
    const QJsonValue v = obj.value(key);
    return v.isBool() ? v.toBool() : false;
}

} // namespace

std::optional<EarthquakeEvent> SimulatedParser::parseReport(const QJsonObject& frame,
                                                            SourceEventKind kind,
                                                            const EewParser::UserLocation& user,
                                                            IntensityStandard standard,
                                                            long long nowMs) {
    const QString eventId = frame.value(QStringLiteral("eventId")).toString();
    if (eventId.isEmpty()) return std::nullopt;

    const long long origin = toEpochMs(frame.value(QStringLiteral("originTime")));
    if (origin <= 0) return std::nullopt;

    const double latitude = toDouble(frame, QStringLiteral("latitude"), 0.0);
    const double longitude = toDouble(frame, QStringLiteral("longitude"), 0.0);
    // 震中缺省为 0/0 会被当成几内亚湾的合法坐标，故用「键是否存在」判定必填。
    if (!frame.contains(QStringLiteral("latitude")) || !frame.contains(QStringLiteral("longitude")))
        return std::nullopt;

    EarthquakeEvent event;
    // id 带 sim_ 前缀，eventId 已在 sim- 命名空间内；两者都不模仿真实报文形状。
    event.id = ("sim_" + eventId).toStdString();
    event.eventId = eventId.toStdString();
    event.magnitude = toDouble(frame, QStringLiteral("magnitude"), 0.0);
    event.latitude = latitude;
    event.longitude = longitude;
    event.depth = toDouble(frame, QStringLiteral("depth"), EewParser::kDefaultDepth);
    const QString location = frame.value(QStringLiteral("location")).toString();
    event.location = (location.isEmpty() ? QString::fromUtf8("未知震源") : location).toStdString();
    event.timestamp = origin;
    event.source = simulatedTitleFor(kind).toStdString();
    event.sourceProvider = SimulatedProtocol::provider().toStdString();
    event.sourceAgency = SimulatedProtocol::agency().toStdString();
    // 烈度文本原文透传：JMA 式写法（如 5弱）经 parseMaxIntensity 会被重新格式化而破坏。
    event.maxIntensityText =
        frame.value(QStringLiteral("maxIntensityText")).toString().toStdString();
    event.maxIntensityRaw = toDouble(frame, QStringLiteral("maxIntensity"), 0.0);
    event.reportNum = toInt(frame, QStringLiteral("reportNum"), 1);
    event.isFinal = toBool(frame, QStringLiteral("isFinal"));
    event.isCanceled = toBool(frame, QStringLiteral("isCanceled"));
    // 收帧时刻：目录去重以 reportTime 决胜（见 AppController::handleEvent），
    // 缺省会令目录条目任意胜出。服务端不发送该字段。
    event.reportTime = nowMs;

    EewParser::recompute(event, user, standard);
    return event;
}

} // namespace komira
