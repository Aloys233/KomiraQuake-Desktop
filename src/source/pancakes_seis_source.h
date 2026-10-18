#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWebSocket>

#include <vector>

#include "source/pancakes_seis_parser.h"
#include "source/pancakes_seis_protocol.h"

namespace komira {

/// PancakesAPI 实时测站源（`/api/v1/seis/stream`）。
///
/// **仅用于地图测站显示，不进入任何 EEW 事件链路**：它产出的不是地震事件，而是
/// 「每站每秒一个仪器烈度」的逐帧数据，因此不实现 EarthquakeSource、不进
/// AppController 的事件源注册表，也不参与告警 / 去重 / 历史。
///
/// 一个连接上会收到两类消息：测站档案（`type:"stations"`，连接建立时立即一次，
/// 此后测站上下线会重发全量）与实时帧（不含 `type`，以 `s` 开头，含设备端算好的
/// 仪器烈度 `i`）。本类按 `sid` 对齐两者，向上一层提供与 stations() 同序的烈度数组。
class PancakesSeisSource : public QObject {
    Q_OBJECT
public:
    explicit PancakesSeisSource(QObject* parent = nullptr);

    void start();
    void stop();

    const QVector<PancakesSeisStation>& stations() const { return stations_; }
    /// 逐站仪器烈度（GB/T 17742-2020），与 stations() 同序；尚未收到该站数据或已
    /// 静默超时时为 kMissingIntensity。**直接使用设备下发值，不做二次换算。**
    const QVector<double>& intensity() const { return intensity_; }

    /// 台站档案是否已到达（可展示）。
    bool hasData() const { return !stations_.isEmpty(); }
    bool connected() const { return connected_; }
    /// 最近一帧的服务端毫秒时间戳；无数据返回 0。
    long long lastTimestamp() const { return lastTimestamp_; }
    /// 当前最大仪器烈度；无有效数据返回缺测哨兵值。
    double maxIntensity() const;

signals:
    /// 测站档案到达或全量刷新（含坐标）。
    void stationsChanged();
    /// 实时帧更新（含静默回落缺测）。
    void dataChanged();
    /// 连接状态变化。
    void statusChanged();

private:
    void connectSocket();
    void attachHandlers(QWebSocket* socket, quint64 generation, quint64 attempt);
    void scheduleReconnect();
    void handleMessage(const QString& text);
    /// 应用测站档案：按 `sid` 重排烈度数组，保留仍在线的旧读数。
    void applyStations(QVector<PancakesSeisStation> stations);
    /// 把静默超过 kLiveTtlMs 的测站置回缺测。
    void expireStale();

    QWebSocket* socket_ = nullptr;
    QTimer reconnectTimer_;
    QTimer heartbeatWatch_;
    QTimer expiryTimer_;
    QVector<PancakesSeisStation> stations_;
    QVector<double> intensity_;
    /// `sid` → stations_ 下标。上游若下发非连续 `sid`，数组下标与 `s` 不再等价。
    QHash<int, int> indexBySid_;
    /// `sid` → 最近一帧的本地时刻（毫秒），用于静默回落。
    QHash<int, qint64> seenAt_;
    long long lastTimestamp_ = 0;
    bool connected_ = false;
    bool running_ = false;
    int retryCount_ = 0;
    quint64 generation_ = 0;
    quint64 attempt_ = 0;
};

} // namespace komira
