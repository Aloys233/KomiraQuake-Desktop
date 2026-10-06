#include "source/eew_parser.h"

#include <QDateTime>
#include <QJsonValue>
#include <QStringList>
#include <QTimeZone>

#include <cmath>

#include "core/quake_calculator.h"
#include "core/travel_time_service.h"

namespace komira {

namespace {

std::optional<double> firstDouble(const QJsonObject& obj, const QStringList& keys) {
    for (const QString& key : keys) {
        const QJsonValue v = obj.value(key);
        if (v.isUndefined() || v.isNull()) continue;
        if (v.isDouble()) {
            const double d = v.toDouble();
            if (std::isfinite(d)) return d;
        } else if (v.isString()) {
            bool ok = false;
            const double d = v.toString().toDouble(&ok);
            if (ok && std::isfinite(d)) return d;
        }
    }
    return std::nullopt;
}

std::optional<int> firstInt(const QJsonObject& obj, const QStringList& keys) {
    const auto d = firstDouble(obj, keys);
    if (!d) return std::nullopt;
    return static_cast<int>(*d);
}

std::optional<QString> firstString(const QJsonObject& obj, const QStringList& keys) {
    for (const QString& key : keys) {
        const QJsonValue v = obj.value(key);
        if (v.isUndefined() || v.isNull()) continue;
        const QString s = v.toString();
        if (!s.isEmpty() && s != QStringLiteral("null")) return s;
    }
    return std::nullopt;
}

std::optional<QJsonValue> firstValue(const QJsonObject& obj, const QStringList& keys) {
    for (const QString& key : keys) {
        const QJsonValue v = obj.value(key);
        if (v.isUndefined() || v.isNull()) continue;
        if (v.isString() && v.toString().trimmed().isEmpty()) continue;
        return v;
    }
    return std::nullopt;
}

/// 源报最大烈度：目录的 intensity、EEW 的 MaxIntensity / epiIntensity。
struct MaxIntensity {
    double raw = 0.0;
    QString text;
};

double jmaTextToRaw(const QString& s) {
    if (s == QStringLiteral("5弱") || s == QStringLiteral("5-")) return 5.0;
    if (s == QStringLiteral("5強") || s == QStringLiteral("5强") || s == QStringLiteral("5+")) return 5.5;
    if (s == QStringLiteral("6弱") || s == QStringLiteral("6-")) return 6.0;
    if (s == QStringLiteral("6強") || s == QStringLiteral("6强") || s == QStringLiteral("6+")) return 6.5;
    if (s == QStringLiteral("7")) return 7.0;
    bool ok = false;
    const double d = s.toDouble(&ok);
    return ok ? d : 0.0;
}

MaxIntensity parseMaxIntensityImpl(const QJsonObject& obj, IntensityStandard standard) {
    MaxIntensity result;
    const auto value = firstValue(obj, {"MaxIntensity", "epiIntensity", "maxIntensity", "intensity"});
    if (!value) return result;

    if (value->isDouble()) {
        const double d = value->toDouble();
        if (std::isfinite(d) && d > 0.0) {
            result.raw = d;
            result.text = standard == IntensityStandard::Jma
                              ? QString::number(d, 'f', 1)
                              : QString::fromStdString(IntensityCalculator::formatCsis(d));
        }
    } else if (value->isString()) {
        const QString s = value->toString().trimmed();
        if (s.isEmpty() || s == QStringLiteral("null") || s == QStringLiteral("-")) return result;
        bool ok = false;
        const double d = s.toDouble(&ok);
        if (ok) {
            result.raw = d;
            result.text = standard == IntensityStandard::Jma
                              ? s
                              : QString::fromStdString(IntensityCalculator::formatCsis(d));
        } else {
            result.text = s;
            result.raw = jmaTextToRaw(s);
        }
    }
    return result;
}

EarthquakeEvent buildEvent(const QString& id,
                           const QString& eventId,
                           double magnitude,
                           double latitude,
                           double longitude,
                           double depth,
                           const QString& location,
                           long long originTime,
                           const QString& sourceTitle,
                           const EewParser::UserLocation& user,
                           IntensityStandard standard,
                           const QString& maxIntensityText,
                           double maxIntensityRaw,
                           int reportNum,
                           bool isFinal,
                           bool isCanceled) {
    EarthquakeEvent event;
    event.id = id.toStdString();
    event.eventId = eventId.toStdString();
    event.magnitude = magnitude;
    event.latitude = latitude;
    event.longitude = longitude;
    event.depth = depth;
    event.location = location.toStdString();
    event.timestamp = originTime;
    event.source = sourceTitle.toStdString();
    event.reportNum = reportNum;
    event.isFinal = isFinal;
    event.isCanceled = isCanceled;
    event.maxIntensityText = maxIntensityText.toStdString();
    event.maxIntensityRaw = maxIntensityRaw;

    EewParser::recompute(event, user, standard);
    return event;
}

} // namespace

void EewParser::recompute(EarthquakeEvent& event, const UserLocation& user,
                          IntensityStandard standard) {
    event.distanceKm = kUnknownDistance;
    event.rawIntensity = 0.0;
    event.estimatedIntensity = "--";
    event.pWaveArrival.reset();
    event.sWaveArrival.reset();
    event.warningLevel = deriveLevelByMagnitude(event.magnitude, event.isCanceled);
    if (!user) return;
    const double distance = QuakeCalculator::haversineDistance(
        user->first, user->second, event.latitude, event.longitude);
    event.distanceKm = distance;
    event.rawIntensity = IntensityCalculator::rawCsis(event.magnitude, distance, event.depth);
    event.estimatedIntensity = standard == IntensityStandard::Jma
        ? IntensityCalculator::formatJma(event.magnitude, distance, event.depth)
        : IntensityCalculator::formatCsis(event.rawIntensity);
    const auto travel = TravelTimeService::instance().estimate(event.depth, distance);
    if (std::isfinite(travel.first) && travel.first >= 0)
        event.pWaveArrival = event.timestamp + std::llround(travel.first * 1000);
    if (std::isfinite(travel.second) && travel.second >= 0)
        event.sWaveArrival = event.timestamp + std::llround(travel.second * 1000);
    event.warningLevel = deriveLevel(event.rawIntensity, event.magnitude, event.isCanceled);
}

WarningLevel EewParser::deriveLevel(double rawIntensity, double magnitude, bool isCanceled) {
    if (isCanceled) return WarningLevel::Normal;
    if (rawIntensity >= 5.0 || magnitude >= 6.5) return WarningLevel::Critical;
    if (rawIntensity >= 3.0 || magnitude >= 4.5) return WarningLevel::Warning;
    if (rawIntensity >= 1.5 || magnitude >= 3.0) return WarningLevel::Watch;
    return WarningLevel::Normal;
}

WarningLevel EewParser::deriveLevelByMagnitude(double magnitude, bool isCanceled) {
    if (isCanceled) return WarningLevel::Normal;
    if (magnitude >= 6.5) return WarningLevel::Critical;
    if (magnitude >= 4.5) return WarningLevel::Warning;
    if (magnitude >= 3.0) return WarningLevel::Watch;
    return WarningLevel::Normal;
}

long long EewParser::parseTime(const QString& raw, bool* ok) {
    if (ok) *ok = false;
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty()) return 0;

