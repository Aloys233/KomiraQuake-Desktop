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

namespace komira {

/// Jian 数据源（api.sismotide.top，Jian Project）。单条 `/all` 聚合 WebSocket 覆盖全部地震类频道。
///
/// 鉴权：设置页填登录密钥 `lk_…` → 换刷新令牌 `rt_…`（本地持久化）→ 连接前用 `rt_` 换
/// 访问令牌 `at_…`（约 1 小时），以 `?key=at_…` 附加在握手。`at_` 过期（4004）时自动重取。
class JianSource : public EarthquakeSource {
    Q_OBJECT
public:
    explicit JianSource(QObject* parent = nullptr);

    QString id() const override { return SourceIds::kJian; }

    void start() override;
    void stop() override;
    void setUserLocation(double lat, double lon) override;
    void clearUserLocation() override;
    void setStandard(IntensityStandard standard) override { standard_ = standard; }
    void refreshDirectory() override;

    void setNowProvider(std::function<long long()> provider) override { nowProvider_ = std::move(provider); }
    void setMonoProvider(std::function<long long()> provider) override { monoProvider_ = std::move(provider); }

    bool hasLocation() const { return hasLocation_; }
    /// 未登录（无刷新令牌）即视为未配置：上层据此不启动本源，不会建立任何连接。
    bool isConfigured() const override { return !refreshToken_.isEmpty(); }
    DataSourceInfo info() const override { return info_; }

    /// 注入已持久化的刷新令牌；变化时重新鉴权并连接。
    void setRefreshToken(const QString& token);
    /// 用登录密钥换取刷新令牌（结果通过 refreshTokenObtained 回传，由上层持久化）。
    void login(const QString& loginKey);

signals:
    /// 登录成功：携带新的刷新令牌 `rt_…`。
    void refreshTokenObtained(const QString& refreshToken);

private:
    void ensureAccessAndConnect();
    void fetchAccessToken();
    void connectSocket();
    void attachSocketHandlers(QWebSocket* socket, quint64 generation, quint64 socketAttempt);
    void scheduleReconnect(const QString& note = QString());
    void handleMessage(const QString& text);
    void handleRecord(const QString& type, const QJsonObject& data);
    void setStatus(ConnectionStatus status, const QString& note = QString());
    EewParser::UserLocation userLocation() const;
    long long nowMs() const;
    long long monoMs() const;

    QWebSocket* socket_ = nullptr;
    quint64 generation_ = 0;
    quint64 attempt_ = 0;
    QNetworkAccessManager network_;
    QTimer reconnectTimer_;
    QTimer heartbeatWatch_;
    DataSourceInfo info_;
    IntensityStandard standard_ = IntensityStandard::Csis;

    QString refreshToken_;
    QString accessToken_;
    long long accessExpiryMonoMs_ = 0;

    double userLat_ = 0.0;
    double userLon_ = 0.0;
    bool hasLocation_ = false;
    bool running_ = false;
    int retryCount_ = 0;
    std::function<long long()> nowProvider_;
    std::function<long long()> monoProvider_;
};

} // namespace komira
