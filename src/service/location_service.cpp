#include "service/location_service.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

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
    emit changed();
}

void LocationService::requestIp() {
    QNetworkRequest request{QUrl(QStringLiteral("https://api.fanstudio.tech/tool/geo_ip.php"))};
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("komiraquake/2.0 (+https://api.fanstudio.tech/)"));
    QNetworkReply* reply = net_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            statusText_ = QStringLiteral("定位失败");
            emit changed();
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            statusText_ = QStringLiteral("定位失败");
            emit changed();
            return;
        }
        const QJsonObject obj = doc.object();
        const double lat = obj.value("latitude").toDouble(qQNaN());
        const double lon = obj.value("longitude").toDouble(qQNaN());
        if (qIsNaN(lat) || qIsNaN(lon)) {
            statusText_ = QStringLiteral("定位失败");
            emit changed();
            return;
        }
        const QString region = obj.value("province").toString() +
                               obj.value("city").toString() +
                               obj.value("district").toString();
        const QString name = region.isEmpty() ? QStringLiteral("IP 定位 (城市级)")
                                              : QStringLiteral("IP 定位 · %1").arg(region);
        apply(lat, lon, name, QStringLiteral("ipFallback"));
    });
}

} // namespace komira
