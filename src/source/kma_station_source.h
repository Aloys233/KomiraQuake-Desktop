#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWebSocket>

#include "source/kma_station_parser.h"

namespace komira {

/// KMA PEWS 实时测站源（经 Jian Project `/kma-station` 通道，免鉴权）。
///
/// **仅用于地图测站显示，不进入任何 EEW 事件链路**：它产出的不是地震事件，
/// 而是"每站一个韩国 MMI 烈度值"的一帧数据，因此不实现 EarthquakeSource、不进
/// AppController 的事件源注册表，也不参与告警/去重/历史。站点表（首包）与
/// 烈度数组（增量帧）通过信号交给上层，由 AppController 打包给 QML 展示。
class KmaStationSource : public QObject {
    Q_OBJECT
public:
    explicit KmaStationSource(QObject* parent = nullptr);

    void start();
    void stop();

    const QVector<KmaStationInfo>& stations() const { return stations_; }
    const QVector<double>& mmi() const { return mmi_; }

    /// 台站表与烈度数组是否已对齐（可展示）。
    bool hasData() const { return !stations_.isEmpty() && mmi_.size() == stations_.size(); }
    bool connected() const { return connected_; }
    /// 最近一帧的上游时间键（UTC `YYYYMMDDHHMMSS`），无数据时为空。
    QString frameTime() const { return frameTime_; }
    /// 当前最大 MMI；无有效数据返回缺测哨兵值。
    double maxMmi() const;

signals:
    /// 首包台站表到达（含坐标）。
    void stationsChanged();
    /// 每帧烈度数组更新。
    void mmiChanged();
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
    QVector<KmaStationInfo> stations_;
    QVector<double> mmi_;
    QString frameTime_;
    bool connected_ = false;
    bool running_ = false;
    int retryCount_ = 0;
    quint64 generation_ = 0;
    quint64 attempt_ = 0;
};

} // namespace komira