    bool allDigits = true;
    for (const QChar& c : trimmed) {
        if (!c.isDigit()) {
            allDigits = false;
            break;
        }
    }
    if (allDigits) {
        bool numOk = false;
        long long v = trimmed.toLongLong(&numOk);
        if (!numOk) return 0;
        if (ok) *ok = true;
        return v < 100'000'000'000LL ? v * 1000 : v;
    }

    const QDateTime isoDt = QDateTime::fromString(trimmed, Qt::ISODate);
    if (isoDt.isValid()) {
        if (ok) *ok = true;
        return isoDt.toMSecsSinceEpoch();
    }

    static const QStringList formats = {
        QStringLiteral("yyyy-MM-dd HH:mm:ss"),
        QStringLiteral("yyyy/MM/dd HH:mm:ss"),
        QStringLiteral("yyyy-MM-ddTHH:mm:ss"),
    };
    for (const QString& fmt : formats) {
        const QDateTime dt = QDateTime::fromString(trimmed, fmt);
        if (dt.isValid()) {
            if (ok) *ok = true;
            return dt.toMSecsSinceEpoch();
        }
    }
    return 0;
}

long long EewParser::parseUtc8Time(const QString& raw, bool* ok) {
    // Whews 的 cenc / cea / usgs 等端点时刻是无时区墙钟（UTC+8）。
    return parseFixedOffsetTime(raw, 8 * 3600, ok);
}

long long EewParser::parseJstTime(const QString& raw, bool* ok) {
    // Wolfx 的 JMA 报文时刻是无时区墙钟（JST）。
    return parseFixedOffsetTime(raw, 9 * 3600, ok);
}

