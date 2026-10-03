#include "service/location_service.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <optional>

#include "core/ip_geo_lookup.h"

namespace komira {

LocationService::LocationService(QObject* parent) : QObject(parent) {}

void LocationService::requestCurrentPosition() {
    statusText_ = QStringLiteral("正在定位…");
    emit changed();
    requestIp();
}

void LocationService::setManual(double latitude, double longitude, const QString& label) {
    const QString name = label.isEmpty()
                             ? QStringLiteral("手动定位 · %1, %2").arg(latitude).arg(longitude)
                             : QStringLiteral("手动定位 · %1").arg(label);
    apply(latitude, longitude, name, QStringLiteral("manual"));
}

void LocationService::apply(double latitude, double longitude, const QString& name,
                            const QString& source) {
    latitude_ = latitude;
    longitude_ = longitude;
    locationName_ = name;
    sourceName_ = source;
    hasLocation_ = true;
    statusText_ = name;
    settings_.setValue(QStringLiteral("location/latitude"), latitude_);
    settings_.setValue(QStringLiteral("location/longitude"), longitude_);
    settings_.setValue(QStringLiteral("location/name"), locationName_);
    settings_.setValue(QStringLiteral("location/source"), sourceName_);
    settings_.sync();
    emit changed();
}

void LocationService::restore() {
    if (!settings_.contains(QStringLiteral("location/latitude")) ||
        !settings_.contains(QStringLiteral("location/longitude")))
        return;
    latitude_ = settings_.value(QStringLiteral("location/latitude")).toDouble();
    longitude_ = settings_.value(QStringLiteral("location/longitude")).toDouble();
    locationName_ = settings_.value(QStringLiteral("location/name"), locationName_).toString();
    sourceName_ = settings_.value(QStringLiteral("location/source"), sourceName_).toString();
    statusText_ = locationName_;
    hasLocation_ = true;
    emit changed();
}

void LocationService::requestIp() {
    QNetworkRequest request{QUrl(QStringLiteral("https://api.aloys23.link/api/v1/network/location"))};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("komiraquake/2.0"));
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json"));
    request.setTransferTimeout(5000);
    QNetworkReply* reply = net_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        const auto region = reply->error() == QNetworkReply::NoError
                                ? parseIpGeoResponse(body)
                                : std::nullopt;
        const auto coord = region ? CityCoordTable::instance().resolve(region->province, region->city)
                                  : std::nullopt;
        if (!coord) {
            // 失败不覆盖既有定位，也不落盘。
            statusText_ = QStringLiteral("定位失败");
            emit changed();
            return;
        }
        QString label = region->province + region->city;
        if (label.isEmpty()) label = QStringLiteral("城市级");
        apply(coord->first, coord->second, QStringLiteral("IP 定位 · %1").arg(label),
              QStringLiteral("ipFallback"));
    });
}

} // namespace komira
