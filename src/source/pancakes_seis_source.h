#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWebSocket>

#include "source/pancakes_seis_parser.h"

namespace komira {

/// PancakesAPI 实时测站源（`/api/v1/seis/stream`）。
///
/// **仅用于地图测站显示，不进入任何 EEW 事件链路**：它产出的不是地震事件，而是
/// 「每站一个 PGA（Gal）」的逐帧数据，因此不实现 EarthquakeSource、不进
/// AppController 的事件源注册表，也不参与告警 / 去重 / 历史。首包台站表与随后的
/// 逐站加速度帧通过信号交给上层，由 AppController 打包给 QML 展示。
class PancakesSeisSource : public QObject {
    Q_OBJECT
public:
    explicit PancakesSeisSource(QObject* parent = nullptr);

    void start();
    void stop();

    const QVector<PancakesSeisStation>& stations() const { return stations_; }
    /// 逐站 PGA（Gal），与 stations() 同序；尚未收到该站数据时为 kMissingPga。
    const QVector<double>& pga() const { return pga_; }

    /// 台站表是否已到达（可展示）。
    bool hasData() const { return !stations_.isEmpty(); }
    bool connected() const { return connected_; }
    /// 最近一帧的上游毫秒时间戳；无数据返回 0。
    long long lastTimestamp() const { return lastTimestamp_; }
    /// 当前最大 PGA（Gal）；无有效数据返回缺测哨兵值。
    double maxPga() const;

signals:
    /// 首包台站表到达（含坐标）。
    void stationsChanged();
    /// 每帧加速度更新。
    void dataChanged();
    /// 连接状态变化。
    void statusChanged();

private:
    void connectSocket();
    void attachHandlers(QWebSocket* socket, quint64 generation, quint64 attempt);
    void scheduleReconnect();
    void handleMessage(const QString& text);

    QWebSocket* socket_ = nullptr;
    QTimer reconnectTimer_;
    QTimer heartbeatWatch_;
    QVector<PancakesSeisStation> stations_;
    QVector<double> pga_;
    long long lastTimestamp_ = 0;
    bool connected_ = false;
    bool running_ = false;
    int retryCount_ = 0;
    quint64 generation_ = 0;
    quint64 attempt_ = 0;
};

} // namespace komira