long long EewParser::parseFixedOffsetTime(const QString& raw, int offsetSeconds, bool* ok) {
    if (ok) *ok = false;
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty()) return 0;
    // 纯数字时间戳（epoch 秒/毫秒）本身不含时区歧义，交给 parseTime。
    bool allDigits = true;
    for (const QChar& c : trimmed) {
        if (!c.isDigit()) { allDigits = false; break; }
    }
    if (allDigits) return parseTime(trimmed, ok);
    // fromString 默认按本地时区解释，必须显式改判偏移，否则非 UTC+8 机器会整体偏移。
    static const QStringList formats = {
        QStringLiteral("yyyy/MM/dd HH:mm:ss"),
        QStringLiteral("yyyy/MM/dd HH:mm"),
        QStringLiteral("yyyy-MM-dd HH:mm:ss"),
        QStringLiteral("yyyy-MM-dd HH:mm"),
    };
    for (const QString& fmt : formats) {
        QDateTime dt = QDateTime::fromString(trimmed, fmt);
        if (!dt.isValid()) continue;
        dt.setTimeZone(QTimeZone(offsetSeconds));   // 墙钟不变，重解释时区
        if (ok) *ok = true;
        return dt.toMSecsSinceEpoch();
    }
    return 0;
}

std::optional<EarthquakeEvent> EewParser::parse(const QJsonObject& obj,
                                                const UserLocation& user,
                                                IntensityStandard standard,
                                                const QString& sourceTitle,
                                                const QString& idPrefix,
                                                long long nowMs,
                                                bool originTimeIsJst,
                                                const QString& eventNamespace) {
    if (obj.value("isTraining").toBool(false)) return std::nullopt;

    // 日本气象厅的报文时刻是 JST 墙钟，中国各局是本机时区（UTC+8）墙钟。
    const auto parseReportTime = [originTimeIsJst](const QString& raw) {
        return originTimeIsJst ? EewParser::parseJstTime(raw) : EewParser::parseTime(raw);
    };

    const bool magnitudeUnknown = obj.value("magnitudeUnknown").toBool(false);
    auto magnitude = firstDouble(obj, {"Magnitude", "Magunitude"});
    if (!magnitude && !magnitudeUnknown) magnitude = firstDouble(obj, {"magnitude"});
    const auto latitude = firstDouble(obj, {"Latitude", "latitude"});
    const auto longitude = firstDouble(obj, {"Longitude", "longitude"});
    if (!magnitude || !latitude || !longitude) return std::nullopt;

    const double depth = firstDouble(obj, {"Depth", "depth"}).value_or(kDefaultDepth);
    QString location = firstString(obj, {"HypoCenter", "Hypocenter", "location", "placeName"})
                           .value_or(QStringLiteral("未知震源"));
    if (location.isEmpty()) location = QStringLiteral("未知震源");

    const QString rawId =
        firstString(obj, {"EventID", "ID", "id", "eventId"}).value_or(QString::number(nowMs));
    const int reportNum = firstInt(obj, {"ReportNum", "Serial", "serial", "number", "updates"}).value_or(1);
    const bool isFinal = obj.value("isFinal").toBool(false);
    const bool isCanceled = obj.value("isCancel").toBool(obj.value("isCanceled").toBool(false));
    const long long originTime =
        parseReportTime(firstString(obj, {"OriginTime", "originTime", "shockTime", "time"}).value_or(QString()));

    const MaxIntensity maxIntensity = parseMaxIntensityImpl(obj, standard);
    // 频道化 eventId：跨聚合商对齐合并键（见 EarthquakeEvent::identity）。
    const QString eventId = eventNamespace.isEmpty()
                                ? rawId
                                : eventNamespace + QLatin1Char(':') + rawId;
    EarthquakeEvent event = buildEvent(idPrefix + rawId, eventId, *magnitude, *latitude, *longitude, depth, location,
                      originTime != 0 ? originTime : nowMs, sourceTitle, user, standard,
                      maxIntensity.text, maxIntensity.raw, reportNum, isFinal, isCanceled);
    event.reportTime = parseReportTime(firstString(obj, {"ReportTime", "reportTime", "updateTime"}).value_or(QString()));
    return event;
}

