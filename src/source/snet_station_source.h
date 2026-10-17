#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWebSocket>

#include "source/snet_station_parser.h"

namespace komira {

/// S-Net 海底地震观测网实时测站源（经 Jian Project `/s-net` 通道，免鉴权）。
///
/// **仅用于地图测站显示，不进入任何 EEW 事件链路**：它产出的不是地震事件，
/// 而是"每站一个连续震度值"的一帧数据，因此不实现 EarthquakeSource、不进
/// AppController 的事件源注册表，也不参与告警/去重/历史。站点表（首包）与
/// 震度数组（增量帧）通过信号交给上层，由 AppController 打包给 QML 展示。
class SnetStationSource : public QObject {
    Q_OBJECT
public:
    explicit SnetStationSource(QObject* parent = nullptr);

    void start();
    void stop();

    const QVector<SnetStationInfo>& stations() const { return stations_; }
    const QVector<double>& intensity() const { return intensity_; }

    /// 台站表与震度数组是否已对齐（可展示）。
    bool hasData() const { return !stations_.isEmpty() && intensity_.size() == stations_.size(); }
    bool connected() const { return connected_; }
    /// 最近一帧的上游时间键（`YYYYMMDDHHMMSS`），无数据时为空。
    QString frameTime() const { return frameTime_; }
    /// 当前最大震度（原始连续值，饱和 99 原样返回）；无有效数据返回缺测哨兵值。
    double maxIntensity() const;

signals:
    /// 首包台站表到达（含坐标）。
    void stationsChanged();
    /// 每帧震度数组更新。
    void intensityChanged();
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
    QVector<SnetStationInfo> stations_;
    QVector<double> intensity_;
    QString frameTime_;
    bool connected_ = false;
    bool running_ = false;
    int retryCount_ = 0;
    quint64 generation_ = 0;
    quint64 attempt_ = 0;
};

} // namespace komira
