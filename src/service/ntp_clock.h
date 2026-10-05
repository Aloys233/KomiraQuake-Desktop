#pragma once

#include <QHostInfo>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUdpSocket>
#include <QVariantMap>

#include <vector>

#include "core/time_sync.h"

class QNetworkReply;

namespace komira {

/// 网络授时：SNTP v4（UDP/123）为主、HTTP JSON 为备，用单调时钟锚定。
/// 地震时间语义统一走 [now]。《NATIVE_PORT_SPEC》 §13。
class NtpClock : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ isEnabled WRITE setEnabled NOTIFY changed)

public:
    explicit NtpClock(QObject* parent = nullptr);

    /// 校正后的当前时刻（epoch ms）。
    long long now() const { return clock_.now(); }
    /// 单调耗时（ms），不受系统时间影响；用于量测往返/冷却。
    long long elapsedMs() const { return clock_.elapsedMs(); }

    bool isEnabled() const { return enabled_; }
    void setEnabled(bool enabled);

    /// 自定义 SNTP 主机名；留空则仅用内置列表。变更后（启用时）会立即重校一次。
    void setCustomServer(const QString& host);

    /// 供 QML：`{ enabled, state, detail }`。
    Q_INVOKABLE QVariantMap info() const;

    void start();
    void stop();
    /// 立即校准一次（不打断周期调度）。
    Q_INVOKABLE void refresh();

signals:
    void changed();

private:
    void scheduleNext(long long delayMs);
    void beginCalibration();

    void querySntp();
    /// 实际尝试的主机顺序：自定义（若非空）优先，其后为内置列表（去重）。
    QStringList sntpHosts() const;
    void onHostResolved(const QHostInfo& info);
    void onUdpReadyRead();
    void onQueryTimeout();
    void advanceSntp();

    void beginHttp();
    void httpGet();
    void onHttpFinished();
    void onHttpTimeout();

    void finishWith(const ntp::Sample& sample, const QString& label);
    void finishFailed();

    Clock clock_;
    QUdpSocket udp_;
    QNetworkAccessManager network_;
    QTimer scheduleTimer_;
    QTimer queryTimer_;
    QTimer httpTimer_;

    bool enabled_ = true;
    bool running_ = false;
    QString customServer_;

    std::vector<ntp::Sample> samples_;
    int hostIndex_ = 0;
    int attemptIndex_ = 0;
    QString pendingHost_;
    QString lastHost_;
    long long pendingT1_ = 0;

    int httpIndex_ = 0;
    long long httpMonoStart_ = 0;
    long long httpWallStart_ = 0;
    QNetworkReply* httpReply_ = nullptr;
};

} // namespace komira
