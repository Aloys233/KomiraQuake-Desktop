#include "source/jian_parser.h"

#include <QJsonDocument>
#include <QJsonValue>
#include <QStringList>

#include <cmath>

#include "core/region_names.h"

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
/// JMA / KMA / EMSC 等带 `+09:00`、`+08:00` 偏移的报文由字符串分支处理，**不按设备本地时区**。
long long originTimeMs(const QJsonObject& obj) {
    const QJsonValue v = obj.value(QStringLiteral("originTime"));
    if (v.isDouble()) {
        const double d = v.toDouble();
        if (std::isfinite(d) && d > 0) return static_cast<long long>(d);
    }
    if (v.isString()) return EewParser::parseTime(v.toString());
    return 0;
}

/// JMA 震度文本（"1"/"5-"/"5+"/"7"）→ 原始数值。
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

/// 罗马数字烈度（KMA `intensity` 字段形如 `Ⅵ`）→ 数值。KMA PEWS 用的是 MMI，
/// 与 CSIS 恰好同为 0–12 的档位命名，所以直接按档位数换算，不做制式映射。
double romanToRaw(const QString& raw) {
    static const QStringList kRomans = {
        QStringLiteral("0"),   QStringLiteral("I"),    QStringLiteral("II"),  QStringLiteral("III"),
        QStringLiteral("IV"),  QStringLiteral("V"),    QStringLiteral("VI"),  QStringLiteral("VII"),
        QStringLiteral("VIII"), QStringLiteral("IX"),  QStringLiteral("X"),   QStringLiteral("XI"),
        QStringLiteral("XII"),
    };
    const QString s = raw.trimmed().toUpper();
    if (s.isEmpty()) return 0.0;
    const int index = kRomans.indexOf(s);
    if (index > 0) return static_cast<double>(index);
    // 上游下发的是 Unicode 罗马数字单码位（U+2160 'Ⅰ' 起连续 12 个码位 = I…XII）。
    // 全角 `Ⅵ` 在上面的 indexOf 里查不到，必须走这条分支。
    if (s.size() == 1) {
        const ushort code = s.at(0).unicode();
        if (code >= 0x2160 && code <= 0x216B) return static_cast<double>(code - 0x2160 + 1);
    }
    return 0.0;
}

/// cenc 的 id 形如 `CD.20260819132221.000_M`（`_M` 正式 / `_A` 自动）；
/// 去掉后缀即与 Wolfx cenc_eqlist 的 EventID 对齐。
QString normalizeUpstreamId(const QString& id) {
    if (id.endsWith(QStringLiteral("_M")) || id.endsWith(QStringLiteral("_A"))) return id.left(id.size() - 2);
    return id;
}

/// 地名是否已是中文（含汉字）。已是中文就直接用，不要拿 1° 粗格表去覆盖官方地名。
bool hasCjk(const QString& s) {
    for (const QChar& c : s) {
        const ushort u = c.unicode();
        if (u >= 0x4E00 && u <= 0x9FFF) return true;
    }
    return false;
}

bool hasKanaOrHangul(const QString& s) {
    for (const QChar& c : s) {
        const ushort u = c.unicode();
        // 平假名 / 片假名 / 谚文：日文与韩文地名保留原文。
        if ((u >= 0x3040 && u <= 0x30FF) || (u >= 0xAC00 && u <= 0xD7AF)) return true;
    }
    return false;
}

/// 震中地名：优先上游中文补充字段（`placename_zh`），其次保留日/韩原文，
/// 再次按坐标查「震源区」中文名，最后保留上游原文。
/// 与 Whews / Pancakes 的 USGS 分支同源同一张表（`assets/region_names.bin`）。
QString displayLocation(const JianChannel& channel, const QJsonObject& data, double lat, double lon) {
    const QString upstream = firstString(data, {"placeName", "place", "location"}).value_or(QString());
    const QString zh = firstString(data, {"placename_zh"}).value_or(QString());
    if (!zh.isEmpty()) return zh;
    if (upstream.isEmpty()) return QStringLiteral("未知震源");
    // JMA 系（jma / jma-eew）地名是日文官方写法，是最准确的震源区名，不动它。
    if (channel.agency == QLatin1String("JMA")) return upstream;
    if (hasCjk(upstream) || hasKanaOrHangul(upstream)) return upstream;
    // 上游地名是英文/西班牙文/韩文罗马字：按坐标补中文震源区名。
    const std::string region = RegionNameTable::instance().nameAt(lat, lon);
    if (!region.empty()) return QString::fromStdString(region);
    return upstream;
}

/// 源报最大烈度。三族字段并存，必须都认：
/// - JMA 系：`intensity` 是震度文本（`5弱` / `5強`），也可能是 `4` 这样的整数字符串；
/// - KMA 系：`intensity` 是罗马数字（`Ⅵ`），同时给数值 `maxMMI`；
/// - CWA / CENC：`intensity` / `maxIntensity` 是数值。
/// 返回 (展示文本, 数值)。数值为 0 且文本为空表示上游没有报烈度。
struct ParsedIntensity {
    QString text;
    double raw = 0.0;
};

