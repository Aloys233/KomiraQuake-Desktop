#include "source/pancakes_parser.h"

#include <QJsonArray>
#include <QJsonValue>
#include <cmath>

#include "source/pancakes_protocol.h"

namespace komira {

namespace {

const QString kPrefixGq = QStringLiteral("pancakes_gq_");
const QString kPrefixUsgs = QStringLiteral("pancakes_usgs_");
const QString kPrefixJmaEew = QStringLiteral("pancakes_jmaeew_");
const QString kPrefixJmaEqlist = QStringLiteral("pancakes_jmaeqlist_");

QString prefixFor(const QString& source) {
    if (source == "gq") return kPrefixGq;
    if (source == "usgs") return kPrefixUsgs;
    if (source == "jma_eew") return kPrefixJmaEew;
    if (source == "jma_eqlist") return kPrefixJmaEqlist;
    return QStringLiteral("pancakes_");
}

std::optional<double> firstDouble(const QJsonObject& obj, const QString& key) {
    const QJsonValue v = obj.value(key);
    if (v.isUndefined() || v.isNull()) return std::nullopt;
    if (v.isDouble()) {
        const double d = v.toDouble();
        if (std::isfinite(d)) return d;
    } else if (v.isString()) {
        bool ok = false;
        const double d = v.toString().toDouble(&ok);
        if (ok && std::isfinite(d)) return d;
    }
    return std::nullopt;
}

std::optional<int> firstInt(const QJsonObject& obj, const QString& key) {
    const auto d = firstDouble(obj, key);
    if (!d) return std::nullopt;
    return static_cast<int>(*d);
}

std::optional<long long> firstLong(const QJsonObject& obj, const QString& key) {
    const QJsonValue v = obj.value(key);
    if (v.isUndefined() || v.isNull()) return std::nullopt;
    if (v.isDouble()) return static_cast<long long>(v.toDouble());
    if (v.isString()) {
        bool ok = false;
        const long long n = v.toString().toLongLong(&ok);
        if (ok) return n;
    }
    return std::nullopt;
}

std::optional<QString> firstString(const QJsonObject& obj, const QString& key) {
    const QJsonValue v = obj.value(key);
    if (v.isUndefined() || v.isNull()) return std::nullopt;
    const QString s = v.toString();
    if (!s.isEmpty() && s != QStringLiteral("null")) return s;
    return std::nullopt;
}

/// 组织基础事件并交由 EewParser::recompute 填充距离/烈度/走时/等级。
PancakesParsed build(const QString& source,
                     const QString& rawEventId,
                     double magnitude,
                     double latitude,
                     double longitude,
                     double depth,
                     const QString& location,
                     long long originTime,
                     const QString& maxText,
                     double maxRaw,
                     int reportNum,
                     bool isFinal,
                     bool isCanceled,
                     long long sourceUpdatedAt,
                     const EewParser::UserLocation& user,
                     IntensityStandard standard,
                     PancakesKind kind) {
    EarthquakeEvent event;
    event.id = (prefixFor(source) + rawEventId).toStdString();
    event.eventId = (source + QStringLiteral(":") + rawEventId).toStdString();
    event.magnitude = magnitude;
    event.latitude = latitude;
    event.longitude = longitude;
    event.depth = depth;
    event.location = location.toStdString();
    event.timestamp = originTime;
    event.reportTime = sourceUpdatedAt;
    event.source = PancakesProtocol::titleFor(source).toStdString();
    event.sourceProvider = PancakesProtocol::providerName().toStdString();
    event.sourceAgency = PancakesProtocol::agencyFor(source).toStdString();
    event.maxIntensityText = maxText.toStdString();
    event.maxIntensityRaw = maxRaw;
    event.reportNum = reportNum;
    event.isFinal = isFinal;
    event.isCanceled = isCanceled;

    EewParser::recompute(event, user, standard);
    if (kind == PancakesKind::Directory) {
        // 目录永不产生 warning/critical：若有则降级为 watch。
        if (event.warningLevel == WarningLevel::Critical || event.warningLevel == WarningLevel::Warning)
            event.warningLevel = WarningLevel::Watch;
    }
    return PancakesParsed{event, kind, source};
}

std::optional<PancakesParsed> parseGq(const QString& action, const QJsonObject& payload,
                                      long long envelopeTime, const EewParser::UserLocation& user,
                                      IntensityStandard standard) {
    const auto rawId = firstString(payload, QStringLiteral("id"));
    if (!rawId) return std::nullopt;
    if (action == QLatin1String("cancelled")) {
        // 取消报文只带 id：构造终止事件，靠 identity 命中并结束生命周期。
        return build(PancakesProtocol::sourceGq(), *rawId, 0.0, 0.0, 0.0, EewParser::kDefaultDepth,
                     QStringLiteral("已取消"), envelopeTime, QString(), 0.0,
                     PancakesProtocol::kCancelReportNum, false, true, envelopeTime,
                     user, standard, PancakesKind::Live);
    }
    const auto latitude = firstDouble(payload, QStringLiteral("latitude"));
    const auto longitude = firstDouble(payload, QStringLiteral("longitude"));
    if (!latitude || !longitude) return std::nullopt;
    const double magnitude = firstDouble(payload, QStringLiteral("magnitude")).value_or(0.0);
    const double depth = firstDouble(payload, QStringLiteral("depth")).value_or(EewParser::kDefaultDepth);
    const QString location = firstString(payload, QStringLiteral("region"))
                                 .value_or(QStringLiteral("未知震源"));
    const long long origin = firstLong(payload, QStringLiteral("originTimeMs")).value_or(envelopeTime);
    const int revision = firstInt(payload, QStringLiteral("revisionId")).value_or(1);
    const auto max = EewParser::parseMaxIntensity(payload, standard);
    return build(PancakesProtocol::sourceGq(), *rawId, magnitude, *latitude, *longitude, depth,
                 location, origin, max.text, max.raw, revision + 1,
                 action == QLatin1String("archived"), false,
                 firstLong(payload, QStringLiteral("lastUpdateMs")).value_or(0),
                 user, standard, PancakesKind::Live);
}

std::optional<PancakesParsed> parseUsgs(const QString& action, const QJsonObject& payload,
                                        long long envelopeTime, const EewParser::UserLocation& user,
                                        IntensityStandard standard) {
    if (!action.isEmpty() && action != QLatin1String("update")) return std::nullopt;
    const auto rawId = firstString(payload, QStringLiteral("eventId"));
    const auto latitude = firstDouble(payload, QStringLiteral("latitude"));
    const auto longitude = firstDouble(payload, QStringLiteral("longitude"));
    if (!rawId || !latitude || !longitude) return std::nullopt;
    const double magnitude = firstDouble(payload, QStringLiteral("magnitude")).value_or(0.0);
    const double depth = firstDouble(payload, QStringLiteral("depth")).value_or(EewParser::kDefaultDepth);
    const QString location = firstString(payload, QStringLiteral("placeName"))
                                 .value_or(QStringLiteral("未知震源"));
    const long long origin = firstLong(payload, QStringLiteral("originTimeMs")).value_or(envelopeTime);
    const QString infoType = firstString(payload, QStringLiteral("infoType")).value_or(QString());
    return build(PancakesProtocol::sourceUsgs(), *rawId, magnitude, *latitude, *longitude, depth,
                 location, origin, QString(), 0.0, 1,
                 infoType == QLatin1String("Reviewed"), false,
                 firstLong(payload, QStringLiteral("updatedTimeMs")).value_or(0),
                 user, standard, PancakesKind::Live);
}

std::optional<PancakesParsed> parseJmaEew(const QJsonObject& payload, long long envelopeTime,
                                          const EewParser::UserLocation& user,
                                          IntensityStandard standard) {
    if (payload.value("isTraining").toBool(false)) return std::nullopt;
    auto rawId = firstString(payload, QStringLiteral("EventID"));
    if (!rawId) rawId = firstString(payload, QStringLiteral("id"));
    auto magnitude = firstDouble(payload, QStringLiteral("Magunitude"));
    if (!magnitude) magnitude = firstDouble(payload, QStringLiteral("Magnitude"));
    const auto latitude = firstDouble(payload, QStringLiteral("Latitude"));
    const auto longitude = firstDouble(payload, QStringLiteral("Longitude"));
    if (!rawId || !magnitude || !latitude || !longitude) return std::nullopt;
    const double depth = firstDouble(payload, QStringLiteral("Depth")).value_or(EewParser::kDefaultDepth);
    const QString location = firstString(payload, QStringLiteral("Hypocenter"))
                                 .value_or(QStringLiteral("未知震源"));
    bool ok = false;
    long long origin = EewParser::parseTime(
        firstString(payload, QStringLiteral("OriginTime")).value_or(QString()), &ok);
    if (!ok || origin == 0) origin = envelopeTime;
    const int serial = firstInt(payload, QStringLiteral("Serial")).value_or(1);
    const bool canceled = payload.value("isCancel").toBool(
        payload.value("isCanceled").toBool(false));
    const auto max = EewParser::parseMaxIntensity(payload, standard);
    return build(PancakesProtocol::sourceJmaEew(), *rawId, *magnitude, *latitude, *longitude, depth,
                 location, origin, max.text, max.raw, serial,
                 payload.value("isFinal").toBool(false), canceled,
                 EewParser::parseTime(firstString(payload, QStringLiteral("AnnouncedTime")).value_or(QString())),
                 user, standard, PancakesKind::Live);
}

std::optional<PancakesParsed> parseJmaEqlist(const QString& action, const QJsonObject& payload,
                                             long long envelopeTime,
                                             const EewParser::UserLocation& user,
                                             IntensityStandard standard) {
    auto rawId = firstString(payload, QStringLiteral("eventId"));
    if (!rawId) rawId = firstString(payload, QStringLiteral("EventID"));
    const auto latitude = firstDouble(payload, QStringLiteral("latitude"));
    const auto longitude = firstDouble(payload, QStringLiteral("longitude"));
    if (!rawId || !latitude || !longitude) return std::nullopt;
    const double magnitude = firstDouble(payload, QStringLiteral("magnitude")).value_or(0.0);
    const double depth = firstDouble(payload, QStringLiteral("depth")).value_or(EewParser::kDefaultDepth);
    const QString location = firstString(payload, QStringLiteral("placeName"))
                                 .value_or(QStringLiteral("未知震源"));
    long long origin = EewParser::parseTime(
        firstString(payload, QStringLiteral("originTime")).value_or(QString()));
    if (origin == 0) origin = envelopeTime;
    const int serial = firstInt(payload, QStringLiteral("serial")).value_or(1);
    const QString infoType = firstString(payload, QStringLiteral("infoType")).value_or(QString());
    const QString status = firstString(payload, QStringLiteral("status")).value_or(QString());
    const bool canceled = action == QLatin1String("cancelled")
        || infoType.contains(QStringLiteral("取消")) || status.contains(QStringLiteral("取消"));
    const QString reportTimeText = firstString(payload, QStringLiteral("reportTime"))
                                       .value_or(firstString(payload, QStringLiteral("announcedTime"))
                                                     .value_or(QString()));
    const auto max = EewParser::parseMaxIntensity(payload, standard);
    return build(PancakesProtocol::sourceJmaEqlist(), *rawId, magnitude, *latitude, *longitude, depth,
                 location, origin, max.text, max.raw, serial, true, canceled,
                 EewParser::parseTime(reportTimeText),
                 user, standard, PancakesKind::Directory);
}

} // namespace

std::optional<PancakesParsed> PancakesParser::parseRealtime(const QJsonObject& envelope,
                                                            const EewParser::UserLocation& user,
                                                            IntensityStandard standard,
                                                            long long nowMs) {
    const QString source = envelope.value("source").toString();
    if (!PancakesProtocol::quakeSources().contains(source)) return std::nullopt;
    if (envelope.value("type").toString() != QLatin1String("earthquake")) return std::nullopt;
    const QString action = envelope.value("action").toString();
    const QJsonObject payload = envelope.value("payload").toObject();
    if (payload.isEmpty()) return std::nullopt;
    const long long envelopeTime =
        static_cast<long long>(envelope.value("timestampMs").toDouble(static_cast<double>(nowMs)));

    if (source == "gq") return parseGq(action, payload, envelopeTime, user, standard);
    if (source == "usgs") return parseUsgs(action, payload, envelopeTime, user, standard);
    if (source == "jma_eew") return parseJmaEew(payload, envelopeTime, user, standard);
    if (source == "jma_eqlist") return parseJmaEqlist(action, payload, envelopeTime, user, standard);
    return std::nullopt;
}

std::optional<EarthquakeEvent> PancakesParser::parseListItem(const QJsonObject& item,
                                                             const EewParser::UserLocation& user,
                                                             IntensityStandard standard,
                                                             long long nowMs) {
    const QString source = item.value("source").toString();
    if (!PancakesProtocol::quakeSources().contains(source)) return std::nullopt;
    const auto rawId = firstString(item, QStringLiteral("eventId"));
    const auto latitude = firstDouble(item, QStringLiteral("latitude"));
    const auto longitude = firstDouble(item, QStringLiteral("longitude"));
    if (!rawId || !latitude || !longitude) return std::nullopt;
    const double magnitude = firstDouble(item, QStringLiteral("magnitude")).value_or(0.0);
    const double depth = firstDouble(item, QStringLiteral("depthKm")).value_or(EewParser::kDefaultDepth);
    const QString location = firstString(item, QStringLiteral("place"))
                                 .value_or(QStringLiteral("未知震源"));
    const long long origin = EewParser::parseTime(
        firstString(item, QStringLiteral("originTime")).value_or(QString()));
    if (origin == 0) return std::nullopt;
    const QString status = firstString(item, QStringLiteral("status")).value_or(QStringLiteral("active"));
    long long updated = firstLong(item, QStringLiteral("revision")).value_or(0);
    if (updated == 0)
        updated = EewParser::parseTime(firstString(item, QStringLiteral("updatedAt")).value_or(QString()));
    const auto max = EewParser::parseMaxIntensity(item, standard);
    return build(source, *rawId, magnitude, *latitude, *longitude, depth, location, origin,
                 max.text, max.raw, 1, status == QLatin1String("archived"),
                 status == QLatin1String("cancelled"), updated,
                 user, standard, PancakesKind::Directory)
        .event;
}

} // namespace komira
