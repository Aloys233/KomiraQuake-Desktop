#pragma once

#include <QByteArray>
#include <QHash>
#include <QPair>
#include <QString>
#include <optional>

namespace komira {

/// IP 定位接口返回的行政区（仅省 + 市；接口不含经纬度）。
struct IpGeoRegion {
    QString province;
    QString city;
};

/// 解析 `https://api.aloys23.link/api/v1/network/location` 的响应，取 `location.province/city`。
/// 无 `location` 字段（非中国大陆 / anycast IP）或非法 JSON 时返回 nullopt。
std::optional<IpGeoRegion> parseIpGeoResponse(const QByteArray& body);

/// 「省 + 市」映射到城市中心坐标（高德 GCJ-02，城市级近似；不转换坐标系）。
/// 资源 `china_cities.json`，结构与 `TravelTimeService` 的单例用法一致。
class CityCoordTable {
public:
    static CityCoordTable& instance();

    bool loadFromFile(const QString& path);
    bool loadFromString(const QByteArray& json);
    bool isLoaded() const { return loaded_; }

    /// 归一化后多级回退：`省市` 精确 → 市名唯一 → 省级中心。命中返回 (lat, lon)。
    std::optional<QPair<double, double>> resolve(const QString& province,
                                                 const QString& city) const;

private:
    static QString normalize(const QString& name, const QStringList& suffixes);
    static QString normalizeProvince(const QString& name);
    static QString normalizeCity(const QString& name);

    QHash<QString, QPair<double, double>> provinces_;
    QHash<QString, QPair<double, double>> cities_;
    QHash<QString, QPair<double, double>> citiesUnique_;
    bool loaded_ = false;
};

} // namespace komira
