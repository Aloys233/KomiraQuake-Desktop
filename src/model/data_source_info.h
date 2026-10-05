#pragma once

#include <QMetaType>
#include <QString>

namespace komira {

enum class ConnectionStatus { Connected, Connecting, Disconnected, Error };

/// 数据源链路状态。《NATIVE_PORT_SPEC》 §1.3。
struct DataSourceInfo {
    QString id;
    QString name;
    QString region;
    QString description;
    // Realtime WebSocket health only; HTTP success must never mark it online.
    ConnectionStatus status = ConnectionStatus::Disconnected;
    qint64 latencyMs = -1;
    qint64 lastHeartbeat = 0;
    ConnectionStatus directoryStatus = ConnectionStatus::Disconnected;
    qint64 directoryLatencyMs = -1;
    qint64 directoryLastSuccess = 0;
    QString directoryError;

    QString statusLabel() const {
        switch (status) {
        case ConnectionStatus::Connected: return QStringLiteral("在线");
        case ConnectionStatus::Connecting: return QStringLiteral("连接中");
        case ConnectionStatus::Disconnected: return QStringLiteral("已断开");
        case ConnectionStatus::Error: return QStringLiteral("异常");
        }
        return QStringLiteral("已断开");
    }

    /// 稳定标识，供 UI 着色使用（不受本地化文案影响）。
    QString statusTag() const {
        switch (status) {
        case ConnectionStatus::Connected: return QStringLiteral("CONNECTED");
        case ConnectionStatus::Connecting: return QStringLiteral("CONNECTING");
        case ConnectionStatus::Disconnected: return QStringLiteral("DISCONNECTED");
        case ConnectionStatus::Error: return QStringLiteral("ERROR");
        }
        return QStringLiteral("DISCONNECTED");
    }
};

/// 数据源唯一标识。
namespace SourceIds {
inline const QString kWolfx = QStringLiteral("wolfx");
inline const QString kPancakes = QStringLiteral("pancakes");
}

} // namespace komira
