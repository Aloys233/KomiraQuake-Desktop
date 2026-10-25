#pragma once

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QWebSocket>

#include <functional>

#include "core/intensity_calculator.h"
#include "model/data_source_info.h"
#include "model/earthquake_event.h"
#include "source/eew_parser.h"
#include "source/earthquake_source.h"

namespace komira {

/// Wolfx 数据源：WebSocket 预警 + WS/HTTP 双路目录。《NATIVE_PORT_SPEC》 §2。
///
/// 链路分工（2026-10-25 按线上抓包重排）：
/// - **EEW**：只在 `all_eew` 上订阅，每 15 s 重发一轮 `query_*`（串行节流，见下）。
/// - **目录**：`query_cenceqlist` / `query_jmaeqlist` 发一次回一帧**全量** 50 条，
///   之后服务端在内容变化时再推。这是主链路 —— 原来只靠 90 s HTTP 轮询，
///   新地震进列表最坏等 90 s，现在秒级。
/// - **HTTP 兜底**：WS 断开或长时间没收到目录帧时按 5 min 拉一次，保证弱网仍有目录。
class WolfxSource : public EarthquakeSource {
    Q_OBJECT
public:
    explicit WolfxSource(QObject* parent = nullptr);

    QString id() const override { return SourceIds::kWolfx; }

    void start() override;
    void stop() override;
    void setDirectoryPollingEnabled(bool enabled) override;
    void setUserLocation(double lat, double lon) override;
    void clearUserLocation() override;
    void setStandard(IntensityStandard standard) override { standard_ = standard; }
    /// Explicit directory refresh; ignored while disabled. Location changes are local only.
    void refreshDirectory() override;

    /// 校时后的墙钟（epoch ms）：新鲜度判定、pong、心跳展示走它。《NATIVE_PORT_SPEC》 §13。
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

    /// 把一条订阅指令排进发送队列。**查询不能并发**：实测同时发 8 条时上游只回第一条，
    /// 其余静默丢弃；排队后由 `flushQueryQueue()` 按 `kQueryStaggerMs` 串行发出。
    void enqueueQuery(const QString& query);
    void flushQueryQueue();
    /// 一轮 EEW 订阅（6 条），由 `queryTimer_` 每 15 s 触发。
    void sendQueries();
    /// 一轮目录订阅（2 条），连上即发一次，之后每 `kDirectoryQueryIntervalMs` 重发。
    void sendDirectoryQueries();

    /// 收到 `cenc_eqlist` / `jma_eqlist` 整份帧：逐条解析为目录事件。
    /// `fromPush` 区分来源 —— 只有 WS 帧才更新「推送时刻」（HTTP 兜底的跳过判据）。
    void handleDirectoryFrame(const QString& type, const QJsonObject& root, bool fromPush);
    /// 该目录子源是否仍值得走 HTTP 兜底（WS 刚推过就跳过）。
    bool shouldPollDirectory(const QString& subId) const;

    void pollDirectory();
    /// 一个轮询周期内链式拉取第 index 个目录端点；全部结束后聚合目录状态。
    void fetchDirectoryEndpoint(int index);
    void setStatus(ConnectionStatus status, const QString& note = QString());
    EewParser::UserLocation userLocation() const;
    /// 校时后的墙钟；未注入时回退系统时间。
    long long nowMs() const;
    /// 单调耗时；未注入时使用 steady_clock。
    long long monoMs() const;

    QWebSocket* socket_ = nullptr;
    quint64 generation_ = 0;
    quint64 directoryGeneration_ = 0;
    /// 每次连接尝试递增。过期尝试的迟到回调据此被丢弃，避免重复调度重连。
    quint64 attempt_ = 0;
    QNetworkAccessManager network_;
    /// 每 15 s 触发一轮 EEW 订阅。
    QTimer queryTimer_;
    /// 每 30 s 触发一轮目录订阅（服务端不主动推目录，必须周期重问）。
    QTimer directoryQueryTimer_;
    /// 订阅队列的串行节拍：一次发一条。
    QTimer queryStaggerTimer_;
    QTimer pollTimer_;
    QTimer reconnectTimer_;
    DataSourceInfo info_;
    IntensityStandard standard_ = IntensityStandard::Csis;
    QString lastUrl_;

    /// 待发送的订阅指令。队列空时 `queryStaggerTimer_` 停止。
    QStringList pendingQueries_;
    /// 每路目录最近一帧的 `md5`：内容没变就整帧跳过，不再重新解析 50 条。
    QHash<QString, QString> directoryMd5_;
    /// 各路目录最近一次由 WS 收到的时刻（epoch ms，0 = 从未）。
    QHash<QString, long long> directoryPushAt_;

    double userLat_ = 0.0;
    double userLon_ = 0.0;
    bool hasLocation_ = false;
    bool running_ = false;
    bool directoryPollingEnabled_ = true;
    /// 当前目录轮询周期的进度与结果（单一 directoryStatus 汇总全部端点）。
    int directoryIndex_ = 0;
    bool directoryFailed_ = false;
    QString directoryError_;
    long long directoryCycleStart_ = 0;
    int urlIndex_ = 0;
    int retryCount_ = 0;
    std::function<long long()> nowProvider_;
    std::function<long long()> monoProvider_;
};

} // namespace komira
