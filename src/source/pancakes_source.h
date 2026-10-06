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
#include "source/earthquake_source.h"
#include "source/pancakes_parser.h"

namespace komira {

/// PancakesAPI 数据源：聚合 WebSocket 实时预警 + 各地震子源的 HTTP 目录轮询。
///
/// 连接 `wss://api.aloys23.link/api/v1/alert/ws/all`，只处理 gq / usgs / jma_eew /
/// jma_eqlist 四类地震事件；气象、海洋与火山事件一律忽略。服务端每 30s 发送 WebSocket
/// Ping，QWebSocket 自动回 Pong，无需业务层心跳或订阅报文。
class PancakesSource : public EarthquakeSource {
    Q_OBJECT
public:
    explicit PancakesSource(QObject* parent = nullptr);

    QString id() const override { return SourceIds::kPancakes; }

    void start() override;
    void stop() override;
    void setUserLocation(double lat, double lon) override;
    void clearUserLocation() override;
    void setStandard(IntensityStandard standard) override { standard_ = standard; }
    /// Explicit directory refresh; ignored while disabled. Location changes are local only.
    void refreshDirectory() override;

    /// 校时后的墙钟（epoch ms）：新鲜度判定与心跳展示走它。
    void setNowProvider(std::function<long long()> provider) override { nowProvider_ = std::move(provider); }
    /// 单调耗时（ms）：目录轮询 RTT 量测走它，不受系统时间影响。
    void setMonoProvider(std::function<long long()> provider) override { monoProvider_ = std::move(provider); }

    bool hasLocation() const { return hasLocation_; }
    DataSourceInfo info() const override { return info_; }

private:
    void connectSocket();
    /// 给某个连接尝试挂事件回调。socketAttempt 为该尝试的令牌：任何终止回调先 ++attempt_
    /// 退休本次尝试，QWebSocket 同时发 errorOccurred/disconnected 时只会调度一次重连。
    void attachSocketHandlers(QWebSocket* socket, quint64 generation, quint64 socketAttempt);
    void scheduleReconnect();
    void handleMessage(const QString& text);
    void handleJsonObject(const QJsonObject& obj);
    void pollDirectory();
    void fetchList(const QString& source, quint64 generation);
    void finishDirectoryRequest();
    void setStatus(ConnectionStatus status, const QString& note = QString());
    EewParser::UserLocation userLocation() const;
    long long nowMs() const;
    long long monoMs() const;

    QWebSocket* socket_ = nullptr;
    quint64 generation_ = 0;
    /// 每次连接尝试递增。过期尝试的迟到回调据此被丢弃，避免重复调度重连。
    quint64 attempt_ = 0;
    QNetworkAccessManager network_;
    QTimer pollTimer_;
    QTimer reconnectTimer_;
    DataSourceInfo info_;
    IntensityStandard standard_ = IntensityStandard::Csis;

    double userLat_ = 0.0;
    double userLon_ = 0.0;
    bool hasLocation_ = false;
    bool running_ = false;
    int retryCount_ = 0;
    /// 本轮目录轮询在途的请求数；归零时结算目录状态。
    int pendingDirectory_ = 0;
    QString directoryError_;
    std::function<long long()> nowProvider_;
    std::function<long long()> monoProvider_;
};

} // namespace komira
