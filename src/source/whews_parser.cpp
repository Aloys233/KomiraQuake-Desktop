#include "source/whews_parser.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>

namespace komira {

namespace {

std::optional<double> firstDouble(const QJsonObject& obj, const QStringList& keys) {
    for (const QString& key : keys) {
        const QJsonValue v = obj.value(key);
        if (v.isUndefined() || v.isNull()) continue;
        if (v.isDouble()) return v.toDouble();
        if (v.isString()) {
            bool ok = false;
            const double d = v.toString().toDouble(&ok);
            if (ok) return d;
        }
    }
    return std::nullopt;
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

std::optional<int> firstInt(const QJsonObject& obj, const QStringList& keys) {
    const auto d = firstDouble(obj, keys);
    if (!d) return std::nullopt;
    return static_cast<int>(*d);
}

/// 按频道时区解释无时区墙钟：JMA 系为 UTC+9，其余为 UTC+8。
long long parseWallClock(const QString& raw, WhewsTimeZone tz) {
    return tz == WhewsTimeZone::Jst ? EewParser::parseJstTime(raw) : EewParser::parseUtc8Time(raw);
}

/// cenc 的 id 形如 `CD.20260819132221.000`；若上游带 `_M`（正式）/ `_A`（自动）后缀，
/// 去掉后缀即与 Wolfx cenc_eqlist 的 EventID 对齐。
QString normalizeUpstreamId(const QString& id) {
    if (id.endsWith(QStringLiteral("_M")) || id.endsWith(QStringLiteral("_A")))
        return id.left(id.size() - 2);
    return id;
}

} // namespace

std::optional<EarthquakeEvent> WhewsParser::parseRecord(const WhewsChannel& channel,
                                                       const QJsonObject& data,
                                                       const EewParser::UserLocation& user,
                                                       IntensityStandard standard) {
    const QString rawId = firstString(data, {"id", "ID", "EventID"}).value_or(QString());
    const auto latitude = firstDouble(data, {"latitude", "Latitude"});
    const auto longitude = firstDouble(data, {"longitude", "Longitude"});
    if (rawId.isEmpty() || !latitude || !longitude) return std::nullopt;

    const QString shockTime = firstString(data, {"shockTime", "originTime"}).value_or(QString());
    const long long origin = shockTime.isEmpty() ? 0 : parseWallClock(shockTime, channel.tz);
    if (origin <= 0) return std::nullopt;

    const double magnitude = firstDouble(data, {"magnitude", "Magnitude"}).value_or(0.0);
    const double depth = firstDouble(data, {"depth", "Depth"}).value_or(EewParser::kDefaultDepth);
    QString location = firstString(data, {"placeName", "place", "location"}).value_or(QString());
    if (location.isEmpty()) location = QString::fromUtf8("未知震源");

    // 报次：EEW 用 updates；情报多为 1。
    const int reportNum = firstInt(data, {"updates", "number", "serial"}).value_or(1);

    // 烈度：EEW 用 epiIntensity（数值或 JMA 文本），情报用 maxIntensity / intensity。
    const EewParser::MaxIntensityValue intensity = EewParser::parseMaxIntensity(data, standard);

    // Whews 用 cancel / final（不带 is 前缀），与 Wolfx 的 isCancel / isFinal 不同。
    const bool canceled = data.value(QStringLiteral("cancel")).toBool(false)
        || data.value(QStringLiteral("canceled")).toBool(false);
    const bool finalReport = channel.kind == SourceEventKind::Directory
        || data.value(QStringLiteral("final")).toBool(false)
        || data.value(QStringLiteral("isFinal")).toBool(false);

    const QString base = channel.eventNs.isEmpty() ? normalizeUpstreamId(rawId) : rawId;
    const QString eventId = channel.eventNs.isEmpty() ? base : channel.eventNs + QLatin1Char(':') + base;

    EarthquakeEvent event;
    event.id = (channel.source + QLatin1Char('_') + base).toStdString();
    event.eventId = eventId.toStdString();
    event.magnitude = magnitude;
    event.latitude = *latitude;
    event.longitude = *longitude;
    event.depth = depth;
    event.location = location.toStdString();
    event.timestamp = origin;
    event.source = whewsTitleFor(channel).toStdString();
    event.sourceProvider = WhewsProtocol::providerName().toStdString();
    event.sourceAgency = channel.agency.toStdString();
    event.maxIntensityText = intensity.text.toStdString();
    event.maxIntensityRaw = intensity.raw;
    event.reportNum = reportNum;
    event.isFinal = finalReport;
    event.isCanceled = canceled;

    const QString updatedAt = firstString(data, {"createTime", "updateTime"}).value_or(QString());
    if (!updatedAt.isEmpty()) event.reportTime = parseWallClock(updatedAt, channel.tz);

    EewParser::recompute(event, user, standard);
    if (channel.kind == SourceEventKind::Directory) {
        // 目录永不产生 warning/critical：若有则降级为 watch。
        if (event.warningLevel == WarningLevel::Critical || event.warningLevel == WarningLevel::Warning)
            event.warningLevel = WarningLevel::Watch;
    }
    return event;
}

} // namespace komira