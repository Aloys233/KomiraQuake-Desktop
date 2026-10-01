#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QWebSocket>

#include <functional>

#include "core/intensity_calculator.h"
#include "model/data_source_info.h"
#include "model/earthquake_event.h"
#include "source/eew_parser.h"

namespace komira {

/// 事件类别：EEW 实时预警走 WebSocket；地震列表只由 HTTP 目录轮询填充。《NATIVE_PORT_SPEC》 §2。
enum class WolfxEventKind { Eew, Directory };

/// Wolfx 数据源：WebSocket 预警 + CENC 目录 HTTP 轮询。《NATIVE_PORT_SPEC》 §2。
class WolfxSource : public QObject {
    Q_OBJECT
public:
    explicit WolfxSource(QObject* parent = nullptr);

    void start();
    void stop();
    void setUserLocation(double lat, double lon);
    void clearUserLocation();
    void setStandard(IntensityStandard standard) { standard_ = standard; }
    /// Explicit directory refresh; ignored while disabled. Location changes are local only.
    void refreshDirectory();

    /// 校时后的墙钟（epoch ms）：新鲜度判定、pong、心跳展示走它。《NATIVE_PORT_SPEC》 §13。
    void setNowProvider(std::function<long long()> provider) { nowProvider_ = std::move(provider); }
    /// 单调耗时（ms）：目录轮询 RTT 量测走它，不受系统时间影响。
    void setMonoProvider(std::function<long long()> provider) { monoProvider_ = std::move(provider); }

    bool hasLocation() const { return hasLocation_; }
    DataSourceInfo info() const { return info_; }

signals:
    void eventReceived(const komira::EarthquakeEvent& event, WolfxEventKind kind);
    void infoChanged();

private:
    void connectSocket();
    /// 给某个连接尝试挂事件回调。socketAttempt 为该尝试的令牌：任何终止回调先 ++attempt_
    /// 退休本次尝试，QWebSocket 同时发 errorOccurred/disconnected 时只会调度一次重连。
    void attachSocketHandlers(QWebSocket* socket, quint64 generation, quint64 socketAttempt);
    void scheduleReconnect();
    void handleMessage(const QString& text);
    void handleJsonObject(const QJsonObject& obj);
    void sendQueries();
    void pollDirectory();
    void setStatus(ConnectionStatus status, const QString& note = QString());
    EewParser::UserLocation userLocation() const;
    /// 校时后的墙钟；未注入时回退系统时间。
    long long nowMs() const;
    /// 单调耗时；未注入时使用 steady_clock。
    long long monoMs() const;

    QWebSocket* socket_ = nullptr;
    quint64 generation_ = 0;
    /// 每次连接尝试递增。过期尝试的迟到回调据此被丢弃，避免重复调度重连。
    quint64 attempt_ = 0;
    QNetworkAccessManager network_;
    QTimer queryTimer_;
    QTimer pollTimer_;
    QTimer reconnectTimer_;
    DataSourceInfo info_;
    IntensityStandard standard_ = IntensityStandard::Csis;
    QString lastUrl_;

    double userLat_ = 0.0;
    double userLon_ = 0.0;
    bool hasLocation_ = false;
    bool running_ = false;
    int urlIndex_ = 0;
    int retryCount_ = 0;
    std::function<long long()> nowProvider_;
    std::function<long long()> monoProvider_;
};

} // namespace komira
