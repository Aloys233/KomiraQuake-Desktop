#include "source/kma_station_source.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <algorithm>
#include <utility>

#include "source/kma_station_protocol.h"

namespace komira {

KmaStationSource::KmaStationSource(QObject* parent) : QObject(parent) {
    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, &KmaStationSource::connectSocket);

    heartbeatWatch_.setSingleShot(true);
    connect(&heartbeatWatch_, &QTimer::timeout, this, [this]() {
        if (!running_) return;
        // 退休本次尝试，避免随后到达的 disconnected 再次调度重连。
        ++attempt_;
        if (socket_) socket_->abort();
        scheduleReconnect();
    });
}

void KmaStationSource::start() {
    if (running_) return;
    running_ = true;
    ++generation_;
    retryCount_ = 0;
    connectSocket();
}

void KmaStationSource::stop() {
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

void KmaStationSource::connectSocket() {
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
    // /kma-station 免鉴权：直接开连，不附加 token、不换票。
    socket->open(QUrl(QString::fromUtf8(KmaStationProtocol::kWsUrl)));
}

void KmaStationSource::attachHandlers(QWebSocket* socket, quint64 generation, quint64 attempt) {
    const auto current = [this, socket, generation, attempt]() {
        return running_ && generation == generation_ && attempt == attempt_ && socket == socket_;
    };

    connect(socket, &QWebSocket::connected, this, [this, current]() {
        if (!current()) return;
        retryCount_ = 0;
        reconnectTimer_.stop();
        heartbeatWatch_.start(KmaStationProtocol::kHeartbeatTimeoutMs);
        if (!connected_) {
            connected_ = true;
            qInfo() << "[kma] station stream connected";
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

void KmaStationSource::scheduleReconnect() {
    if (!running_) return;
    // 与其它测站通道同款退避：3s 起，逐步升到 15s。
    const int delayMs = std::min(15, std::max(3, 3 + retryCount_)) * 1000;
    ++retryCount_;
    reconnectTimer_.start(delayMs);
}

void KmaStationSource::handleMessage(const QString& text) {
    if (!running_) return;
    heartbeatWatch_.start(KmaStationProtocol::kHeartbeatTimeoutMs);

    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isObject()) return;
    const QJsonObject obj = doc.object();
    const QString type = obj.value(QStringLiteral("type")).toString();

    // 心跳/探活帧不含业务数据。
    if (type == QLatin1String("heartbeat") || type == QLatin1String("pong")) return;
    // 只处理测站帧；其它 type（含 error）忽略，链路错误由 errorOccurred 处理。
    if (type != QLatin1String("kma-station")) return;

    // 首包携带 stations 坐标数组；后续帧不再携带，需缓存。
    if (const auto parsed = KmaStationParser::parseStations(obj)) {
        stations_ = *parsed;
        qInfo() << "[kma] stations" << stations_.size();
        emit stationsChanged();
    }
    if (const auto parsed = KmaStationParser::parseMmi(obj)) {
        mmi_ = *parsed;
        const QString timeKey = obj.value(QStringLiteral("time")).toString();
        if (!timeKey.isEmpty()) frameTime_ = timeKey;
        emit mmiChanged();
    }
}

double KmaStationSource::maxMmi() const {
    double best = KmaStationProtocol::kMissing;
    // std::as_const：这些访问器是只读的，非 const 迭代会让 QVector 触发 detach 检查。
    for (double value : std::as_const(mmi_)) {
        if (value > best) best = value;
    }
    return best;
}

} // namespace komira
