#pragma once

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWebSocket>

#include "source/wolfx_seis_parser.h"
#include "source/wolfx_seis_protocol.h"

namespace komira {

/// 合并后的测站记录：列表给出位置/坐标，实时帧给出計測震度。
struct WolfxSeisStation {
    QString id;              ///< 测站 UUID
    QString location;        ///< 展示位置（列表 `location`，退化到帧 `region`）
    double latitude = 0.0;
    double longitude = 0.0;
    /// 合成 JMA 計測震度；无实时数据（未上报或已超时）为 kMissingShindo。
    double calcShindo = WolfxSeisProtocol::kMissingShindo;
    /// 实时报文 `Intensity`（中国地震烈度，整数 0–12）；无实时数据为 kMissingIntensity。
    int csis = WolfxSeisProtocol::kMissingIntensity;
    QString updatedAt;       ///< 最近实时帧的 `update_at`
};

/// Wolfx SeisJS 实时测站源（`wss://seisjs.wolfx.jp/all_seis` + `seis_list.json`）。
///
/// **仅用于地图测站显示，不进入任何 EEW 事件链路**：实时帧自带坐标与 JMA 計測震度，
/// 按站约 1 Hz 推送。完整测站名录/展示位置来自官方列表（每 10 分钟拉取一次），
/// 因此没有实时数据的测站也会出现在测站网上（缺测画成深蓝点）。收到 `heartbeat`
/// 时回 `ping`（文档推荐），心跳同时作为链路探活依据。它不实现 EarthquakeSource、
/// 不进 AppController 的事件源注册表，也不参与告警 / 去重 / 历史。
class WolfxSeisSource : public QObject {
    Q_OBJECT
public:
    explicit WolfxSeisSource(QObject* parent = nullptr);

    void start();
    void stop();

    /// 当前测站表快照（列表顺序在前，实时新增的测站追加在后）。
    const QVector<WolfxSeisStation>& stations() const { return stations_; }
    bool hasData() const { return !stations_.isEmpty(); }
    bool connected() const { return connected_; }
    /// 最大合成計測震度；无有效数据返回缺测哨兵值。
    double maxShindo() const;
    /// 最大中国地震烈度（帧 `Intensity`）；无有效数据返回 kMissingIntensity。
    int maxCsis() const;
    /// 最近一帧的上游 `update_at`；无数据返回空串。
    QString frameTime() const { return frameTime_; }

signals:
    /// 测站表变化（新测站加入或名录信息更新）。
    void stationsChanged();
    /// 已有测站的读数变化（含缺测超时回落）。
    void dataChanged();
    /// 连接状态变化。
    void statusChanged();

private:
    void connectSocket();
    void attachHandlers(QWebSocket* socket, quint64 generation, quint64 attempt);
    void scheduleReconnect();
    void handleMessage(const QString& text);
    /// 拉取测站列表并合入测站表（启动时及每 kListRefreshMs 一次）。
    void fetchStationList();
    void applyCatalog(const QVector<WolfxSeisCatalogStation>& catalog);
    /// 把静默超过 kLiveTtlMs 的测站置回缺测。
    void expireStale();

    QWebSocket* socket_ = nullptr;
    QNetworkAccessManager network_;
    QTimer reconnectTimer_;
    QTimer heartbeatWatch_;
    QTimer listTimer_;
    QTimer expiryTimer_;
    QVector<WolfxSeisStation> stations_;
    QHash<QString, int> index_;     ///< 测站 UUID -> stations_ 下标
    QHash<QString, qint64> seen_;   ///< 测站 UUID -> 最近收到实时帧的时刻（本机 ms）
    QString frameTime_;
    bool connected_ = false;
    bool running_ = false;
    int retryCount_ = 0;
    quint64 generation_ = 0;
    quint64 attempt_ = 0;
};

} // namespace komira
