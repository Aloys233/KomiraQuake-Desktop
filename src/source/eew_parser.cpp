#include "source/eew_parser.h"

#include <QDateTime>
#include <QJsonValue>
#include <QStringList>

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
    if (s == QStringLiteral("5弱")) return 5.0;
    if (s == QStringLiteral("5強") || s == QStringLiteral("5强")) return 5.5;
    if (s == QStringLiteral("6弱")) return 6.0;
    if (s == QStringLiteral("6強") || s == QStringLiteral("6强")) return 6.5;
    if (s == QStringLiteral("7")) return 7.0;
    bool ok = false;
    const double d = s.toDouble(&ok);
    return ok ? d : 0.0;
}

MaxIntensity parseMaxIntensity(const QJsonObject& obj, IntensityStandard standard) {
    MaxIntensity result;
    const auto value = firstValue(obj, {"MaxIntensity", "epiIntensity", "intensity"});
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

std::optional<EarthquakeEvent> EewParser::parse(const QJsonObject& obj,
                                                const UserLocation& user,
                                                IntensityStandard standard,
                                                const QString& sourceTitle,
                                                const QString& idPrefix,
                                                long long nowMs) {
    if (obj.value("isTraining").toBool(false)) return std::nullopt;

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
        parseTime(firstString(obj, {"OriginTime", "originTime", "shockTime", "time"}).value_or(QString()));

    const MaxIntensity maxIntensity = parseMaxIntensity(obj, standard);
    EarthquakeEvent event = buildEvent(idPrefix + rawId, rawId, *magnitude, *latitude, *longitude, depth, location,
                      originTime != 0 ? originTime : nowMs, sourceTitle, user, standard,
                      maxIntensity.text, maxIntensity.raw, reportNum, isFinal, isCanceled);
    event.reportTime = parseTime(firstString(obj, {"ReportTime", "reportTime", "updateTime"}).value_or(QString()));
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

    const bool reviewed = firstString(obj, {"type"}) == QStringLiteral("reviewed");
    const QString sourceTitle = reviewed ? QStringLiteral("CENC 正式测定") : QStringLiteral("CENC 自动测定");
    // 优先用数据源自带的 EventID（跨报次稳定，且可与 WS 预警链路对齐）
    const QString rawEventId = firstString(obj, {"EventID", "id"}).value_or(QString());
    const QString id = rawEventId.isEmpty()
                           ? QStringLiteral("cenc_%1_%2").arg(origin).arg(*latitude, 0, 'f', 2)
                           : QStringLiteral("cenc_") + rawEventId;

    const MaxIntensity maxIntensity = parseMaxIntensity(obj, standard);
    EarthquakeEvent event = buildEvent(id, rawEventId, *magnitude, *latitude, *longitude, depth,
                                       location, origin, sourceTitle, user, standard,
                                       maxIntensity.text, maxIntensity.raw, 1, true, false);
    // 目录永不产生 warning/critical
    if (event.warningLevel == WarningLevel::Critical || event.warningLevel == WarningLevel::Warning) {
        event.warningLevel = WarningLevel::Watch;
    }
    return event;
}

} // namespace komira