ParsedIntensity parseIntensity(const QJsonObject& data) {
    ParsedIntensity out;
    // KMA 的数值档先取：罗马数字只用于展示，数值才是可比较的量。
    const auto maxMmi = firstDouble(data, {"maxMMI", "maxMmi"});
    const QJsonValue iv = data.value(QStringLiteral("intensity"));
    const bool hasIntensity = !iv.isUndefined() && !iv.isNull();

    if (hasIntensity && iv.isDouble()) {
        out.raw = iv.toDouble();
        out.text = QString::number(out.raw, 'f', 0);
        return out;
    }
    if (hasIntensity && iv.isString()) {
        const QString s = iv.toString().trimmed();
        if (!s.isEmpty() && s != QStringLiteral("null") && s != QStringLiteral("-")) {
            bool ok = false;
            const double numeric = s.toDouble(&ok);
            if (ok) {
                // 纯数字字符串：可能是 JMA 震度的 "4"，也可能是 CSIS 的 "4"。
                out.raw = numeric;
                out.text = s;
            } else {
                const double roman = romanToRaw(s);
                if (roman > 0.0) {
                    // 罗马数字：文本保留原文（与上游一致），数值走 maxMMI 或罗马换算。
                    out.raw = maxMmi.value_or(roman);
                    out.text = s;
                    return out;
                }
                // JMA 震度文本（5弱 / 5強…）
                out.text = s;
                out.raw = jmaTextToRaw(s);
            }
            if (maxMmi && out.raw <= 0.0) out.raw = *maxMmi;
            return out;
        }
    }
    if (const auto mv = firstDouble(data, {"maxIntensity"})) {
        out.raw = *mv;
        out.text = QString::number(out.raw, 'f', 0);
        return out;
    }
    if (maxMmi) {
        out.raw = *maxMmi;
        out.text = QString::number(out.raw, 'f', 0);
    }
    return out;
}

/// 取消报判定：各源表达不一样，`isCancel` / `isCanceled` 布尔、
/// `infoType` / `infoTypeName` 含「取消」、JMA 津波式的 `cancel`（此处地震类不用）。
bool isCanceledRecord(const QJsonObject& data) {
    if (data.value(QStringLiteral("isCancel")).toBool(false)) return true;
    if (data.value(QStringLiteral("isCanceled")).toBool(false)) return true;
    if (data.value(QStringLiteral("cancel")).toBool(false)) return true;
    const QString infoType = data.value(QStringLiteral("infoType")).toString();
    const QString infoTypeName = data.value(QStringLiteral("infoTypeName")).toString();
    return infoType.contains(QStringLiteral("取消")) || infoTypeName.contains(QStringLiteral("取消"));
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
    // 深度 0 是「未知」而不是「正好在地表」：KMA 的 PEWS 快速解常用 0 表示未定深度，
    // 云南/山西台网的无深度条目也是 0。按 10 km 兜底，否则震源正好压在用户脚下时
    // 烈度衰减会算出虚高的近场值。
    double depth = firstDouble(data, {"depth", "Depth"}).value_or(EewParser::kDefaultDepth);
    if (depth <= 0.0) depth = EewParser::kDefaultDepth;

    const QString location = displayLocation(channel, data, *latitude, *longitude);

    // 报次：EEW 用 number/serial；速报多为 1。KMA PEWS 不给报次（`phase` 是相位不是报次），
    // 保持 1 —— 与参考实现「显式 unsequenced，不伪造首报」的口径一致。
    const int reportNum = firstInt(data, {"number", "serial", "Serial"}).value_or(1);

    const ParsedIntensity intensity = parseIntensity(data);
    const bool canceled = isCanceledRecord(data);

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
    event.source = jianTitleFor(channel).toStdString();
    event.sourceProvider = JianProtocol::providerName().toStdString();
    event.sourceAgency = channel.agency.toStdString();
    event.maxIntensityText = intensity.text.toStdString();
    event.maxIntensityRaw = intensity.raw;
    event.reportNum = reportNum;
    event.isFinal = channel.kind == SourceEventKind::Directory
                        ? true
                        : data.value(QStringLiteral("isFinal")).toBool(false);
    event.isCanceled = canceled;
    // 目录/速报通道：只进列表与历史，没有「第 N 报」语义；测定状态（正式/自动）
    // 留给详情页标注，不参与合并。
    event.isInfo = channel.kind == SourceEventKind::Directory;
    if (event.isInfo) event.determination = EewParser::parseDetermination(data).toStdString();
    // 发布时刻：JMA 的 reportTime、USGS / EMSC 的 updateTime 等。解析失败留 0，
    // 展示层退回接收时刻（不要在这里伪造）。
    if (const auto reported = firstString(data, {"reportTime", "gmtPublish", "updateTime",
                                                 "createTime", "ReportTime"})) {
        event.reportTime = EewParser::parseTime(*reported);
    }

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
