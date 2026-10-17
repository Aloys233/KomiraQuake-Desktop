#include "source/snet_station_source.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <algorithm>

#include "source/snet_station_protocol.h"

namespace komira {

SnetStationSource::SnetStationSource(QObject* parent) : QObject(parent) {
    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, &SnetStationSource::connectSocket);

    heartbeatWatch_.setSingleShot(true);
    connect(&heartbeatWatch_, &QTimer::timeout, this, [this]() {
        if (!running_) return;
        // 退休本次尝试，避免随后到达的 disconnected 再次调度重连。
        ++attempt_;
        if (socket_) socket_->abort();
        scheduleReconnect();
    });
}

void SnetStationSource::start() {
    if (running_) return;
    running_ = true;
    ++generation_;
    retryCount_ = 0;
    connectSocket();
}

void SnetStationSource::stop() {
    running_ = false;
    ++generation_;
    ++attempt_;
    reconnectTimer_.stop();
    heartbeatWatch_.stop();
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    if (connected_) {
        connected_ = false;
        emit statusChanged();
    }
}

void SnetStationSource::connectSocket() {
    if (!running_) return;
    const quint64 generation = generation_;
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    socket_ = socket;
    attachHandlers(socket, generation, ++attempt_);
    // /s-net 免鉴权：直接开连，不附加 token、不换票。
    socket->open(QUrl(QString::fromUtf8(SnetStationProtocol::kWsUrl)));
}

void SnetStationSource::attachHandlers(QWebSocket* socket, quint64 generation, quint64 attempt) {
    const auto current = [this, socket, generation, attempt]() {
        return running_ && generation == generation_ && attempt == attempt_ && socket == socket_;
    };

    connect(socket, &QWebSocket::connected, this, [this, current]() {
        if (!current()) return;
        retryCount_ = 0;
        reconnectTimer_.stop();
        heartbeatWatch_.start(SnetStationProtocol::kHeartbeatTimeoutMs);
        if (!connected_) {
            connected_ = true;
            qInfo() << "[s-net] station stream connected";
            emit statusChanged();
        }
    });

    connect(socket, &QWebSocket::disconnected, this, [this, current]() {
        if (!current()) return;
        ++attempt_;
        heartbeatWatch_.stop();
        if (connected_) {
            connected_ = false;
            emit statusChanged();
        }
        scheduleReconnect();
    });

    connect(socket, &QWebSocket::textMessageReceived, this, [this, current](const QString& text) {
        if (current()) handleMessage(text);
    });

    connect(socket, &QWebSocket::errorOccurred, this, [this, current]() {
        if (!current()) return;
        ++attempt_;
        heartbeatWatch_.stop();
        if (connected_) {
            connected_ = false;
            emit statusChanged();
        }
        scheduleReconnect();
    });
}

void SnetStationSource::scheduleReconnect() {
    if (!running_) return;
    // 与其它测站通道同款退避：3s 起，逐步升到 15s。
    const int delayMs = std::min(15, std::max(3, 3 + retryCount_)) * 1000;
    ++retryCount_;
    reconnectTimer_.start(delayMs);
}

void SnetStationSource::handleMessage(const QString& text) {
    if (!running_) return;
    heartbeatWatch_.start(SnetStationProtocol::kHeartbeatTimeoutMs);

    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isObject()) return;
    const QJsonObject obj = doc.object();
    const QString type = obj.value(QStringLiteral("type")).toString();

    // 心跳/探活帧不含业务数据。
    if (type == QLatin1String("heartbeat") || type == QLatin1String("pong")) return;
    // 只处理测站帧；其它 type（含 error）忽略，链路错误由 errorOccurred 处理。
    if (type != QLatin1String("s-net")) return;

    // 首包携带 stations 台站表（含坐标）；后续帧不再携带，需缓存。
    if (const auto parsed = SnetStationParser::parseStations(obj)) {
        stations_ = *parsed;
        qInfo() << "[s-net] stations" << stations_.size();
        emit stationsChanged();
    }
    if (const auto parsed = SnetStationParser::parseIntensity(obj)) {
        intensity_ = *parsed;
        const QString timeKey = obj.value(QStringLiteral("time")).toString();
        if (!timeKey.isEmpty()) frameTime_ = timeKey;
        emit intensityChanged();
    }
}

double SnetStationSource::maxIntensity() const {
    double best = SnetStationProtocol::kMissing;
    for (double value : intensity_) {
        if (value > best) best = value;
    }
    return best;
}

} // namespace komira
