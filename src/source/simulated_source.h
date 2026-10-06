#pragma once

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

/// 模拟数据源（协议 sim-eew/1，服务端见仓库 `Simulated/`）。**仅供开发与自测。**
///
/// 连接用户在设置页自填的地址（`ws://host:port/ws`），接收合成的 `report` /
/// `directory` 帧。帧内 `type` 决定实时预警还是目录情报。
///
/// 门控：**开发者模式关闭或地址为空即视为未配置**，上层据此不启动本源，
/// 因而不会建立任何连接。模拟源故意不进 `SettingsStore::defaultDisabledSources()`
/// ——那套语义是「需要用户另行获取的凭据」，地址不是凭据。
///
/// 机构固定为 `SIM`：合并键是 `sourceAgency|eventId`，若与真实报文撞键，
/// `EventGate` 会判进同一条目，告警不触发，用户看到真实数据被假数据覆盖。
class SimulatedSource : public EarthquakeSource {
    Q_OBJECT
public:
    explicit SimulatedSource(QObject* parent = nullptr);

    QString id() const override { return SourceIds::kSimulated; }

    void start() override;
    void stop() override;
    void setUserLocation(double lat, double lon) override;
    void clearUserLocation() override;
    void setStandard(IntensityStandard standard) override { standard_ = standard; }
    /// 服务端按自己的节奏推报文，无需主动请求列表。
    void refreshDirectory() override;

    void setNowProvider(std::function<long long()> provider) override { nowProvider_ = std::move(provider); }
    void setMonoProvider(std::function<long long()> provider) override { monoProvider_ = std::move(provider); }

    /// 开发者模式开启且地址非空才视为已配置。
    bool isConfigured() const override { return devMode_ && !url_.isEmpty(); }
    DataSourceInfo info() const override { return info_; }
    /// 仓库层判定后回报最终处置：墓碑 / 重复 / 取消这些在链路上不报错，
    /// 不回报就表现为静默丢弃（对自建的服务端尤其难排查）。
    void onAdmission(const EarthquakeEvent& event, AdmissionStatus status) override;

    /// 注入地址；变化时重连。
    void setUrl(const QString& url);
    /// 注入开发者模式开关。翻开关会改变 isConfigured()，由上层启停逻辑接手重连。
    void setDevMode(bool enabled) { devMode_ = enabled; }
    QString lastUrl() const { return lastUrl_; }

private:
    void connectSocket();
    /// 给某个连接尝试挂事件回调。socketAttempt 为该尝试的令牌：任何终止回调先 ++attempt_
    /// 退休本次尝试，QWebSocket 同时发 errorOccurred/disconnected 时只会调度一次重连。
    void attachSocketHandlers(QWebSocket* socket, quint64 generation, quint64 socketAttempt);
    void scheduleReconnect(const QString& note = QString());
    void handleMessage(const QString& text);
    void handleFrame(const QJsonObject& frame);
    void setStatus(ConnectionStatus status, const QString& note = QString());
    /// 向服务端回报本帧的处置结果。控制台据此把「静默丢弃」变成可见状态。
    void sendAck(const EarthquakeEvent& event, const QString& status);
    EewParser::UserLocation userLocation() const;
    long long nowMs() const;
    long long monoMs() const;

    /// 最近一次发出的帧及其类别，供 onAdmission 匹配回执（乱序时不得张冠李戴）。
    EarthquakeEvent lastEmitted_;
    SourceEventKind lastKind_ = SourceEventKind::Live;

    QWebSocket* socket_ = nullptr;
    quint64 generation_ = 0;
    /// 每次连接尝试递增。过期尝试的迟到回调据此被丢弃，避免重复调度重连。
    quint64 attempt_ = 0;
    QTimer reconnectTimer_;
    QTimer heartbeatWatch_;
    DataSourceInfo info_;
    IntensityStandard standard_ = IntensityStandard::Csis;
    QString lastUrl_;
    QString url_;
    bool devMode_ = false;

    double userLat_ = 0.0;
    double userLon_ = 0.0;
    bool hasLocation_ = false;
    bool running_ = false;
    int retryCount_ = 0;
    std::function<long long()> nowProvider_;
    std::function<long long()> monoProvider_;
};

} // namespace komira
