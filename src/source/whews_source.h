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

/// Whews 数据源（备用站国内 api.2v8.cn / 主站 api.beecld.com）。单条 `/ws/all` 聚合
/// WebSocket 覆盖全部已接入地震类频道；帧内 `source` 短名决定频道与机构。
///
/// 站点：按 [WhewsProtocol::wsHosts] 顺序**优先直连国内备用站**，仅在该站连接失败时
/// 才轮换到主站；连通后不回落（urlIndex_ 只在失败时前进）。
///
/// 鉴权：设置页粘贴 `wat_…` 令牌（本地持久化），以 `?token=` 附加在握手。无令牌即视为
/// 未配置，不建立任何连接。服务端对无效令牌回 `4401`、封禁回 `4403`，此时按文档要求
/// **停止重连**（重连过频会被智能封禁）。
class WhewsSource : public EarthquakeSource {
    Q_OBJECT
public:
    explicit WhewsSource(QObject* parent = nullptr);

    QString id() const override { return SourceIds::kWhews; }

    void start() override;
    void stop() override;
    void setUserLocation(double lat, double lon) override;
    void clearUserLocation() override;
    void setStandard(IntensityStandard standard) override { standard_ = standard; }
    /// 情报类端点首连即补发近期缓存，无需主动请求列表。
    void refreshDirectory() override;

    void setNowProvider(std::function<long long()> provider) override { nowProvider_ = std::move(provider); }
    void setMonoProvider(std::function<long long()> provider) override { monoProvider_ = std::move(provider); }

    bool hasLocation() const { return hasLocation_; }
    /// 未填令牌即视为未配置：上层据此不启动本源，不会建立任何连接。
    bool isConfigured() const override { return !token_.isEmpty(); }
    DataSourceInfo info() const override { return info_; }

    /// 注入已持久化的令牌；变化时重连。
    void setToken(const QString& token);
    /// 当前已连接的站点描述（设置页展示，便于用户判断走了备用站还是主站）。
    QString lastHost() const { return lastUrl_; }

signals:
    /// 连接成功（或站点轮换）后通知，设置页据此刷新站点文案。
    void lastHostChanged();

private:
    void connectSocket();
    void attachSocketHandlers(QWebSocket* socket, quint64 generation, quint64 socketAttempt);
    /// fatal = 服务端明确拒绝鉴权（4401/4403）：按文档停止重连，避免触发智能封禁。
    void scheduleReconnect(const QString& note = QString(), bool fatal = false);
    void handleMessage(const QString& text);
    void handleFrame(const QJsonObject& frame);
    void setStatus(ConnectionStatus status, const QString& note = QString());
    EewParser::UserLocation userLocation() const;
    long long nowMs() const;
    long long monoMs() const;
    /// `4401` 令牌无效、`4403` 被封禁：文档要求停止盲目重连。
    static bool isFatalClose(QWebSocketProtocol::CloseCode closeCode);

    QWebSocket* socket_ = nullptr;
    quint64 generation_ = 0;
    /// 每次连接尝试递增。过期尝试的迟到回调据此被丢弃，避免重复调度重连。
    quint64 attempt_ = 0;
    /// 站点索引：0 = 国内备用站（优先），失败才前进到主站。
    int urlIndex_ = 0;
    QTimer reconnectTimer_;
    QTimer heartbeatWatch_;
    DataSourceInfo info_;
    IntensityStandard standard_ = IntensityStandard::Csis;
    QString lastUrl_;
    QString token_;

    double userLat_ = 0.0;
    double userLon_ = 0.0;
    bool hasLocation_ = false;
    bool running_ = false;
    int retryCount_ = 0;
    std::function<long long()> nowProvider_;
    std::function<long long()> monoProvider_;
};

} // namespace komira