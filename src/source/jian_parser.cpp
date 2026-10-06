#include "source/jian_parser.h"

#include <QJsonDocument>
#include <QJsonValue>
#include <QStringList>

#include <cmath>

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

/// 发震时刻：数字按 epoch 毫秒，字符串走 EewParser::parseTime（ISO 带偏移可正确解析）。
long long originTimeMs(const QJsonObject& obj) {
    const QJsonValue v = obj.value(QStringLiteral("originTime"));
    if (v.isDouble()) {
        const double d = v.toDouble();
        if (std::isfinite(d) && d > 0) return static_cast<long long>(d);
    }
    if (v.isString()) return EewParser::parseTime(v.toString());
    return 0;
}

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

/// cenc 的 id 形如 `CD.20260819132221.000_M`（`_M` 正式 / `_A` 自动）；
/// 去掉后缀即与 Wolfx cenc_eqlist 的 EventID 对齐。
QString normalizeUpstreamId(const QString& id) {
    if (id.endsWith(QStringLiteral("_M")) || id.endsWith(QStringLiteral("_A"))) return id.left(id.size() - 2);
    return id;
}

} // namespace

std::optional<EarthquakeEvent> JianParser::parseRecord(const JianChannel& channel,
                                                       const QJsonObject& data,
                                                       const EewParser::UserLocation& user,
                                                       IntensityStandard standard) {
    const auto latitude = firstDouble(data, {"latitude", "Latitude"});
    const auto longitude = firstDouble(data, {"longitude", "Longitude"});
    const QString rawId = firstString(data, {"id", "ID", "EventID"}).value_or(QString());
    if (!latitude || !longitude || rawId.isEmpty()) return std::nullopt;

    const long long origin = originTimeMs(data);
    if (origin <= 0) return std::nullopt;

    const double magnitude = firstDouble(data, {"magnitude", "Magnitude"}).value_or(0.0);
    const double depth = firstDouble(data, {"depth", "Depth"}).value_or(EewParser::kDefaultDepth);
    QString location = firstString(data, {"placeName", "place", "location"}).value_or(QStringLiteral("未知震源"));
    if (location.isEmpty()) location = QStringLiteral("未知震源");

    // 报次：EEW 用 number/serial；速报多为 1。
    const int reportNum = firstInt(data, {"number", "serial", "Serial"}).value_or(1);

    // 最大烈度：JMA 震度文本或 CWA/CENC 数值。
    QString intensityText;
    double intensityRaw = 0.0;
    if (const QJsonValue iv = data.value(QStringLiteral("intensity"));
        !iv.isUndefined() && !iv.isNull()) {
        if (iv.isDouble()) {
            intensityRaw = iv.toDouble();
            intensityText = QString::number(intensityRaw, 'f', 0);
        } else {
            intensityText = iv.toString().trimmed();
            intensityRaw = jmaTextToRaw(intensityText);
        }
    } else if (const auto mv = firstDouble(data, {"maxIntensity"})) {
        intensityRaw = *mv;
        intensityText = QString::number(intensityRaw, 'f', 0);
    }

    bool canceled = data.value(QStringLiteral("isCancel")).toBool(false);
    if (!canceled) {
        const QString infoType = data.value(QStringLiteral("infoType")).toString();
        const QString infoTypeName = data.value(QStringLiteral("infoTypeName")).toString();
        canceled = infoType.contains(QStringLiteral("取消")) || infoTypeName.contains(QStringLiteral("取消"));
    }

    const QString base = channel.eventNs.isEmpty() ? normalizeUpstreamId(rawId) : rawId;
    const QString eventId = channel.eventNs.isEmpty() ? base : channel.eventNs + QLatin1Char(':') + base;

    EarthquakeEvent event;
    event.id = (channel.type + QLatin1Char('_') + base).toStdString();
    event.eventId = eventId.toStdString();
    event.magnitude = magnitude;
    event.latitude = *latitude;
    event.longitude = *longitude;
    event.depth = depth;
    event.location = location.toStdString();
    event.timestamp = origin;
    event.source = channel.type.toStdString();
    event.sourceProvider = JianProtocol::providerName().toStdString();
    event.sourceAgency = channel.agency.toStdString();
    event.maxIntensityText = intensityText.toStdString();
    event.maxIntensityRaw = intensityRaw;
    event.reportNum = reportNum;
    event.isFinal = channel.kind == SourceEventKind::Directory
                        ? true
                        : data.value(QStringLiteral("isFinal")).toBool(false);
    event.isCanceled = canceled;

    EewParser::recompute(event, user, standard);
    if (channel.kind == SourceEventKind::Directory) {
        // 目录永不产生 warning/critical：若有则降级为 watch。
        if (event.warningLevel == WarningLevel::Critical || event.warningLevel == WarningLevel::Warning)
            event.warningLevel = WarningLevel::Watch;
    }
    return event;
}

std::optional<QString> JianParser::parseAuthToken(const QByteArray& body) {
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) return std::nullopt;
    const QJsonObject obj = doc.object();
    if (!obj.value(QStringLiteral("ok")).toBool(false)) return std::nullopt;
    const QString token = obj.value(QStringLiteral("token")).toString();
    if (token.isEmpty()) return std::nullopt;
    return token;
}

} // namespace komira