std::optional<EarthquakeEvent> EewParser::parseCencDirectory(const QJsonObject& obj,
                                                             const UserLocation& user,
                                                             IntensityStandard standard,
                                                             long long nowMs) {
    const auto magnitude = firstDouble(obj, {"magnitude", "Magnitude"});
    const auto latitude = firstDouble(obj, {"latitude", "Latitude"});
    const auto longitude = firstDouble(obj, {"longitude", "Longitude"});
    if (!magnitude || !latitude || !longitude) return std::nullopt;

    const double depth = firstDouble(obj, {"depth", "Depth"}).value_or(kDefaultDepth);
    QString location = firstString(obj, {"location", "placeName"}).value_or(QStringLiteral("未知震源"));
    if (location.isEmpty()) location = QStringLiteral("未知震源");
    long long origin = parseTime(firstString(obj, {"time", "originTime"}).value_or(QString()));
    if (origin == 0) origin = nowMs;

    const QString sourceTitle = QStringLiteral("中国地震台网 地震信息");
    // 优先用数据源自带的 EventID（跨报次稳定，且可与 WS 预警链路对齐）
    const QString rawEventId = firstString(obj, {"EventID", "id"}).value_or(QString());
    const QString id = rawEventId.isEmpty()
                           ? QStringLiteral("cenc_%1_%2").arg(origin).arg(*latitude, 0, 'f', 2)
                           : QStringLiteral("cenc_") + rawEventId;

    const MaxIntensity maxIntensity = parseMaxIntensityImpl(obj, standard);
    EarthquakeEvent event = buildEvent(id, rawEventId, *magnitude, *latitude, *longitude, depth,
                                       location, origin, sourceTitle, user, standard,
                                       maxIntensity.text, maxIntensity.raw, 1, true, false);
    // 目录永不产生 warning/critical
    if (event.warningLevel == WarningLevel::Critical || event.warningLevel == WarningLevel::Warning) {
        event.warningLevel = WarningLevel::Watch;
    }
    return event;
}

EewParser::MaxIntensityValue EewParser::parseMaxIntensity(const QJsonObject& obj,
                                                          IntensityStandard standard) {
    const MaxIntensity m = parseMaxIntensityImpl(obj, standard);
    return MaxIntensityValue{m.raw, m.text};
}

std::optional<EarthquakeEvent> EewParser::parseJmaDirectory(const QJsonObject& obj,
                                                            const UserLocation& user,
                                                            IntensityStandard standard) {
    const auto latitude = firstDouble(obj, {"latitude", "Latitude"});
    const auto longitude = firstDouble(obj, {"longitude", "Longitude"});
    if (!latitude || !longitude) return std::nullopt;
    const double magnitude = firstDouble(obj, {"magnitude", "Magnitude"}).value_or(0.0);

    // depth 形如 "10km"，需去掉单位后缀再解析。
    double depth = kDefaultDepth;
    if (const auto raw = firstString(obj, {"depth", "Depth"})) {
        int end = 0;
        while (end < raw->size()) {
            const QChar c = raw->at(end);
            if (c.isDigit() || c == QLatin1Char('.') || c == QLatin1Char('-')) ++end;
            else break;
        }
        bool depthOk = false;
        const double d = raw->left(end).toDouble(&depthOk);
        if (depthOk && std::isfinite(d) && d >= 0.0) depth = d;
    }

    QString location = firstString(obj, {"location", "placeName"}).value_or(QStringLiteral("未知震源"));
    if (location.isEmpty()) location = QStringLiteral("未知震源");

    // time_full 含秒，time 只到分钟；两者都是 JST 墙钟。解析失败则丢弃，不退回 now。
    bool timeOk = false;
    const long long origin = parseJstTime(firstString(obj, {"time_full", "time"}).value_or(QString()), &timeOk);
    if (!timeOk || origin == 0) return std::nullopt;

    const QString rawEventId = firstString(obj, {"EventID", "id"}).value_or(QString());
    const QString id = rawEventId.isEmpty()
                           ? QStringLiteral("wolfx_jmaeqlist_%1_%2").arg(origin).arg(*latitude, 0, 'f', 2)
                           : QStringLiteral("wolfx_jmaeqlist_") + rawEventId;
    // 与 Pancakes 的 jma_eqlist 共用事件命名空间，源内去重/合并口径一致。
    const QString eventId = rawEventId.isEmpty()
                                ? QStringLiteral("jma_eqlist:%1").arg(origin)
                                : QStringLiteral("jma_eqlist:") + rawEventId;

    // shindo 为 JMA 震度（"1"/"5-"/"5+"/"7"）：固定按 JMA 展示，不随用户烈度标准转换。
    QString shindoText;
    double shindoRaw = 0.0;
    if (const auto shindo = firstString(obj, {"shindo", "Shindo"})) {
        const QString t = shindo->trimmed();
        if (!t.isEmpty() && t != QStringLiteral("-") && t != QStringLiteral("null")) {
            bool numOk = false;
            const double d = t.toDouble(&numOk);
            shindoText = t;
            shindoRaw = numOk ? d : jmaTextToRaw(t);
        }
    }

    EarthquakeEvent event = buildEvent(id, eventId, magnitude, *latitude, *longitude, depth,
                                       location, origin, QStringLiteral("JMA 地震情报"), user, standard,
                                       shindoText, shindoRaw, 1, true, false);
    // 目录永不产生 warning/critical
    if (event.warningLevel == WarningLevel::Critical || event.warningLevel == WarningLevel::Warning) {
        event.warningLevel = WarningLevel::Watch;
    }
    return event;
}

} // namespace komira
