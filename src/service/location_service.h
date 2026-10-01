#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

namespace komira {

/// 桌面定位：无 GPS，走 IP 回退 + 手动输入。《NATIVE_PORT_SPEC》 §11。
class LocationService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasLocation READ hasLocation NOTIFY changed)
    Q_PROPERTY(double latitude READ latitude NOTIFY changed)
    Q_PROPERTY(double longitude READ longitude NOTIFY changed)
    Q_PROPERTY(QString locationName READ locationName NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)

public:
    explicit LocationService(QObject* parent = nullptr);

    bool hasLocation() const { return hasLocation_; }
    double latitude() const { return latitude_; }
    double longitude() const { return longitude_; }
    QString locationName() const { return locationName_; }
    QString sourceName() const { return sourceName_; }
    QString statusText() const { return statusText_; }

    void requestCurrentPosition();
    void setManual(double latitude, double longitude, const QString& label = QString());

signals:
    void changed();

private:
    void requestIp();
    void apply(double latitude, double longitude, const QString& name, const QString& source);

    QNetworkAccessManager net_;
    bool hasLocation_ = false;
    double latitude_ = 0.0;
    double longitude_ = 0.0;
    QString locationName_ = QStringLiteral("未设置定位");
    QString sourceName_ = QStringLiteral("unknown");
    QString statusText_ = QStringLiteral("未设置定位");
};

} // namespace komira
