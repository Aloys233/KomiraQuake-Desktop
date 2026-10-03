#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QSettings>
#include <QString>

namespace komira {

/// 桌面定位：无 GPS，走 IP 回退 + 手动输入。《NATIVE_PORT_SPEC》 §11。
/// 定位结果持久化：手动与 IP 定位都落盘，启动时由 AppController 调 restore() 恢复，
/// 有记录就不再自动 IP，避免覆盖用户基准地。
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
    /// 启动时恢复上次定位（手动或 IP）；有记录则 emit changed()。
    void restore();

signals:
    void changed();

private:
    void requestIp();
    void apply(double latitude, double longitude, const QString& name, const QString& source);

    QNetworkAccessManager net_;
    QSettings settings_;
    bool hasLocation_ = false;
    double latitude_ = 0.0;
    double longitude_ = 0.0;
    QString locationName_ = QStringLiteral("未设置定位");
    QString sourceName_ = QStringLiteral("unknown");
    QString statusText_ = QStringLiteral("未设置定位");
};

} // namespace komira
