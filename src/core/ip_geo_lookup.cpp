#include "core/ip_geo_lookup.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace komira {

namespace {

// 与 tools/gen_china_cities.py 的归一化规则保持一致。
const QStringList kProvinceSuffixes = {
    QStringLiteral("维吾尔自治区"), QStringLiteral("壮族自治区"),
    QStringLiteral("回族自治区"),   QStringLiteral("特别行政区"),
    QStringLiteral("自治区"),       QStringLiteral("省"),
    QStringLiteral("市")};
const QStringList kCitySuffixes = {
    QStringLiteral("自治州"), QStringLiteral("地区"), QStringLiteral("盟"),
    QStringLiteral("市"),     QStringLiteral("特别行政区")};

std::optional<QPair<double, double>> toCoord(const QJsonValue& value) {
    if (!value.isArray()) return std::nullopt;
    const QJsonArray arr = value.toArray();
    if (arr.size() != 2) return std::nullopt;
    const double lat = arr.at(0).toDouble(qQNaN());
    const double lon = arr.at(1).toDouble(qQNaN());
    if (qIsNaN(lat) || qIsNaN(lon)) return std::nullopt;
    return QPair<double, double>{lat, lon};
}

void readMap(const QJsonObject& root, const QString& key,
             QHash<QString, QPair<double, double>>& out) {
    const QJsonObject map = root.value(key).toObject();
    for (auto it = map.begin(); it != map.end(); ++it) {
        if (auto coord = toCoord(it.value())) out.insert(it.key(), *coord);
    }
}

} // namespace

std::optional<IpGeoRegion> parseIpGeoResponse(const QByteArray& body) {
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) return std::nullopt;
    const QJsonValue location = doc.object().value(QStringLiteral("location"));
    if (!location.isObject()) return std::nullopt; // 非 CN / anycast：无定位信息
    const QJsonObject obj = location.toObject();
    const QString province = obj.value(QStringLiteral("province")).toString().trimmed();
    const QString city = obj.value(QStringLiteral("city")).toString().trimmed();
    if (province.isEmpty() && city.isEmpty()) return std::nullopt;
    return IpGeoRegion{province, city};
}

CityCoordTable& CityCoordTable::instance() {
    static CityCoordTable table;
    return table;
}

QString CityCoordTable::normalize(const QString& name, const QStringList& suffixes) {
    const QString trimmed = name.trimmed();
    for (const QString& suffix : suffixes) {
        if (trimmed.endsWith(suffix) && trimmed.size() > suffix.size())
            return trimmed.left(trimmed.size() - suffix.size());
    }
    return trimmed;
}

QString CityCoordTable::normalizeProvince(const QString& name) {
    return normalize(name, kProvinceSuffixes);
}

QString CityCoordTable::normalizeCity(const QString& name) {
    return normalize(name, kCitySuffixes);
}

bool CityCoordTable::loadFromString(const QByteArray& json) {
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return false;
    const QJsonObject root = doc.object();

    QHash<QString, QPair<double, double>> provinces;
    QHash<QString, QPair<double, double>> cities;
    QHash<QString, QPair<double, double>> citiesUnique;
    readMap(root, QStringLiteral("provinces"), provinces);
    readMap(root, QStringLiteral("cities"), cities);
    readMap(root, QStringLiteral("cities_unique"), citiesUnique);
    if (provinces.isEmpty() && cities.isEmpty()) return false;

    provinces_ = std::move(provinces);
    cities_ = std::move(cities);
    citiesUnique_ = std::move(citiesUnique);
    loaded_ = true;
    return true;
}

bool CityCoordTable::loadFromFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    return loadFromString(file.readAll());
}

std::optional<QPair<double, double>> CityCoordTable::resolve(const QString& province,
                                                             const QString& city) const {
    if (!loaded_) return std::nullopt;
    const QString np = normalizeProvince(province);
    const QString nc = normalizeCity(city);
    if (!nc.isEmpty()) {
        const auto exact = cities_.constFind(np + QLatin1Char('|') + nc);
        if (exact != cities_.constEnd()) return *exact;
        const auto byCity = citiesUnique_.constFind(nc);
        if (byCity != citiesUnique_.constEnd()) return *byCity;
    }
    if (!np.isEmpty()) {
        const auto byProvince = provinces_.constFind(np);
        if (byProvince != provinces_.constEnd()) return *byProvince;
    }
    return std::nullopt;
}

} // namespace komira
